#include "calib/validate.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <thread>

#include "calib/sha256.hpp"
#include "replay/lobster.hpp"
#include "sim/agents/calibrated_flow.hpp"
#include "sim/config.hpp"
#include "sim/lobster_export.hpp"
#include "sim/simulator.hpp"

namespace calib {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::int64_t kNs = 1'000'000'000;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The window the exported simulations are measured over starts at the exporter's 9:30 and lasts as long as the
// test period.
constexpr std::int64_t kSimWindowFromNs = sim::kLobsterOpenNs;

// ---- file names, levels, tick --------------------------------------------------------------------

struct NameParts {
  std::string ticker{"DATA"};
  std::string date{"unknown"};
};

// <TICKER>_<DATE>_<start>_<end>_(message|orderbook)_<L>.csv; anything else keeps the placeholders.
NameParts parse_name(const std::string& path) {
  NameParts out;
  std::string name = fs::path(path).filename().string();
  if (name.size() > 4 && name.substr(name.size() - 4) == ".csv") name.resize(name.size() - 4);
  std::vector<std::string> tok;
  std::size_t at = 0;
  for (;;) {
    const std::size_t u = name.find('_', at);
    tok.push_back(name.substr(at, u == std::string::npos ? std::string::npos : u - at));
    if (u == std::string::npos) break;
    at = u + 1;
  }
  if (tok.size() < 6) return out;
  const std::size_t n = tok.size();
  if (tok[n - 2] != "message" && tok[n - 2] != "orderbook") return out;
  std::string ticker;
  for (std::size_t i = 0; i + 5 < n; ++i) ticker += (i ? "_" : "") + tok[i];
  if (ticker.empty()) return out;
  out.ticker = ticker;
  out.date = tok[n - 5];
  return out;
}

int levels_of(const std::string& orderbook_path) {
  std::ifstream f(orderbook_path, std::ios::binary);
  std::string line;
  if (!f || !std::getline(f, line)) throw std::runtime_error("cannot read the first row of " + orderbook_path);
  const auto l = replay::levels_from_columns(replay::count_fields(line));
  if (!l) throw std::runtime_error("the first row of " + orderbook_path + " does not have 4 * L columns");
  return *l;
}

// The auto tick of the estimator's rule (GCD of the type-1 prices of the window; a multiple of 100 gives
// 100), for the case that no estimation is done (a given calibration file). The window is the training
// period, never the test period.
std::int64_t scan_tick(const std::string& messages_path, const Period& window, std::string& warning) {
  std::ifstream f(messages_path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot read " + messages_path);
  std::string line;
  std::int64_t g = 0;
  while (std::getline(f, line)) {
    const auto m = replay::parse_message_line(line);
    if (!m.ok()) continue;
    if (m.value->time_ns >= window.to_ns) break;
    if (m.value->time_ns < window.from_ns || m.value->type != 1) continue;
    g = std::gcd(g, m.value->price);
  }
  if (g <= 0) throw std::runtime_error("no limit order in the training period of " + messages_path + ": cannot choose the tick");
  if (g % 100 == 0) return 100;
  if (g != 1) warning = "the GCD of the limit prices is " + std::to_string(g) + ", not a multiple of 100: tick_units set to it";
  return g;
}

// README_SYNTHETIC.txt beside the message file, or beside the file a symbolic link points to (a folder of links
// has no README of its own). A hard link cannot be resolved; the real-data script refuses what it cannot label.
bool readme_synthetic_beside(const std::string& messages_path) {
  std::error_code ec;
  if (fs::exists(fs::path(messages_path).parent_path() / "README_SYNTHETIC.txt", ec)) return true;
  const fs::path canonical = fs::canonical(messages_path, ec);
  return !ec && fs::exists(canonical.parent_path() / "README_SYNTHETIC.txt", ec);
}

// What the message file covers, against the test period: the first and last message time and the number of
// messages inside [test.from, test.to). Lines that do not parse are skipped (the replay reports them).
struct Coverage {
  std::optional<std::int64_t> first_ns, last_ns;
  std::uint64_t in_test{0};
};

Coverage scan_coverage(const std::string& messages_path, const Period& test) {
  std::ifstream f(messages_path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot read " + messages_path);
  Coverage c;
  std::string line;
  while (std::getline(f, line)) {
    const auto m = replay::parse_message_line(line);
    if (!m.ok()) continue;
    const std::int64_t t = m.value->time_ns;
    if (!c.first_ns || t < *c.first_ns) c.first_ns = t;
    if (!c.last_ns || t > *c.last_ns) c.last_ns = t;
    if (t >= test.from_ns && t < test.to_ns) ++c.in_test;
  }
  return c;
}

std::string seconds_text(std::int64_t ns) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld.%03lld s", static_cast<long long>(ns / kNs), static_cast<long long>(std::llabs(ns % kNs) / 1'000'000));
  return buf;
}

// The test period must be in the data: some message in it, the data not starting later than kCoverageSlackS
// after it starts, and not ending earlier than kCoverageSlackS before it ends.
void check_test_coverage(const Coverage& c, const Period& test) {
  const std::string period = "[" + seconds_text(test.from_ns) + ", " + seconds_text(test.to_ns) + ")";
  if (!c.first_ns || c.in_test == 0) {
    throw TestPeriodError("the data has no message inside the test period " + period + (c.first_ns ? " (it covers " + seconds_text(*c.first_ns) + " to " + seconds_text(*c.last_ns) + ")" : std::string(" (no message at all)")) +
                          ": there is nothing to compare the simulations with");
  }
  const std::int64_t slack = kCoverageSlackS * kNs;
  if (*c.last_ns < test.to_ns - slack) {
    throw TestPeriodError("the data ends at " + seconds_text(*c.last_ns) + ", more than " + std::to_string(kCoverageSlackS) +
                          " s before the test period " + period + " ends: the rest would be scored against a frozen last row");
  }
  if (*c.first_ns > test.from_ns + slack) {
    throw TestPeriodError("the data starts at " + seconds_text(*c.first_ns) + ", more than " + std::to_string(kCoverageSlackS) +
                          " s after the test period " + period + " starts: the start would be scored against an empty book");
  }
}

// ---- configurations ------------------------------------------------------------------------------

json base_config(double duration_s, double warmup_s, std::int64_t start_price) {
  json j;
  j["seed"] = 1;
  j["duration_s"] = duration_s;
  j["warmup_s"] = warmup_s;
  j["max_price"] = std::max<std::int64_t>(1'000'000, 4 * start_price);
  j["initial_price"] = start_price;
  j["tick_display"] = 0.01;
  j["fundamental"] = {{"sigma_ticks_per_sqrt_s", 1.0}, {"dt_s", 0.1}};
  j["markout_horizons_s"] = json::array({1.0});
  j["sample_interval_s"] = 1.0;
  return j;
}

sim::SimConfig calibrated_config(const ValidateOptions& o, const std::string& calibration_path, std::int64_t start_price,
                                 double duration_s) {
  json j = base_config(duration_s, o.warmup_s, start_price);
  // The calibrated flow is the whole background: no noise traders, no informed trader, no market maker.
  j["noise_traders"] = baseline_noise_traders_json();
  j["noise_traders"]["count"] = 0;
  j["background"] = {{"model", "calibrated"},
                     {"file", fs::absolute(calibration_path).string()},
                     {"owner_count", o.owner_count},
                     {"latency_us", {{"entry", 0}, {"response", 0}, {"feed", 0}, {"jitter", 0}}},
                     {"start_price", start_price}};
  return sim::parse_config(j);
}

sim::SimConfig baseline_config(const ValidateOptions& o, std::int64_t start_price, double duration_s) {
  json j = base_config(duration_s, o.warmup_s, start_price);
  j["noise_traders"] = baseline_noise_traders_json();
  return sim::parse_config(j);
}

// ---- one simulation ------------------------------------------------------------------------------

struct SimOutcome {
  Fingerprint fp;
  sim::ExportResult exported;
  bool has_counters{false};
  sim::CalibratedFlowCounters counters;
};

void write_stream(const fs::path& path, std::stringstream& s) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  s.clear();
  s.seekg(0);
  f << s.rdbuf();
  f.flush();
  if (!f) throw std::runtime_error("cannot write " + path.string());
  s.clear();
  s.seekg(0);
}

// Runs the simulator in memory, exports it with the data's L and tick, and fingerprints the exported window.
// `export_dir` non-empty: the two exported files are also written there.
SimOutcome run_one(const sim::SimConfig& cfg, int levels, std::int64_t tick_units, std::int64_t window_ns,
                   std::int64_t warmup_ns, const std::string& export_dir, const std::string& export_ticker) {
  std::stringstream log;
  SimOutcome out;
  {
    sim::Simulator simulator(cfg, sim::SimulatorOptions{&log});
    simulator.run();
    if (const auto bi = simulator.background_index()) {
      out.has_counters = true;
      out.counters = dynamic_cast<const sim::CalibratedFlowAgent&>(simulator.agent(*bi)).counters();
    }
  }
  sim::ExportSpec spec;
  spec.levels = levels;
  spec.filter = true;
  spec.price_scale = tick_units;
  // LOBSTER-like times (the window starts at 9:30), the whole window filtered to the top-L prices, and none of
  // the injected hidden executions or the halt: the fingerprint ignores them (types 5 and 7), and the
  // calibrated flow has no counterpart of them.
  spec.like.on = true;
  spec.like.window_start_ns = warmup_ns;
  spec.like.hidden_rate = 0.0;
  spec.like.halt = false;
  std::stringstream messages, orderbook;
  out.exported = sim::export_order_log(log, spec, messages, orderbook);
  log.str(std::string());
  if (!export_dir.empty()) {
    fs::create_directories(export_dir);
    const std::string stem = export_ticker + "_2012-06-21_34200000_57600000_";
    write_stream(fs::path(export_dir) / (stem + "message_" + std::to_string(levels) + ".csv"), messages);
    write_stream(fs::path(export_dir) / (stem + "orderbook_" + std::to_string(levels) + ".csv"), orderbook);
  }
  FingerprintWindow w;
  w.from_ns = kSimWindowFromNs;
  w.to_ns = kSimWindowFromNs + window_ns;
  out.fp = Prepared::from_streams(messages, orderbook, tick_units, w).fingerprint();
  return out;
}

// ---- reductions ----------------------------------------------------------------------------------

json pmf_probs(const std::vector<const Pmf*>& pmfs) {
  std::map<std::int64_t, double> acc;
  std::size_t n = 0;
  for (const Pmf* p : pmfs) {
    const std::uint64_t total = p->total();
    if (total == 0) continue;
    ++n;
    for (std::size_t i = 0; i < p->values.size(); ++i)
      acc[p->values[i]] += static_cast<double>(p->counts[i]) / static_cast<double>(total);
  }
  json v = json::array(), pr = json::array();
  for (const auto& [value, sum] : acc) {
    v.push_back(value);
    pr.push_back(n ? sum / static_cast<double>(n) : kNaN);
  }
  return json{{"values", v}, {"probs", pr}, {"runs", n}};
}

json survival_json(const Calibration& c) {
  json out = json::array();
  for (int b = 0; b < kDistanceBuckets; ++b) {
    const Survival& s = c.lifetime[static_cast<std::size_t>(b)];
    out.push_back({{"bucket", distance_bucket_name(b)}, {"times_ns", s.times_ns}, {"survival", s.survival},
                   {"orders", s.orders}, {"cancelled", s.cancelled},
                   {"median_ns", s.median_ns() ? json(*s.median_ns()) : json(nullptr)}});
  }
  return out;
}

json counters_json(const sim::CalibratedFlowCounters& k) {
  return json{{"limits_sent", k.limits_sent},         {"markets_sent", k.markets_sent},
              {"placement_redraws", k.placement_redraws}, {"placement_bucket_fallback", k.placement_bucket_fallback}, {"placement_clamped", k.placement_clamped},
              {"fallback_one_sided", k.fallback_one_sided}, {"fallback_empty", k.fallback_empty},
              {"price_clamped", k.price_clamped},     {"cancels_scheduled", k.cancels_scheduled},
              {"no_cancel_draws", k.no_cancel_draws}, {"cancels_sent", k.cancels_sent},
              {"ignored_rejects", k.ignored_rejects}};
}

json run_json(const RunSummary& r) {
  return json{{"n", r.n},       {"mean", r.mean},     {"sd", r.sd},         {"ci_lo", r.ci_lo},
              {"ci_hi", r.ci_hi}, {"run_lo", r.run_lo}, {"run_hi", r.run_hi}};
}

json verdict_json(const Verdict& v) {
  return json{{"kind", to_string(v.kind)}, {"gap", v.gap}, {"ratio", v.ratio}};
}

// Distances of every run to the data: the mean over runs (in seed order) and the range.
json distances_json(const std::vector<DistributionDistances>& ds) {
  const auto summary = [&](auto get, bool w1) {
    double sum = 0.0, lo = kNaN, hi = kNaN;
    std::size_t n = 0;
    for (const auto& d : ds) {
      const Distance x = get(d);
      if (!x.valid) continue;
      const double v = w1 ? x.w1 : x.tv;
      sum += v;
      ++n;
      lo = std::isnan(lo) ? v : std::min(lo, v);
      hi = std::isnan(hi) ? v : std::max(hi, v);
    }
    return json{{"n", n}, {"mean", n ? sum / static_cast<double>(n) : kNaN}, {"min", lo}, {"max", hi}};
  };
  const auto spread = [](const DistributionDistances& d) { return d.spread; };
  const auto buy = [](const DistributionDistances& d) { return d.depth[kBuy]; };
  const auto sell = [](const DistributionDistances& d) { return d.depth[kSell]; };
  json out;
  out["spread"] = {{"w1", summary(spread, true)}, {"tv", summary(spread, false)}};
  out["depth_buy"] = {{"w1", summary(buy, true)}, {"tv", summary(buy, false)}};
  out["depth_sell"] = {{"w1", summary(sell, true)}, {"tv", summary(sell, false)}};
  return out;
}

}  // namespace

