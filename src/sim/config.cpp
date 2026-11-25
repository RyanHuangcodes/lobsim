#include "sim/config.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sim {

namespace {

using nlohmann::json;

[[noreturn]] void bad(const std::string& field, const std::string& why) {
  throw std::runtime_error("config: " + field + ": " + why);
}

// The object `j`, which must contain only keys from `known`.
void check_keys(const json& j, const std::string& where, std::initializer_list<std::string_view> known) {
  if (!j.is_object()) bad(where.empty() ? "(root)" : where, "must be an object");
  for (auto it = j.begin(); it != j.end(); ++it) {
    bool found = false;
    for (std::string_view k : known) found = found || k == it.key();
    if (!found) bad(where.empty() ? it.key() : where + "." + it.key(), "unknown key");
  }
}

// The empty `where` of the top level, named so `at` is never handed a temporary (a
// `const json&` bound to the result would otherwise trip -Wdangling-reference on GCC 14).
const std::string kRoot;

const json& at(const json& j, const std::string& where, const char* key) {
  const auto it = j.find(key);
  if (it == j.end()) bad(where.empty() ? key : where + "." + key, "missing");
  return *it;
}

double number(const json& j, const std::string& where, const char* key) {
  const json& v = at(j, where, key);
  if (!v.is_number()) bad(where.empty() ? key : where + "." + key, "must be a number");
  return v.get<double>();
}

std::int64_t integer(const json& j, const std::string& where, const char* key) {
  const json& v = at(j, where, key);
  if (!v.is_number_integer()) bad(where.empty() ? key : where + "." + key, "must be an integer");
  return v.get<std::int64_t>();
}

// One conversion from a config number to integer nanoseconds. A value that does not fit
// in 63 bits (or is not finite) is an error naming the field: llround would be undefined.
// The bound is kMaxConfigNs (1e18 ns), so now + base latency + jitter can never overflow.
TimeNs to_ns(double value, double unit_ns, const std::string& field) {
  const double ns = value * unit_ns;
  if (!std::isfinite(ns) || std::fabs(ns) > static_cast<double>(kMaxConfigNs)) bad(field, "out of range");
  return static_cast<TimeNs>(std::llround(ns));
}

LatencySpec latency(const json& parent, const std::string& where) {
  const std::string here = where + ".latency_us";
  const json& j = at(parent, where, "latency_us");
  check_keys(j, here, {"entry", "response", "feed", "jitter"});
  LatencySpec l;
  l.entry = to_ns(number(j, here, "entry"), 1e3, here + ".entry");
  l.response = to_ns(number(j, here, "response"), 1e3, here + ".response");
  l.feed = to_ns(number(j, here, "feed"), 1e3, here + ".feed");
  l.jitter = to_ns(number(j, here, "jitter"), 1e3, here + ".jitter");
  return l;
}

json latency_json(const LatencySpec& l) {
  return json{{"entry", static_cast<double>(l.entry) / 1e3},
              {"response", static_cast<double>(l.response) / 1e3},
              {"feed", static_cast<double>(l.feed) / 1e3},
              {"jitter", static_cast<double>(l.jitter) / 1e3}};
}

void check_latency(const LatencySpec& l, const std::string& where) {
  if (l.entry < 0) bad(where + ".latency_us.entry", "must be >= 0");
  if (l.response < 0) bad(where + ".latency_us.response", "must be >= 0");
  if (l.feed < 0) bad(where + ".latency_us.feed", "must be >= 0");
  if (l.jitter < 0) bad(where + ".latency_us.jitter", "must be >= 0");
}

// What the agent needs from the calibration, as a clear error at load time. The same bucket
// rules as the agent: every spread bucket is reachable (a one-sided or empty local book uses
// bucket "1"), so every side that can send a limit order needs at least one placement PMF; a bucket without one
// falls back to the nearest non-empty bucket of the side (D-385).
void check_calibration_usable(const calib::Calibration& c, const std::string& file) {
  const std::string w = "background.file (" + file + ")";
  const double lim_buy = c.rates.limit[calib::kBuy].per_s, lim_sell = c.rates.limit[calib::kSell].per_s;
  const double agg_buy = c.rates.aggressive[calib::kBuy].per_s, agg_sell = c.rates.aggressive[calib::kSell].per_s;
  if (!(lim_buy + lim_sell > 0.0) && !(agg_buy + agg_sell > 0.0)) bad(w, "every rate is 0: nothing to generate");
  if (!std::isfinite(lim_buy + lim_sell) || !std::isfinite(agg_buy + agg_sell)) bad(w, "rates overflow");
  if (lim_buy + lim_sell > 0.0) {
    if (c.limit_size.empty()) bad(w, "limit_size is empty but limit orders have a positive rate");
    for (int s = 0; s < 2; ++s) {
      if (c.rates.limit[s].per_s <= 0.0) continue;
      // One empty bucket is fine (the agent draws from the nearest non-empty one); all three are not.
      bool any = false;
      for (int b = 0; b < calib::kSpreadBuckets; ++b) any = any || !c.placement[s][b].empty();
      if (!any)
        bad(w, std::string("placement[") + (s == calib::kBuy ? "buy" : "sell") +
                   "]: all three spread buckets are empty but that side has a positive limit rate");
    }
  }
  if (agg_buy + agg_sell > 0.0 && c.aggressive_size.empty())
    bad(w, "aggressive_size is empty but aggressive orders have a positive rate");
}

bool positive(double x) { return std::isfinite(x) && x > 0.0; }

}  // namespace

