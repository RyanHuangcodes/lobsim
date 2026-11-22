#include "calib/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace calib {

namespace {

using json = nlohmann::json;

const char* side_name(int side) { return side == kBuy ? "buy" : "sell"; }

[[noreturn]] void fail(const std::string& path, const std::string& what) {
  throw std::runtime_error(path + ": " + what);
}

std::string idx(const std::string& path, std::size_t i) { return path + "[" + std::to_string(i) + "]"; }

void validate_pmf(const Pmf& p, const std::string& path) {
  if (p.values.size() != p.counts.size()) fail(path, "values and counts differ in length");
  for (std::size_t i = 1; i < p.values.size(); ++i)
    if (p.values[i] <= p.values[i - 1]) fail(idx(path + ".values", i), "not strictly increasing");
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < p.counts.size(); ++i) {
    if (p.counts[i] == 0) fail(idx(path + ".counts", i), "must be positive");
    if (total > std::numeric_limits<std::uint64_t>::max() - p.counts[i])
      fail(idx(path + ".counts", i), "total overflows uint64");
    total += p.counts[i];
  }
}

void validate_rate(const Rate& r, const std::string& path) {
  if (!std::isfinite(r.per_s) || r.per_s < 0.0) fail(path + ".per_s", "must be finite and >= 0");
}

void validate_survival(const Survival& s, const std::string& path) {
  const std::size_t n = s.times_ns.size();
  if (s.survival.size() != n) fail(path + ".survival", "length differs from times_ns");
  for (std::size_t i = 0; i < n; ++i)
    if (s.times_ns[i] <= 0) fail(idx(path + ".times_ns", i), "must be positive");
  for (std::size_t i = 1; i < n; ++i)
    if (s.times_ns[i] <= s.times_ns[i - 1]) fail(idx(path + ".times_ns", i), "not strictly increasing");
  for (std::size_t i = 0; i < n; ++i) {
    const double v = s.survival[i];
    if (!std::isfinite(v)) fail(idx(path + ".survival", i), "not finite");
    if (v < 0.0 || v > 1.0) fail(idx(path + ".survival", i), "must be in [0, 1]");
  }
  for (std::size_t i = 1; i < n; ++i)
    if (s.survival[i] > s.survival[i - 1]) fail(path + ".survival", "not non-increasing");
  if (!(s.at_risk.empty() && s.events.empty()) && (s.at_risk.size() != n || s.events.size() != n))
    fail(path + ".at_risk/events", "must both be empty or match the length of times_ns");
}

// ---- JSON helpers -------------------------------------------------------------------------

[[noreturn]] void bad_type(const std::string& path, const char* want) { fail(path, std::string("expected ") + want); }

const json& need(const json& j, const char* key, const std::string& path) {
  if (!j.is_object()) bad_type(path, "an object");
  auto it = j.find(key);
  if (it == j.end()) fail(path + "." + key, "missing key");
  return *it;
}

// Rejects any key of `j` not in `allowed`.
void only_keys(const json& j, std::initializer_list<const char*> allowed, const std::string& path) {
  if (!j.is_object()) bad_type(path, "an object");
  for (auto it = j.begin(); it != j.end(); ++it) {
    bool ok = false;
    for (const char* a : allowed)
      if (it.key() == a) ok = true;
    if (!ok) fail(path + "." + it.key(), "unknown key");
  }
}