// ---- public pieces -------------------------------------------------------------------------------

Period parse_period(const std::string& text, const std::string& what) {
  const std::size_t dash = text.find('-');
  const auto parse = [&](const std::string& s, std::int64_t& v) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return !s.empty() && ec == std::errc{} && p == s.data() + s.size();
  };
  std::int64_t a = 0, b = 0;
  if (dash == std::string::npos || !parse(text.substr(0, dash), a) || !parse(text.substr(dash + 1), b) || a < 0 ||
      b > 86'400 || a >= b) {
    throw std::invalid_argument(what + " must be A-B in whole seconds after midnight, 0 <= A < B <= 86400 (got '" +
                                text + "')");
  }
  return Period{a * kNs, b * kNs};
}

void check_period_order(const Period& train, const Period& test) {
  if (train.to_ns > test.from_ns) {
    throw std::invalid_argument("--train " + std::to_string(train.from_ns / kNs) + "-" + std::to_string(train.to_ns / kNs) +
                                " must end at or before --test " + std::to_string(test.from_ns / kNs) + "-" +
                                std::to_string(test.to_ns / kNs) + " starts: the test period may not overlap or precede the training period");
  }
}

void check_estimator_range(std::optional<std::int64_t> first_ns, std::optional<std::int64_t> last_ns, const Period& train,
                           const Period& test) {
  if (!first_ns || !last_ns) {
    throw LeakageError("leakage check: the estimator reported no observed message range, so it cannot be shown that it stayed out of the test period");
  }
  const auto sec = [](std::int64_t ns) { return std::to_string(ns / kNs) + "." + std::to_string(std::abs(ns % kNs) + kNs).substr(1) + " s"; };
  if (*first_ns < train.from_ns || *last_ns >= train.to_ns) {
    throw LeakageError("leakage check: the estimator read messages from " + sec(*first_ns) + " to " + sec(*last_ns) +
                       ", which is not inside the training period [" + sec(train.from_ns) + ", " + sec(train.to_ns) + ")");
  }
  // Both ends are inclusive times of messages: the observed range is [first, last].
  if (*first_ns < test.to_ns && *last_ns >= test.from_ns) {
    throw LeakageError("leakage check: the estimator read messages from " + sec(*first_ns) + " to " + sec(*last_ns) +
                       ", which overlaps the test period [" + sec(test.from_ns) + ", " + sec(test.to_ns) + ")");
  }
}