SimConfig parse_config(const json& j, const std::string& base_dir) {
  check_keys(j, "",
             {"seed", "duration_s", "warmup_s", "max_price", "initial_price", "tick_display", "fundamental",
              "noise_traders", "informed", "mm", "background", "markout_horizons_s", "sample_interval_s"});
  SimConfig c;
  {
    // Any unsigned 64-bit seed, as `--seed` accepts.
    const json& v = at(j, kRoot, "seed");
    if (!v.is_number_integer()) bad("seed", "must be an integer");
    if (v.is_number_unsigned()) c.seed = v.get<std::uint64_t>();
    else if (v.get<std::int64_t>() < 0) bad("seed", "must be >= 0");
    else c.seed = static_cast<std::uint64_t>(v.get<std::int64_t>());
  }
  c.duration_s = number(j, "", "duration_s");
  c.warmup_s = number(j, "", "warmup_s");
  c.max_price = integer(j, "", "max_price");
  c.initial_price = integer(j, "", "initial_price");
  c.tick_display = number(j, "", "tick_display");

  {
    const json& f = at(j, kRoot, "fundamental");
    check_keys(f, "fundamental", {"sigma_ticks_per_sqrt_s", "dt_s"});
    c.fundamental.sigma_ticks_per_sqrt_s = number(f, "fundamental", "sigma_ticks_per_sqrt_s");
    c.fundamental.dt_s = number(f, "fundamental", "dt_s");
  }
  {
    const std::string w = "noise_traders";
    const json& n = at(j, kRoot, "noise_traders");
    check_keys(n, w, {"count", "lambda", "p_limit", "order_lifetime_mean_s", "D", "max_size", "latency_us",
                     "reference_price"});
    const std::int64_t count = integer(n, w, "count");
    if (count < 0 || count > 1'000'000) bad(w + ".count", "must be between 0 and 1000000");
    c.noise_traders.count = static_cast<std::uint32_t>(count);
    c.noise_traders.lambda = number(n, w, "lambda");
    c.noise_traders.p_limit = number(n, w, "p_limit");
    c.noise_traders.order_lifetime_mean_s = number(n, w, "order_lifetime_mean_s");
    c.noise_traders.D = integer(n, w, "D");
    c.noise_traders.max_size = integer(n, w, "max_size");
    c.noise_traders.latency = latency(n, w);
    if (const auto it = n.find("reference_price"); it != n.end()) {
      const std::string field = w + ".reference_price";
      if (!it->is_string()) bad(field, "must be \"floor\" or \"symmetric\"");
      const std::string v = it->get<std::string>();
      if (v == "floor") c.noise_traders.reference_price = ReferencePrice::Floor;
      else if (v == "symmetric") c.noise_traders.reference_price = ReferencePrice::Symmetric;
      else bad(field, "must be \"floor\" or \"symmetric\"");
    }
  }
  if (j.contains("informed")) {
    const std::string w = "informed";
    const json& i = j["informed"];
    check_keys(i, w, {"lambda", "theta_ticks", "size", "latency_us"});
    InformedConfig inf;
    inf.lambda = number(i, w, "lambda");
    inf.theta_ticks = number(i, w, "theta_ticks");
    inf.size = integer(i, w, "size");
    inf.latency = latency(i, w);
    c.informed = inf;
  }
  if (j.contains("mm")) {
    const std::string w = "mm";
    const json& m = j["mm"];
    check_keys(m, w,
               {"gamma", "kappa", "sigma_ticks_per_sqrt_s", "quote_size", "q_max", "requote_interval_ms",
                "maker_fee_ticks", "taker_fee_ticks", "latency_us"});
    MarketMakerConfig mm;
    mm.gamma = number(m, w, "gamma");
    mm.kappa = number(m, w, "kappa");
    mm.sigma_ticks_per_sqrt_s = number(m, w, "sigma_ticks_per_sqrt_s");
    mm.quote_size = integer(m, w, "quote_size");
    mm.q_max = integer(m, w, "q_max");
    mm.requote_interval_ms = number(m, w, "requote_interval_ms");
    mm.maker_fee_ticks = number(m, w, "maker_fee_ticks");
    mm.taker_fee_ticks = number(m, w, "taker_fee_ticks");
    mm.latency = latency(m, w);
    c.mm = mm;
  }
  if (j.contains("background")) {
    const std::string w = "background";
    const json& b = j["background"];
    check_keys(b, w, {"model", "file", "owner_count", "latency_us", "start_price"});
    const json& model = at(b, w, "model");
    if (!model.is_string() || model.get<std::string>() != "calibrated") bad(w + ".model", "must be \"calibrated\"");
    const json& file = at(b, w, "file");
    if (!file.is_string() || file.get<std::string>().empty()) bad(w + ".file", "must be a non-empty string");
    BackgroundConfig bg;
    std::filesystem::path path(file.get<std::string>());
    if (path.is_relative() && !base_dir.empty()) path = std::filesystem::path(base_dir) / path;
    bg.file = path.string();
    if (b.contains("owner_count")) {
      const std::int64_t n = integer(b, w, "owner_count");
      if (n < 1 || n > 100'000) bad(w + ".owner_count", "must be between 1 and 100000");
      bg.owner_count = static_cast<std::uint32_t>(n);
    }
    if (b.contains("latency_us")) bg.latency = latency(b, w);
    if (b.contains("start_price")) bg.start_price = integer(b, w, "start_price");
    try {
      bg.calibration = std::make_shared<const calib::Calibration>(calib::load(bg.file));
    } catch (const std::runtime_error& e) {
      bad(w + ".file", e.what());
    }
    check_calibration_usable(*bg.calibration, bg.file);
    c.background = std::move(bg);
  }
  {
    const json& h = at(j, kRoot, "markout_horizons_s");
    if (!h.is_array()) bad("markout_horizons_s", "must be an array of numbers");
    for (const json& v : h) {
      if (!v.is_number()) bad("markout_horizons_s", "must be an array of numbers");
      c.markout_horizons_s.push_back(v.get<double>());
    }
  }
  c.sample_interval_s = number(j, "", "sample_interval_s");
  finalize(c);
  return c;
}

void finalize(SimConfig& c) {
  if (!positive(c.duration_s)) bad("duration_s", "must be > 0");
  if (c.duration_s > kMaxDurationS) bad("duration_s", "must be <= 1e9 seconds");
  if (!std::isfinite(c.warmup_s) || c.warmup_s < 0.0 || c.warmup_s > c.duration_s)
    bad("warmup_s", "must be between 0 and duration_s");
  if (c.max_price < 2) bad("max_price", "must be >= 2");
  if (c.initial_price < 1 || c.initial_price > c.max_price) bad("initial_price", "must be in [1, max_price]");
  if (!positive(c.tick_display)) bad("tick_display", "must be > 0");
  if (!std::isfinite(c.fundamental.sigma_ticks_per_sqrt_s) || c.fundamental.sigma_ticks_per_sqrt_s < 0.0)
    bad("fundamental.sigma_ticks_per_sqrt_s", "must be >= 0");
  if (!positive(c.fundamental.dt_s)) bad("fundamental.dt_s", "must be > 0");
  if (!positive(c.sample_interval_s)) bad("sample_interval_s", "must be > 0");

  const NoiseTradersConfig& n = c.noise_traders;
  if (!positive(n.lambda)) bad("noise_traders.lambda", "must be > 0");
  if (!std::isfinite(n.p_limit) || n.p_limit < 0.0 || n.p_limit > 1.0) bad("noise_traders.p_limit", "must be in [0, 1]");
  if (!positive(n.order_lifetime_mean_s)) bad("noise_traders.order_lifetime_mean_s", "must be > 0");
  if (n.D < 1) bad("noise_traders.D", "must be >= 1");
  if (n.max_size < 1) bad("noise_traders.max_size", "must be >= 1");
  check_latency(n.latency, "noise_traders");

  if (c.informed) {
    if (!positive(c.informed->lambda)) bad("informed.lambda", "must be > 0");
    if (!std::isfinite(c.informed->theta_ticks) || c.informed->theta_ticks < 0.0)
      bad("informed.theta_ticks", "must be >= 0");
    if (c.informed->size < 1) bad("informed.size", "must be >= 1");
    check_latency(c.informed->latency, "informed");
  }
  if (c.background) {
    if (!c.background->calibration) bad("background.file", "the calibration is not loaded");
    if (c.background->owner_count < 1) bad("background.owner_count", "must be >= 1");
    check_latency(c.background->latency, "background");
    if (c.background->start_price && (*c.background->start_price < 1 || *c.background->start_price > c.max_price))
      bad("background.start_price", "must be in [1, max_price]");
    if (!c.background->start_price && (c.background->calibration->median_mid_ticks > c.max_price))
      bad("background.file", "median_mid_ticks is above max_price; set background.start_price");
  }
  c.requote_interval_ns = 0;
  if (c.mm) {
    if (!positive(c.mm->gamma)) bad("mm.gamma", "must be > 0");
    if (!positive(c.mm->kappa)) bad("mm.kappa", "must be > 0");
    if (!std::isfinite(c.mm->sigma_ticks_per_sqrt_s) || c.mm->sigma_ticks_per_sqrt_s < 0.0)
      bad("mm.sigma_ticks_per_sqrt_s", "must be >= 0");
    if (c.mm->quote_size < 1) bad("mm.quote_size", "must be >= 1");
    if (c.mm->q_max < 1) bad("mm.q_max", "must be >= 1");
    if (!positive(c.mm->requote_interval_ms)) bad("mm.requote_interval_ms", "must be > 0");
    if (!std::isfinite(c.mm->maker_fee_ticks)) bad("mm.maker_fee_ticks", "must be finite");
    if (!std::isfinite(c.mm->taker_fee_ticks)) bad("mm.taker_fee_ticks", "must be finite");
    check_latency(c.mm->latency, "mm");
    c.requote_interval_ns = to_ns(c.mm->requote_interval_ms, 1e6, "mm.requote_interval_ms");
    if (c.requote_interval_ns < 1) bad("mm.requote_interval_ms", "must be at least 1 ns");
  }

  c.duration_ns = to_ns(c.duration_s, 1e9, "duration_s");
  c.warmup_ns = to_ns(c.warmup_s, 1e9, "warmup_s");
  c.fundamental_dt_ns = to_ns(c.fundamental.dt_s, 1e9, "fundamental.dt_s");
  c.sample_interval_ns = to_ns(c.sample_interval_s, 1e9, "sample_interval_s");
  if (c.duration_ns < 1) bad("duration_s", "must be at least 1 ns");
  if (c.fundamental_dt_ns < 1) bad("fundamental.dt_s", "must be at least 1 ns");
  if (c.sample_interval_ns < 1) bad("sample_interval_s", "must be at least 1 ns");
  c.markout_horizons_ns.clear();
  for (double h : c.markout_horizons_s) {
    if (!positive(h)) bad("markout_horizons_s", "every horizon must be > 0");
    c.markout_horizons_ns.push_back(to_ns(h, 1e9, "markout_horizons_s"));
  }
}

SimConfig load_config(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("config: cannot open " + path);
  json j;
  try {
    in >> j;
  } catch (const json::exception& e) {
    throw std::runtime_error("config: " + path + " is not valid JSON: " + e.what());
  }
  return parse_config(j, std::filesystem::path(path).parent_path().string());
}

json to_json(const SimConfig& c) {
  // nlohmann::ordered_json would keep insertion order; plain json sorts keys, which is
  // just as deterministic and needs no second type.
  json j;
  j["seed"] = c.seed;
  j["duration_s"] = c.duration_s;
  j["warmup_s"] = c.warmup_s;
  j["max_price"] = c.max_price;
  j["initial_price"] = c.initial_price;
  j["tick_display"] = c.tick_display;
  j["fundamental"] = {{"sigma_ticks_per_sqrt_s", c.fundamental.sigma_ticks_per_sqrt_s}, {"dt_s", c.fundamental.dt_s}};
  j["noise_traders"] = {{"count", c.noise_traders.count},
                        {"reference_price", c.noise_traders.reference_price == ReferencePrice::Symmetric ? "symmetric" : "floor"},
                        {"lambda", c.noise_traders.lambda},
                        {"p_limit", c.noise_traders.p_limit},
                        {"order_lifetime_mean_s", c.noise_traders.order_lifetime_mean_s},
                        {"D", c.noise_traders.D},
                        {"max_size", c.noise_traders.max_size},
                        {"latency_us", latency_json(c.noise_traders.latency)}};
  if (c.informed)
    j["informed"] = {{"lambda", c.informed->lambda},
                     {"theta_ticks", c.informed->theta_ticks},
                     {"size", c.informed->size},
                     {"latency_us", latency_json(c.informed->latency)}};
  if (c.mm)
    j["mm"] = {{"gamma", c.mm->gamma},
               {"kappa", c.mm->kappa},
               {"sigma_ticks_per_sqrt_s", c.mm->sigma_ticks_per_sqrt_s},
               {"quote_size", c.mm->quote_size},
               {"q_max", c.mm->q_max},
               {"requote_interval_ms", c.mm->requote_interval_ms},
               {"maker_fee_ticks", c.mm->maker_fee_ticks},
               {"taker_fee_ticks", c.mm->taker_fee_ticks},
               {"latency_us", latency_json(c.mm->latency)}};
  if (c.background) {
    j["background"] = {{"model", "calibrated"},
                       {"file", c.background->file},
                       {"owner_count", c.background->owner_count},
                       {"latency_us", latency_json(c.background->latency)}};
    if (c.background->start_price) j["background"]["start_price"] = *c.background->start_price;
  }
  j["markout_horizons_s"] = c.markout_horizons_s;
  j["sample_interval_s"] = c.sample_interval_s;
  return j;
}

}  // namespace sim