std::int64_t get_i64(const json& j, const std::string& path) {
  if (!j.is_number_integer()) bad_type(path, "an integer");
  if (j.is_number_unsigned() && j.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    fail(path, "out of range");
  return j.get<std::int64_t>();
}
std::uint64_t get_u64(const json& j, const std::string& path) {
  if (!j.is_number_integer() || (j.is_number_integer() && !j.is_number_unsigned() && j.get<std::int64_t>() < 0))
    bad_type(path, "a non-negative integer");
  return j.get<std::uint64_t>();
}
double get_double(const json& j, const std::string& path) {
  if (!j.is_number()) bad_type(path, "a number");
  return j.get<double>();
}
const std::string& get_str(const json& j, const std::string& path) {
  if (!j.is_string()) bad_type(path, "a string");
  return j.get_ref<const std::string&>();
}

template <class T, class F>
std::vector<T> get_vec(const json& j, const std::string& path, F conv) {
  if (!j.is_array()) bad_type(path, "an array");
  std::vector<T> out;
  out.reserve(j.size());
  for (std::size_t i = 0; i < j.size(); ++i) out.push_back(conv(j[i], idx(path, i)));
  return out;
}

json pmf_to_json(const Pmf& p) { return json{{"values", p.values}, {"counts", p.counts}}; }
Pmf pmf_from_json(const json& j, const std::string& path) {
  only_keys(j, {"values", "counts"}, path);
  Pmf p;
  p.values = get_vec<std::int64_t>(need(j, "values", path), path + ".values", get_i64);
  p.counts = get_vec<std::uint64_t>(need(j, "counts", path), path + ".counts", get_u64);
  return p;
}

json rate_to_json(const Rate& r) { return json{{"count", r.count}, {"per_s", r.per_s}}; }
Rate rate_from_json(const json& j, const std::string& path) {
  only_keys(j, {"count", "per_s"}, path);
  Rate r;
  r.count = get_u64(need(j, "count", path), path + ".count");
  r.per_s = get_double(need(j, "per_s", path), path + ".per_s");
  return r;
}

json survival_to_json(const Survival& s) {
  return json{{"times_ns", s.times_ns},
              {"survival", s.survival},
              {"at_risk", s.at_risk},
              {"events", s.events},
              {"orders", s.orders},
              {"cancelled", s.cancelled},
              {"censored_executed", s.censored_executed},
              {"censored_window_end", s.censored_window_end},
              {"censored_left_window", s.censored_left_window}};
}
Survival survival_from_json(const json& j, const std::string& path) {
  only_keys(j, {"times_ns", "survival", "at_risk", "events", "orders", "cancelled", "censored_executed",
                "censored_window_end", "censored_left_window"}, path);
  Survival s;
  s.times_ns = get_vec<std::int64_t>(need(j, "times_ns", path), path + ".times_ns", get_i64);
  s.survival = get_vec<double>(need(j, "survival", path), path + ".survival", get_double);
  s.at_risk = get_vec<std::uint64_t>(need(j, "at_risk", path), path + ".at_risk", get_u64);
  s.events = get_vec<std::uint64_t>(need(j, "events", path), path + ".events", get_u64);
  s.orders = get_u64(need(j, "orders", path), path + ".orders");
  s.cancelled = get_u64(need(j, "cancelled", path), path + ".cancelled");
  s.censored_executed = get_u64(need(j, "censored_executed", path), path + ".censored_executed");
  s.censored_window_end = get_u64(need(j, "censored_window_end", path), path + ".censored_window_end");
  s.censored_left_window = get_u64(need(j, "censored_left_window", path), path + ".censored_left_window");
  return s;
}

json fit_to_json(const std::optional<PlacementFit>& f) {
  if (!f) return nullptr;
  return json{{"delta0", f->delta0}, {"mu", f->mu}, {"samples", f->samples}, {"method", f->method}};
}
std::optional<PlacementFit> fit_from_json(const json& j, const std::string& path) {
  if (j.is_null()) return std::nullopt;
  only_keys(j, {"delta0", "mu", "samples", "method"}, path);
  PlacementFit f;
  f.delta0 = get_double(need(j, "delta0", path), path + ".delta0");
  f.mu = get_double(need(j, "mu", path), path + ".mu");
  f.samples = get_u64(need(j, "samples", path), path + ".samples");
  f.method = get_str(need(j, "method", path), path + ".method");
  return f;
}

// Per-side object {"buy": f(0), "sell": f(1)}.
template <class F>
json per_side(F f) {
  return json{{"buy", f(kBuy)}, {"sell", f(kSell)}};
}
template <class T, class F>
std::array<T, 2> per_side_from(const json& j, const std::string& path, F conv) {
  only_keys(j, {"buy", "sell"}, path);
  std::array<T, 2> out{};
  for (int s = 0; s < 2; ++s) out[s] = conv(need(j, side_name(s), path), path + "." + side_name(s), s);
  return out;
}

}  // namespace

// ---- Pmf, buckets, Survival -----------------------------------------------------------------