json baseline_noise_traders_json() {
  return json{{"count", 10},
              {"lambda", 5.0},
              {"p_limit", 0.85},
              {"order_lifetime_mean_s", 30.0},
              {"D", 10},
              {"max_size", 10},
              {"reference_price", "symmetric"},
              {"latency_us", {{"entry", 200}, {"response", 200}, {"feed", 200}, {"jitter", 50}}}};
}

const char* to_string(VerdictKind k) {
  switch (k) {
    case VerdictKind::Match: return "match";
    case VerdictKind::Close: return "close";
    case VerdictKind::Miss: return "miss";
    case VerdictKind::Undefined: return "n/a";
  }
  return "n/a";
}

Verdict judge(double data_value, double data_lo, double data_hi, double sim_mean, double sim_lo, double sim_hi) {
  Verdict v;
  for (double x : {data_value, data_lo, data_hi, sim_mean, sim_lo, sim_hi})
    if (!std::isfinite(x)) return v;  // Undefined
  const double gap = std::max(sim_lo - data_hi, data_lo - sim_hi);
  if (gap <= 0.0) {
    v.kind = VerdictKind::Match;
    return v;
  }
  v.gap = gap;
  const double half_width = (data_hi - data_lo) / 2.0;
  if (gap <= 2.0 * half_width) {
    v.kind = VerdictKind::Close;
    return v;
  }
  v.kind = VerdictKind::Miss;
  v.ratio = data_value != 0.0 ? sim_mean / data_value : kNaN;
  return v;
}