std::uint64_t Pmf::total() const {
  std::uint64_t t = 0;
  for (auto c : counts) t += c;
  return t;
}

int spread_bucket(std::int64_t s) {
  if (s < 1) throw std::invalid_argument("spread_bucket: spread must be >= 1 tick");
  return s == 1 ? 0 : (s == 2 ? 1 : 2);
}
const char* spread_bucket_name(int b) {
  switch (b) {
    case 0: return "1";
    case 1: return "2";
    case 2: return "3+";
    default: throw std::invalid_argument("spread_bucket_name: bad bucket");
  }
}
int distance_bucket(std::int64_t d) { return d <= 0 ? 0 : (d <= 2 ? 1 : (d <= 9 ? 2 : 3)); }
const char* distance_bucket_name(int b) {
  switch (b) {
    case 0: return "<=0";
    case 1: return "1-2";
    case 2: return "3-9";
    case 3: return ">=10";
    default: throw std::invalid_argument("distance_bucket_name: bad bucket");
  }
}

std::optional<std::int64_t> Survival::median_ns() const {
  for (std::size_t i = 0; i < survival.size() && i < times_ns.size(); ++i)
    if (survival[i] <= 0.5) return times_ns[i];
  return std::nullopt;
}

// ---- validate -------------------------------------------------------------------------------

void validate(const Calibration& c) {
  if (c.schema_version != kSchemaVersion) fail("schema_version", "must be " + std::to_string(kSchemaVersion));
  if (c.source != "real" && c.source != "synthetic" && c.source != "hand-made")
    fail("source", "must be one of real, synthetic, hand-made");
  if (c.window_from_ns > c.window_to_ns) fail("window_from_ns", "must not exceed window_to_ns");
  if (c.observed_ns < 0) fail("observed_ns", "must be >= 0");
  if (c.tick_units < 1) fail("tick_units", "must be >= 1");
  if (c.levels < 0) fail("levels", "must be >= 0");
  if (c.median_mid_ticks < 1) fail("median_mid_ticks", "must be >= 1");
  for (int s = 0; s < 2; ++s) {
    validate_rate(c.rates.limit[s], std::string("rates.limit[") + side_name(s) + "]");
    validate_rate(c.rates.aggressive[s], std::string("rates.aggressive[") + side_name(s) + "]");
  }
  for (int s = 0; s < 2; ++s) {
    for (int b = 0; b < kSpreadBuckets; ++b) {
      const std::string path = std::string("placement[") + side_name(s) + "][" + spread_bucket_name(b) + "]";
      const Pmf& p = c.placement[s][b];
      validate_pmf(p, path);
      if (b < 2 && !p.values.empty() && p.values.front() < -static_cast<std::int64_t>(b))
        fail(path + ".values[0]", "below -(s-1)");
    }
    if (c.placement_fit[s]) {
      const std::string path = std::string("placement_fit[") + side_name(s) + "]";
      if (!std::isfinite(c.placement_fit[s]->delta0)) fail(path + ".delta0", "not finite");
      if (!std::isfinite(c.placement_fit[s]->mu)) fail(path + ".mu", "not finite");
    }
  }
  validate_pmf(c.limit_size, "limit_size");
  validate_pmf(c.aggressive_size, "aggressive_size");
  for (std::size_t i = 0; i < c.limit_size.values.size(); ++i)
    if (c.limit_size.values[i] < 1) fail(idx("limit_size.values", i), "sizes must be >= 1");
  for (std::size_t i = 0; i < c.aggressive_size.values.size(); ++i)
    if (c.aggressive_size.values[i] < 1) fail(idx("aggressive_size.values", i), "sizes must be >= 1");
  for (int b = 0; b < kDistanceBuckets; ++b)
    validate_survival(c.lifetime[b], std::string("lifetime[") + distance_bucket_name(b) + "]");
}

// ---- JSON -----------------------------------------------------------------------------------

json to_json(const Calibration& c) {
  json j;
  j["schema_version"] = c.schema_version;
  j["source"] = c.source;
  j["ticker"] = c.ticker;
  j["date"] = c.date;
  j["window_from_ns"] = c.window_from_ns;
  j["window_to_ns"] = c.window_to_ns;
  j["observed_ns"] = c.observed_ns;
  j["window_note"] = c.window_note;
  json inputs = json::array();
  for (const auto& in : c.inputs) inputs.push_back(json{{"name", in.name}, {"sha256", in.sha256}});
  j["inputs"] = inputs;
  j["code_commit"] = c.code_commit;
  j["levels"] = c.levels;
  j["tick_units"] = c.tick_units;
  j["median_mid_ticks"] = c.median_mid_ticks;
  j["rates"] = json{{"limit", per_side([&](int s) { return rate_to_json(c.rates.limit[s]); })},
                    {"aggressive", per_side([&](int s) { return rate_to_json(c.rates.aggressive[s]); })}};
  j["placement"] = per_side([&](int s) {
    json o = json::object();
    for (int b = 0; b < kSpreadBuckets; ++b) o[spread_bucket_name(b)] = pmf_to_json(c.placement[s][b]);
    return o;
  });
  j["placement_fit"] = per_side([&](int s) { return fit_to_json(c.placement_fit[s]); });
  j["limit_size"] = pmf_to_json(c.limit_size);
  j["aggressive_size"] = pmf_to_json(c.aggressive_size);
  j["pooled_limit_size"] = c.pooled_limit_size;
  j["pooled_aggressive_size"] = c.pooled_aggressive_size;
  json life = json::object();
  for (int b = 0; b < kDistanceBuckets; ++b) life[distance_bucket_name(b)] = survival_to_json(c.lifetime[b]);
  j["lifetime"] = life;
  j["report"] = c.report;
  return j;
}

Calibration from_json(const json& j) {
  const std::string root = "calibration";
  only_keys(j, {"schema_version", "source", "ticker", "date", "window_from_ns", "window_to_ns", "observed_ns",
                "window_note", "inputs", "code_commit", "levels", "tick_units", "median_mid_ticks", "rates",
                "placement", "placement_fit", "limit_size", "aggressive_size", "pooled_limit_size",
                "pooled_aggressive_size", "lifetime", "report"},
            root);
  Calibration c;
  auto key = [&](const char* k) -> const json& { return need(j, k, root); };
  c.schema_version = static_cast<int>(get_i64(key("schema_version"), "schema_version"));
  c.source = get_str(key("source"), "source");
  c.ticker = get_str(key("ticker"), "ticker");
  c.date = get_str(key("date"), "date");
  c.window_from_ns = get_i64(key("window_from_ns"), "window_from_ns");
  c.window_to_ns = get_i64(key("window_to_ns"), "window_to_ns");
  c.observed_ns = get_i64(key("observed_ns"), "observed_ns");
  c.window_note = get_str(key("window_note"), "window_note");
  c.inputs = get_vec<InputFile>(key("inputs"), "inputs", [](const json& e, const std::string& p) {
    only_keys(e, {"name", "sha256"}, p);
    return InputFile{get_str(need(e, "name", p), p + ".name"), get_str(need(e, "sha256", p), p + ".sha256")};
  });
  c.code_commit = get_str(key("code_commit"), "code_commit");
  const std::int64_t levels = get_i64(key("levels"), "levels");
  if (levels > std::numeric_limits<int>::max() || levels < std::numeric_limits<int>::min()) fail("levels", "out of range");
  c.levels = static_cast<int>(levels);
  c.tick_units = get_i64(key("tick_units"), "tick_units");
  c.median_mid_ticks = get_i64(key("median_mid_ticks"), "median_mid_ticks");

  const json& rates = key("rates");
  only_keys(rates, {"limit", "aggressive"}, "rates");
  auto rate_side = [](const json& o, const std::string& p, int) { return rate_from_json(o, p); };
  c.rates.limit = per_side_from<Rate>(need(rates, "limit", "rates"), "rates.limit", rate_side);
  c.rates.aggressive = per_side_from<Rate>(need(rates, "aggressive", "rates"), "rates.aggressive", rate_side);

  auto placement = per_side_from<std::array<Pmf, kSpreadBuckets>>(
      key("placement"), "placement", [](const json& o, const std::string& p, int) {
        only_keys(o, {"1", "2", "3+"}, p);
        std::array<Pmf, kSpreadBuckets> arr;
        for (int b = 0; b < kSpreadBuckets; ++b) {
          const char* name = spread_bucket_name(b);
          arr[b] = pmf_from_json(need(o, name, p), p + "[" + name + "]");
        }
        return arr;
      });
  c.placement = placement;
  c.placement_fit = per_side_from<std::optional<PlacementFit>>(
      key("placement_fit"), "placement_fit",
      [](const json& o, const std::string& p, int) { return fit_from_json(o, p); });

  c.limit_size = pmf_from_json(key("limit_size"), "limit_size");
  c.aggressive_size = pmf_from_json(key("aggressive_size"), "aggressive_size");
  c.pooled_limit_size = get_u64(key("pooled_limit_size"), "pooled_limit_size");
  c.pooled_aggressive_size = get_u64(key("pooled_aggressive_size"), "pooled_aggressive_size");

  const json& life = key("lifetime");
  only_keys(life, {"<=0", "1-2", "3-9", ">=10"}, "lifetime");
  for (int b = 0; b < kDistanceBuckets; ++b) {
    const char* name = distance_bucket_name(b);
    c.lifetime[b] = survival_from_json(need(life, name, "lifetime"), std::string("lifetime[") + name + "]");
  }
  if (j.contains("report")) {
    if (!j["report"].is_object()) bad_type("report", "an object");
    c.report = j["report"];
  }
  validate(c);
  return c;
}