// ---- the flow --------------------------------------------------------------------------------------

ValidateResult run_validation(const ValidateOptions& o, const EstimateFn& estimate, const ProgressFn& progress) {
  const auto say = [&](const std::string& s) {
    if (progress) progress(s);
  };
  if (o.seeds < 2) throw std::invalid_argument("--seeds must be at least 2 (an interval across seeds needs two runs)");
  if (o.warmup_s < 0.0 || !std::isfinite(o.warmup_s)) throw std::invalid_argument("--warmup-s must be >= 0");
  if (o.owner_count < 1) throw std::invalid_argument("owner_count must be >= 1");
  if (o.train.length_ns() <= 0 || o.test.length_ns() <= 0) throw std::invalid_argument("empty --train or --test period");
  check_period_order(o.train, o.test);
  if (o.messages.empty() || o.orderbook.empty()) throw std::invalid_argument("--messages and --orderbook are required");
  if (o.tick_units && *o.tick_units < 1) throw std::invalid_argument("--tick-units must be >= 1");
  for (const std::string& p : {o.messages, o.orderbook}) {
    if (!fs::exists(p)) throw DataMissingError("file not found: " + p);
  }
  if (o.calibration_file.empty() && !estimate) throw std::invalid_argument("no estimator available and no --calibration file");

  check_test_coverage(scan_coverage(o.messages, o.test), o.test);

  const NameParts names = parse_name(o.messages);
  const std::string out_dir =
      !o.out_dir.empty() ? o.out_dir : (fs::path("build") / "validation" / (names.ticker + "_" + names.date)).string();
  fs::create_directories(out_dir);
  const int levels = levels_of(o.orderbook);
  std::vector<std::string> notes;

  // ---- step 1: the calibration (training period only)
  ValidateResult result;
  result.out_dir = out_dir;
  Calibration cal;
  json leakage;
  json file_check = nullptr;
  const std::string stem = names.ticker + "_" + names.date + "_" + std::to_string(o.train.from_ns / kNs) + "-" +
                           std::to_string(o.train.to_ns / kNs);
  if (o.calibration_file.empty()) {
    say("estimating on the training period " + std::to_string(o.train.from_ns / kNs) + "-" + std::to_string(o.train.to_ns / kNs) + " s");
    const EstimatorOutput est = estimate(o.messages, o.orderbook, o.train.from_ns, o.train.to_ns, o.tick_units);
    check_estimator_range(est.first_message_ns, est.last_message_ns, o.train, o.test);
    cal = est.calibration;
    result.calibration_path = (fs::path(out_dir) / (stem + ".json")).string();
    save(result.calibration_path, cal);
    if (!est.summary_markdown.empty()) {
      std::ofstream md(fs::path(out_dir) / (stem + ".md"), std::ios::binary | std::ios::trunc);
      md << est.summary_markdown;
    }
    leakage = {{"checked", true},
               {"passed", true},
               {"first_message_ns", *est.first_message_ns},
               {"last_message_ns", *est.last_message_ns},
               {"note", "the estimator read messages only inside the training period and none inside the test period"}};
  } else {
    cal = load(o.calibration_file);
    // The file may have been estimated from this very data's test period: the same message file (SHA-256) or the
    // same ticker and date, with an estimation window that overlaps the test period, is leakage.
    const std::string data_sha = sha256_file(o.messages);
    bool same_sha = false;
    for (const InputFile& in : cal.inputs) same_sha = same_sha || in.sha256 == data_sha;
    const bool same_id = cal.ticker == names.ticker && cal.date == names.date;
    const bool overlaps = cal.window_to_ns > cal.window_from_ns && cal.window_from_ns < o.test.to_ns && cal.window_to_ns > o.test.from_ns;
    if ((same_sha || same_id) && overlaps) {
      throw LeakageError("leakage check: the calibration file " + fs::path(o.calibration_file).filename().string() + " was estimated from " +
                         (same_sha ? "this message file (same SHA-256)" : "the same ticker and date (" + cal.ticker + " " + cal.date + ")") +
                         " over [" + seconds_text(cal.window_from_ns) + ", " + seconds_text(cal.window_to_ns) + "), which overlaps the test period [" +
                         seconds_text(o.test.from_ns) + ", " + seconds_text(o.test.to_ns) + ")");
    }
    file_check = {{"same_message_file_sha256", same_sha}, {"same_ticker_and_date", same_id}, {"window_overlaps_test_period", overlaps}};
    result.calibration_path = (fs::path(out_dir) / fs::path(o.calibration_file).filename()).string();
    if (fs::absolute(o.calibration_file) != fs::absolute(result.calibration_path)) {
      save(result.calibration_path, cal);
    }
    leakage = {{"checked", false},
               {"passed", nullptr},
               {"calibration_file_check", file_check},
               {"note", "a calibration file was given (--calibration): no estimation was done on this data, so there is no estimator range to check; "
                        "the file's data identity (message-file SHA-256, ticker, date) and estimation window were compared with the test period instead"}};
    notes.push_back("The calibration was read from " + fs::path(o.calibration_file).filename().string() + " (source: " + cal.source + ", ticker " +
                    cal.ticker + ", date " + cal.date + ", estimation window " + seconds_text(cal.window_from_ns) + " to " + seconds_text(cal.window_to_ns) +
                    "). It was not estimated by this run; it does not come from this message file or from this ticker and date over a window overlapping the test period.");
  }
  result.calibration = cal;
  // Placement buckets with no samples in the calibration: the simulator draws from the nearest non-empty bucket
  // of the same side (D-385).
  json empty_buckets = json::array();
  for (int s = 0; s < 2; ++s)
    for (int b = 0; b < kSpreadBuckets; ++b)
      if (cal.placement[static_cast<std::size_t>(s)][static_cast<std::size_t>(b)].empty())
        empty_buckets.push_back(std::string(s == kBuy ? "buy" : "sell") + ":" + spread_bucket_name(b));
  if (!empty_buckets.empty())
    notes.push_back("The calibration has no placement samples for " + empty_buckets.dump() +
                    " (spread bucket and side); those draws use the nearest non-empty bucket of the same side.");

  // ---- tick, start price, labels
  std::int64_t tick = 0;
  if (o.tick_units) {
    tick = *o.tick_units;
  } else if (o.calibration_file.empty()) {
    tick = cal.tick_units;
  } else {
    std::string warning;
    tick = scan_tick(o.messages, o.train, warning);
    if (!warning.empty()) notes.push_back(warning);
  }
  if (tick < 1) throw std::runtime_error("the tick is " + std::to_string(tick));
  const std::int64_t start_price = o.start_price ? *o.start_price : cal.median_mid_ticks;
  if (start_price < 1) throw std::runtime_error("the start price is " + std::to_string(start_price) + " ticks: pass --start-price");
  const bool data_synthetic = readme_synthetic_beside(o.messages);
  std::string label;  // what every page says the numbers are
  if (data_synthetic || cal.source == "synthetic") label = "SYNTHETIC";
  else if (cal.source == "real") label = "REAL";
  else label = "HAND-MADE CALIBRATION";

  // ---- step 4 (data): fingerprint of the test period, with its bootstrap and the halves yardstick
  say("fingerprint of the test period of the data");
  FingerprintWindow dw;
  dw.from_ns = o.test.from_ns;
  dw.to_ns = o.test.to_ns;
  const Prepared data_prep = Prepared::from_files(o.messages, o.orderbook, tick, dw);
  const Fingerprint data_fp = data_prep.fingerprint();
  BootstrapOptions bo;
  bo.resamples = o.bootstrap_resamples;
  const BootstrapResult data_boot = data_prep.bootstrap(bo);
  const HalvesYardstick halves = data_prep.halves(bo);
  if (data_fp.levels != levels) throw std::runtime_error("the data's fingerprint has L = " + std::to_string(data_fp.levels));

  // ---- steps 2 and 3: the simulations
  const double test_s = static_cast<double>(o.test.length_ns()) / 1e9;
  const double duration_s = o.warmup_s + test_s;
  const std::int64_t warmup_ns = static_cast<std::int64_t>(std::llround(o.warmup_s * 1e9));
  const sim::SimConfig cal_cfg = calibrated_config(o, result.calibration_path, start_price, duration_s);
  const sim::SimConfig base_cfg = baseline_config(o, start_price, duration_s);
  const fs::path export_root = fs::path(out_dir) / "exports";
  const bool want_survival = static_cast<bool>(estimate);

  const std::size_t seeds = static_cast<std::size_t>(o.seeds);
  std::vector<SimOutcome> cal_out(seeds), base_out(seeds);
  std::vector<std::exception_ptr> errors(2 * seeds);
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> done{0};
  const auto work = [&]() {
    for (;;) {
      const std::size_t task = next.fetch_add(1);
      if (task >= 2 * seeds) return;
      const bool calibrated = task < seeds;
      const std::size_t i = calibrated ? task : task - seeds;
      try {
        sim::SimConfig cfg = calibrated ? cal_cfg : base_cfg;
        cfg.seed = o.first_seed + i;
        sim::finalize(cfg);
        std::string dir;
        if (i == 0 && (want_survival || o.keep_exports)) dir = (export_root / (calibrated ? "calibrated" : "baseline")).string();
        SimOutcome r = run_one(cfg, levels, tick, o.test.length_ns(), warmup_ns, dir, calibrated ? "SIMCAL" : "SIMBASE");
        (calibrated ? cal_out : base_out)[i] = std::move(r);
      } catch (...) {
        errors[task] = std::current_exception();
      }
      const std::size_t n = done.fetch_add(1) + 1;
      if (progress && (n % std::max<std::size_t>(1, seeds / 4) == 0 || n == 2 * seeds))
        say("simulated " + std::to_string(n) + " of " + std::to_string(2 * seeds) + " runs");
    }
  };
  int threads = o.threads > 0 ? o.threads : static_cast<int>(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
  threads = std::min<int>(threads, static_cast<int>(2 * seeds));
  say("running " + std::to_string(seeds) + " calibrated and " + std::to_string(seeds) + " baseline simulations of " +
      std::to_string(duration_s) + " s on " + std::to_string(threads) + " thread(s)");
  {
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(work);
    for (auto& th : pool) th.join();
  }
  for (const auto& e : errors)
    if (e) std::rethrow_exception(e);  // the first failing task in task order

  // ---- reductions, always in seed order
  std::vector<Fingerprint> cal_fps, base_fps;
  std::vector<DistributionDistances> cal_dist, base_dist;
  sim::CalibratedFlowCounters totals;
  json per_seed_cal = json::array(), per_seed_base = json::array();
  for (std::size_t i = 0; i < seeds; ++i) {
    cal_fps.push_back(cal_out[i].fp);
    base_fps.push_back(base_out[i].fp);
    cal_dist.push_back(distances(data_fp, cal_out[i].fp));
    base_dist.push_back(distances(data_fp, base_out[i].fp));
    const sim::CalibratedFlowCounters& k = cal_out[i].counters;
    totals.limits_sent += k.limits_sent;
    totals.markets_sent += k.markets_sent;
    totals.placement_redraws += k.placement_redraws;
    totals.placement_clamped += k.placement_clamped;
    totals.placement_bucket_fallback += k.placement_bucket_fallback;
    totals.fallback_one_sided += k.fallback_one_sided;
    totals.fallback_empty += k.fallback_empty;
    totals.price_clamped += k.price_clamped;
    totals.cancels_scheduled += k.cancels_scheduled;
    totals.no_cancel_draws += k.no_cancel_draws;
    totals.cancels_sent += k.cancels_sent;
    totals.ignored_rejects += k.ignored_rejects;
    const auto seed_row = [&](const SimOutcome& r, std::uint64_t seed) {
      return json{{"seed", seed}, {"requests", r.exported.requests}, {"messages_written", r.exported.written},
                  {"messages_filtered", r.exported.dropped}, {"messages_in_window", r.fp.messages}};
    };
    per_seed_cal.push_back(seed_row(cal_out[i], o.first_seed + i));
    per_seed_base.push_back(seed_row(base_out[i], o.first_seed + i));
  }
  const std::vector<RunSummary> cal_runs = combine_runs(cal_fps);
  const std::vector<RunSummary> base_runs = combine_runs(base_fps);

  // ---- the scorecard
  json scorecard = json::array();
  std::map<std::string, int> counts_cal, counts_base;
  for (std::size_t i = 0; i < data_boot.scalars.size(); ++i) {
    const ScalarInterval& d = data_boot.scalars[i];
    if (d.name != cal_runs[i].name || d.name != base_runs[i].name)
      throw std::logic_error("scalar order differs between the data and the simulations at " + d.name);
    const Verdict vc = judge(d.value, d.lo, d.hi, cal_runs[i].mean, cal_runs[i].ci_lo, cal_runs[i].ci_hi);
    const Verdict vb = judge(d.value, d.lo, d.hi, base_runs[i].mean, base_runs[i].ci_lo, base_runs[i].ci_hi);
    ++counts_cal[to_string(vc.kind)];
    ++counts_base[to_string(vb.kind)];
    json row;
    row["name"] = d.name;
    row["data"] = {{"value", d.value}, {"lo", d.lo}, {"hi", d.hi}, {"se", d.se}, {"defined", d.defined}};
    row["calibrated"] = run_json(cal_runs[i]);
    row["baseline"] = run_json(base_runs[i]);
    row["verdict"] = verdict_json(vc);
    row["baseline_verdict"] = verdict_json(vb);
    scorecard.push_back(std::move(row));
  }
  const auto counts_json = [](const std::map<std::string, int>& m) {
    json j = {{"match", 0}, {"close", 0}, {"miss", 0}, {"n/a", 0}};
    for (const auto& [k, v] : m) j[k] = v;
    return j;
  };

  // ---- distributions against the halves yardstick
  json dists;
  dists["calibrated"] = distances_json(cal_dist);
  dists["baseline"] = distances_json(base_dist);
  dists["yardstick"] = to_json(halves);
  {
    std::vector<const Pmf*> cp, bp;
    for (const Fingerprint& f : cal_fps) cp.push_back(&f.spread_pmf);
    for (const Fingerprint& f : base_fps) bp.push_back(&f.spread_pmf);
    dists["spread_pmf"] = {{"data", pmf_probs({&data_fp.spread_pmf})}, {"calibrated", pmf_probs(cp)}, {"baseline", pmf_probs(bp)}};
  }

  // ---- survival curves: the data's are the training period's (what was fitted); the simulations' come from
  // the estimator run on the first seed's export of each model.
  json survival;
  survival["bucket_names"] = json::array({distance_bucket_name(0), distance_bucket_name(1), distance_bucket_name(2), distance_bucket_name(3)});
  survival["data_train"] = survival_json(cal);
  survival["data_train_note"] = o.calibration_file.empty()
                                    ? "Kaplan-Meier curves the estimator fitted on the training period (the estimator never reads the test period, so the test period has no curve here)"
                                    : "Kaplan-Meier curves stored in the calibration file that was used";
  survival["calibrated"] = nullptr;
  survival["baseline"] = nullptr;
  if (want_survival) {
    const std::int64_t to_ns = kSimWindowFromNs + o.test.length_ns();
    for (const char* kind : {"calibrated", "baseline"}) {
      const fs::path dir = export_root / kind;
      const std::string tick_name = std::string(kind) == "calibrated" ? "SIMCAL" : "SIMBASE";
      const std::string stem2 = tick_name + "_2012-06-21_34200000_57600000_";
      try {
        const EstimatorOutput e = estimate((dir / (stem2 + "message_" + std::to_string(levels) + ".csv")).string(),
                                           (dir / (stem2 + "orderbook_" + std::to_string(levels) + ".csv")).string(),
                                           kSimWindowFromNs, to_ns, tick);
        survival[kind] = survival_json(e.calibration);
      } catch (const std::exception& ex) {
        survival[std::string(kind) + "_error"] = ex.what();
        notes.push_back(std::string("The estimator could not be run on the ") + kind + " simulation's export: " + ex.what());
      }
    }
    survival["sim_note"] = "the estimator run on the first seed's exported window of each model (seed " + std::to_string(o.first_seed) + ")";
    if (!o.keep_exports) {
      // Only the folders written here; anything else the user keeps in <out>/exports stays.
      std::error_code ec;
      fs::remove_all(export_root / "calibrated", ec);
      fs::remove_all(export_root / "baseline", ec);
      if (fs::is_directory(export_root, ec) && fs::is_empty(export_root, ec)) fs::remove(export_root, ec);
    }
  }

  // ---- validation.json
  json& j = result.json;
  j["title"] = "Out-of-sample validation: " + names.ticker + " " + names.date;
  j["label"] = label;
  j["data_source"] = data_synthetic ? "synthetic" : "real";
  j["calibration_source"] = cal.source;
  j["ticker"] = names.ticker;
  j["date"] = names.date;
  j["levels"] = levels;
  j["tick_units"] = tick;
  j["train"] = {{"from_s", o.train.from_ns / kNs}, {"to_s", o.train.to_ns / kNs}, {"window_note", cal.window_note}};
  j["test"] = {{"from_s", o.test.from_ns / kNs}, {"to_s", o.test.to_ns / kNs}};
  j["leakage"] = leakage;
  j["data_checks"] = {{"resubmitted_order_ids", data_fp.duplicate_order_ids},
                      {"messages_in_test_period", data_fp.messages}};
  j["bootstrap"] = {{"degenerate", data_boot.degenerate},
                    {"block_seconds_used", data_boot.block_seconds_used},
                    {"blocks_per_resample", data_boot.blocks_per_resample}};
  j["calibration"] = {{"file", fs::path(result.calibration_path).filename().string()},
                      {"estimated", o.calibration_file.empty()},
                      {"ticker", cal.ticker},
                      {"date", cal.date},
                      {"source", cal.source},
                      {"window_from_ns", cal.window_from_ns},
                      {"window_to_ns", cal.window_to_ns},
                      {"window_note", cal.window_note},
                      {"median_mid_ticks", cal.median_mid_ticks},
                      {"empty_placement_buckets", empty_buckets},
                      {"tick_units", cal.tick_units},
                      {"lambda_limit_per_s", cal.rates.limit[kBuy].per_s + cal.rates.limit[kSell].per_s},
                      {"lambda_aggressive_per_s", cal.rates.aggressive[kBuy].per_s + cal.rates.aggressive[kSell].per_s}};
  j["simulation"] = {{"seeds", o.seeds},
                     {"first_seed", o.first_seed},
                     {"warmup_s", o.warmup_s},
                     {"test_s", test_s},
                     {"duration_s", duration_s},
                     {"start_price_ticks", start_price},
                     {"owner_count", o.owner_count},
                     {"bootstrap_resamples", o.bootstrap_resamples},
                     {"calibrated_latency_us", 0},
                     {"baseline_noise_traders", baseline_noise_traders_json()},
                     {"export", {{"levels", levels}, {"filter_window", true}, {"price_scale", tick}, {"window_start_s", o.warmup_s},
                                 {"hidden_executions", false}, {"halt", false}}},
                     {"calibrated_per_seed", per_seed_cal},
                     {"baseline_per_seed", per_seed_base},
                     {"calibrated_agent_counters", counters_json(totals)}};
  j["notes"] = notes;
  j["scorecard"] = scorecard;
  j["verdict_counts"] = {{"calibrated", counts_json(counts_cal)}, {"baseline", counts_json(counts_base)}};
  j["distributions"] = dists;
  j["survival"] = survival;
  j["fingerprints"] = {{"data", to_json(data_fp)},
                       {"data_bootstrap", to_json(data_boot)},
                       {"calibrated_runs", to_json(cal_runs)},
                       {"baseline_runs", to_json(base_runs)},
                       {"calibrated_each_seed", [&] { json a = json::array(); for (auto& f : cal_fps) a.push_back(to_json(f)); return a; }()},
                       {"baseline_each_seed", [&] { json a = json::array(); for (auto& f : base_fps) a.push_back(to_json(f)); return a; }()}};
  {
    std::ofstream f(fs::path(out_dir) / "validation.json", std::ios::binary | std::ios::trunc);
    f << j.dump(2) << '\n';
    f.flush();
    if (!f) throw std::runtime_error("cannot write " + (fs::path(out_dir) / "validation.json").string());
  }
  return result;
}

}  // namespace calib