Calibration load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error(path + ": cannot open calibration file");
  std::ostringstream buf;
  buf << in.rdbuf();
  try {
    return from_json(json::parse(buf.str()));
  } catch (const std::runtime_error& e) {
    throw std::runtime_error(path + ": " + e.what());
  } catch (const std::exception& e) {
    throw std::runtime_error(path + ": " + e.what());
  }
}

void save(const std::string& path, const Calibration& c) {
  validate(c);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error(path + ": cannot open for writing");
  out << to_json(c).dump(2) << "\n";
  out.flush();
  if (!out) throw std::runtime_error(path + ": write failed");
}

// ---- distances ------------------------------------------------------------------------------

double total_variation(const Pmf& p, const Pmf& q) {
  if (p.empty() || q.empty()) throw std::invalid_argument("total_variation: empty Pmf");
  const double tp = static_cast<double>(p.total()), tq = static_cast<double>(q.total());
  double sum = 0.0;
  std::size_t i = 0, k = 0;
  while (i < p.values.size() || k < q.values.size()) {
    double a = 0.0, b = 0.0;
    if (k >= q.values.size() || (i < p.values.size() && p.values[i] < q.values[k])) {
      a = static_cast<double>(p.counts[i++]) / tp;
    } else if (i >= p.values.size() || q.values[k] < p.values[i]) {
      b = static_cast<double>(q.counts[k++]) / tq;
    } else {
      a = static_cast<double>(p.counts[i++]) / tp;
      b = static_cast<double>(q.counts[k++]) / tq;
    }
    sum += std::fabs(a - b);
  }
  return 0.5 * sum;
}

double wasserstein1(const Pmf& p, const Pmf& q) {
  if (p.empty() || q.empty()) throw std::invalid_argument("wasserstein1: empty Pmf");
  const double tp = static_cast<double>(p.total()), tq = static_cast<double>(q.total());
  double sum = 0.0;
  std::uint64_t cp = 0, cq = 0;
  std::size_t i = 0, k = 0;
  while (i < p.values.size() || k < q.values.size()) {
    std::int64_t x;
    if (k >= q.values.size() || (i < p.values.size() && p.values[i] <= q.values[k])) x = p.values[i];
    else x = q.values[k];
    while (i < p.values.size() && p.values[i] == x) cp += p.counts[i++];
    while (k < q.values.size() && q.values[k] == x) cq += q.counts[k++];
    // F_P and F_Q are constant on [x, next support point); the last one has zero difference.
    std::int64_t next = x;
    if (i < p.values.size() && k < q.values.size()) next = std::min(p.values[i], q.values[k]);
    else if (i < p.values.size()) next = p.values[i];
    else if (k < q.values.size()) next = q.values[k];
    const double gap = static_cast<double>(next - x);
    sum += std::fabs(static_cast<double>(cp) / tp - static_cast<double>(cq) / tq) * gap;
  }
  return sum;
}

}  // namespace calib
