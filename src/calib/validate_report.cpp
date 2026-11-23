// validation.md, rendered from validation.json alone (so the report can be rebuilt from the numbers).
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "calib/validate.hpp"

namespace calib {

namespace {

using nlohmann::json;

std::string fmt(const json& v) {
  if (v.is_null() || !v.is_number()) return "n/a";
  const double x = v.get<double>();
  if (!std::isfinite(x)) return "n/a";
  char buf[64];
  if (std::fabs(x) >= 1e4) std::snprintf(buf, sizeof buf, "%.0f", x);
  else std::snprintf(buf, sizeof buf, "%.4g", x);
  return buf;
}

std::string interval(const json& value, const json& lo, const json& hi) {
  if (lo.is_null() || hi.is_null()) return fmt(value) + " [n/a]";
  return fmt(value) + " [" + fmt(lo) + ", " + fmt(hi) + "]";
}

std::string verdict_text(const json& v) {
  const std::string kind = v.at("kind").get<std::string>();
  if (kind == "miss") {
    const json& r = v.at("ratio");
    return "miss" + (r.is_number() && std::isfinite(r.get<double>()) ? " (sim/data = " + fmt(r) + ")" : std::string(" (data value 0)"));
  }
  return kind;
}

std::string seconds(const json& ns) {
  if (!ns.is_number()) return "n/a";
  return fmt(json(ns.get<double>() / 1e9));
}

std::string period_text(const json& p) {
  return std::to_string(p.at("from_s").get<long long>()) + " to " + std::to_string(p.at("to_s").get<long long>()) + " s after midnight";
}

}  // namespace

std::string render_markdown(const json& v, bool with_plots) {
  std::ostringstream o;
  const std::string label = v.at("label").get<std::string>();
  const bool synthetic = label == "SYNTHETIC";
  const std::string ticker = v.at("ticker").get<std::string>();
  const std::string date = v.at("date").get<std::string>();
  const json& sim = v.at("simulation");
  const json& cal = v.at("calibration");
  const std::string tag = synthetic ? "SYNTHETIC data, NOT real market data" : label == "REAL" ? "real data" : "hand-made calibration";

  o << "# Out-of-sample validation: " << ticker << " " << date << " (" << tag << ")\n\n";
  if (synthetic) {
    o << "> **SYNTHETIC.** The data, or the calibration, or both come from lobsim's own simulator (`README_SYNTHETIC.txt` or "
         "`source: synthetic`). Every number below is a statement about a synthetic market, not about a real one. "
         "Data source: " << v.at("data_source").get<std::string>() << "; calibration source: "
      << v.at("calibration_source").get<std::string>() << ".\n\n";
  } else if (label == "REAL") {
    o << "> **Real data.** Data source: " << v.at("data_source").get<std::string>() << "; calibration source: "
      << v.at("calibration_source").get<std::string>() << ". The numbers were computed from the files named below.\n\n";
  } else {
    o << "> **The calibration is hand-made** (source: " << v.at("calibration_source").get<std::string>()
      << ") and the data is not marked synthetic. The numbers say how a hand-written flow compares with this data.\n\n";
  }

  o << "- Data: `" << ticker << "`, " << date << ", L = " << v.at("levels").get<int>() << ", tick = "
    << v.at("tick_units").get<long long>() << " file price unit(s).\n"
    << "- Calibration: `" << cal.at("file").get<std::string>() << "` ("
    << (cal.at("estimated").get<bool>() ? "estimated on the training period" : "read from a file, not estimated from this data") << "), ticker "
    << cal.at("ticker").get<std::string>() << ", start price " << sim.at("start_price_ticks").get<long long>() << " ticks.\n"
    << "- Training period: " << period_text(v.at("train")) << ". Test period: " << period_text(v.at("test")) << ".\n"
    << "- Simulations: " << sim.at("seeds").get<int>() << " seeds per model (seeds " << sim.at("first_seed").get<long long>() << " to "
    << sim.at("first_seed").get<long long>() + sim.at("seeds").get<int>() - 1 << "), each " << fmt(sim.at("warmup_s")) << " s of warm-up plus "
    << fmt(sim.at("test_s")) << " s measured (as long as the test period). Informed trader and market maker off in both.\n"
    << "- Calibrated: the whole background is one `CalibratedFlowAgent` (" << sim.at("owner_count").get<int>()
    << " owner ids, latency 0). Baseline: the old hand-picked noise traders of `configs/default.json` (10 traders, lambda 5/s each, "
       "distance uniform on 1 to 10 ticks, exponential lifetimes of mean 30 s, sizes 1 to 10, latency 200 us), same start price, same duration, same export.\n"
    << "- Export of every simulation: `--lobster-like --filter-window` at L = " << v.at("levels").get<int>() << ", `--price-scale "
    << v.at("tick_units").get<long long>() << "`, window start " << fmt(sim.at("warmup_s")) << " s; no hidden executions and no halt (the fingerprint ignores types 5 and 7).\n";
  const json& leak = v.at("leakage");
  o << "- Leakage check: ";
  if (leak.at("checked").get<bool>()) {
    o << "passed. The estimator read messages from " << seconds(leak.at("first_message_ns")) << " s to " << seconds(leak.at("last_message_ns"))
      << " s: inside the training period, and none inside the test period.\n";
  } else {
    o << "not applicable (" << leak.at("note").get<std::string>() << ").\n";
  }
  for (const json& n : v.at("notes")) o << "- Note: " << n.get<std::string>() << "\n";
  o << "\n";

  // ---- scorecard
  const json& counts = v.at("verdict_counts");
  o << "## Scorecard (" << tag << ")\n\n"
    << "Verdict, calibrated simulation against the test-period data: **match** if the simulation's interval overlaps the data's interval; "
       "**close** if the gap between the two intervals is at most twice the data interval's half-width; otherwise **miss**, with the ratio "
       "simulation mean / data value. The data interval is the 95% moving-block bootstrap interval (5-minute blocks, "
    << sim.at("bootstrap_resamples").get<int>() << " resamples). The simulation interval is the 95% interval of the mean over the "
    << sim.at("seeds").get<int>() << " seeds (Student t); the range of the individual runs is in `validation.json` (`run_lo`, `run_hi`). "
       "The baseline cell ends with the baseline's own verdict, by the same rule.\n\n";
  o << "Calibrated: " << counts.at("calibrated").at("match").get<int>() << " match, " << counts.at("calibrated").at("close").get<int>()
    << " close, " << counts.at("calibrated").at("miss").get<int>() << " miss, " << counts.at("calibrated").at("n/a").get<int>()
    << " undefined. Baseline: " << counts.at("baseline").at("match").get<int>() << " match, " << counts.at("baseline").at("close").get<int>()
    << " close, " << counts.at("baseline").at("miss").get<int>() << " miss, " << counts.at("baseline").at("n/a").get<int>() << " undefined.\n\n";
  o << "| metric | data (test) [95% interval] | calibrated sim [95% CI of mean] | baseline sim [95% CI of mean] | verdict |\n|---|---|---|---|---|\n";
  for (const json& r : v.at("scorecard")) {
    const json& d = r.at("data");
    const json& c = r.at("calibrated");
    const json& b = r.at("baseline");
    o << "| `" << r.at("name").get<std::string>() << "` | " << interval(d.at("value"), d.at("lo"), d.at("hi")) << " | "
      << interval(c.at("mean"), c.at("ci_lo"), c.at("ci_hi")) << " | " << interval(b.at("mean"), b.at("ci_lo"), b.at("ci_hi")) << "; "
      << verdict_text(r.at("baseline_verdict")) << " | " << verdict_text(r.at("verdict")) << " |\n";
  }
  o << "\nRows: spread (mean in ticks, share of two-sided time at one tick), share of time with one side empty, depth in shares at levels 1 to L "
       "per side, volatility (standard deviation of log mid returns on a 1 s, 10 s and 60 s clock), excess kurtosis of the 10 s returns, "
       "lag-1 autocorrelation of the 1 s returns (bid-ask bounce), autocorrelation of |10 s returns| at lags 1 to 10 (volatility clustering), "
       "autocorrelation of aggressive-order signs at lags 1, 10, 100 (trade-sign memory), fill probability by placement distance, executed share, "
       "and the input rates re-measured from the files. Definitions: `docs/FINGERPRINT.md`. The autocorrelation estimator is the pair mean "
       "over the variance, which differs from the textbook estimator by a factor n/(n-k); it is the same code on the data and on the simulations.\n\n";

  // ---- distributions
  const json& dist = v.at("distributions");
  const json& yard = dist.at("yardstick");
  o << "## Distributions against the data's own halves (" << tag << ")\n\n"
    << "W1 is the Wasserstein-1 distance in the variable's unit (ticks for the spread, levels for the depth profile, which is compared as a shape "
       "normalised over levels); TV is the total-variation distance. The yardstick is the same distance between the first and the second half "
       "of the test period of the data, with the upper end of its bootstrap 95% interval: a simulation within it is as close to the data as the "
       "data's halves are to each other. Simulation columns are the mean over seeds of the distance to the whole test period, with the range "
       "over seeds in brackets.\n\n"
    << "| distribution | distance | data halves | halves, bootstrap upper | calibrated sim | verdict | baseline sim | verdict |\n|---|---|---|---|---|---|---|---|\n";
  struct Row {
    const char* label;
    const char* key;      // in distributions.<model>
    const char* point;    // in yardstick.point
    const char* boot_w1;
    const char* boot_tv;
  };
  const Row rows[] = {{"spread PMF", "spread", "spread", "spread_w1", "spread_tv"},
                      {"depth profile, buy side", "depth_buy", "depth_buy", "depth_buy_w1", "depth_buy_tv"},
                      {"depth profile, sell side", "depth_sell", "depth_sell", "depth_sell_w1", "depth_sell_tv"}};
  const auto sim_cell = [](const json& x) {
    return fmt(x.at("mean")) + " [" + fmt(x.at("min")) + ", " + fmt(x.at("max")) + "]";
  };
  const auto within = [](const json& x, const json& upper) {
    if (!x.at("mean").is_number() || !upper.is_number()) return std::string("n/a");
    return x.at("mean").get<double>() <= upper.get<double>() ? std::string("within") : std::string("beyond");
  };
  for (const Row& r : rows) {
    for (const bool w1 : {true, false}) {
      const json& point = yard.at("point").at(r.point);
      const json& upper = yard.at(w1 ? r.boot_w1 : r.boot_tv).at("hi");
      const char* m = w1 ? "w1" : "tv";
      const json& c = dist.at("calibrated").at(r.key).at(m);
      const json& b = dist.at("baseline").at(r.key).at(m);
      o << "| " << r.label << " | " << (w1 ? (std::string(r.key) == "spread" ? "W1 (ticks)" : "W1 (levels)") : "TV") << " | "
        << (point.at("valid").get<bool>() ? fmt(point.at(m)) : std::string("n/a")) << " | " << fmt(upper) << " | " << sim_cell(c) << " | "
        << within(c, upper) << " | " << sim_cell(b) << " | " << within(b, upper) << " |\n";
    }
  }
  o << "\nSpread PMF of the test period and of the two models: `plots/spread_pmf.png`, numbers in `validation.json` (`distributions.spread_pmf`).\n\n";

  // ---- survival
  const json& surv = v.at("survival");
  o << "## Lifetime survival (" << tag << ")\n\n"
    << "The data curve is the training period's Kaplan-Meier curve, i.e. what was fitted: " << surv.at("data_train_note").get<std::string>()
    << ". The simulation curves are the estimator run on the exported window of the first seed of each model"
       " (survival is not a scalar of the fingerprint, and the estimator may not read the test period). "
       "Median lifetime in seconds by placement-distance bucket (n/a: the curve never reaches 0.5, i.e. the order is never cancelled in half the cases):\n\n"
    << "| distance bucket | data (training period) | calibrated sim | baseline sim |\n|---|---|---|---|\n";
  const auto median_cell = [&](const json& curves, std::size_t b) {
    if (curves.is_null() || !curves.is_array() || b >= curves.size()) return std::string("not available");
    const json& m = curves[b].at("median_ns");
    return m.is_number() ? seconds(m) : std::string("n/a");
  };
  for (std::size_t b = 0; b < surv.at("bucket_names").size(); ++b) {
    o << "| " << surv.at("bucket_names")[b].get<std::string>() << " | " << median_cell(surv.at("data_train"), b) << " | "
      << median_cell(surv.at("calibrated"), b) << " | " << median_cell(surv.at("baseline"), b) << " |\n";
  }
  o << "\n";

  // ---- plots
  if (with_plots) {
    o << "## Plots (" << tag << ")\n\n";
    for (const char* name : {"spread_pmf", "depth_profile", "survival", "volatility_vs_interval"})
      o << "![" << name << "](plots/" << name << ".png)\n\n";
  } else {
    o << "## Plots\n\nNot drawn (see the run's output for why); `tools/plot_calibration.py --validation validation.json --out plots` draws them.\n\n";
  }

  // ---- known gaps
  const json& k = sim.at("calibrated_agent_counters");
  o << "## Known gaps\n\n"
    << "- **Trade-sign memory and volatility clustering are expected misses.** The calibrated flow draws every order independently: signs have no memory "
       "(real order signs decay roughly like lag^-0.6, Lillo and Farmer 2004) and nothing makes high-volatility periods follow each other. "
       "`sign_acf_lag*` and `acf_abs_10s_lag*` are therefore expected to differ from the data, and nothing was tuned to hide it."
    << (synthetic ? " On synthetic data from this simulator, the sign memory there is comes from the informed trader, which the calibrated flow does not contain." : "")
    << "\n"
    << "- Event rates here are constant over the window and do not react to the book state (no queue-reactive or Hawkes dynamics), and there is no intraday profile.\n"
    << "- The estimator under-counts submissions placed outside the published window (they are never seen), so the arrival rate and the far tail of the placement distribution are slightly low; "
       "C3's filtered recovery measures by how much.\n"
    << "- Volatility is the least certain scalar: a zero-intelligence flow with the right spread and depth still moves the mid price differently from real traders (Farmer, Patelli and Zovko explain about three quarters of the diffusion rate, not all of it).\n"
    << "- The fingerprint measures the whole exported window of each simulation, which starts after a warm-up that fills the book; the test-period data starts wherever the data's own book is. "
       "Spread and depth rows are the first to show a mismatch of the starting state.\n"
    << "- Calibrated flow counters summed over seeds: " << k.at("limits_sent").get<unsigned long long>() << " limit orders, "
    << k.at("markets_sent").get<unsigned long long>() << " market orders, " << k.at("placement_redraws").get<unsigned long long>()
    << " placement redraws (a draw that would cross), " << k.at("placement_clamped").get<unsigned long long>()
    << " placements clamped after 100 redraws, " << k.at("fallback_one_sided").get<unsigned long long>() << " one-sided and "
    << k.at("fallback_empty").get<unsigned long long>() << " empty-book fallbacks, " << k.at("no_cancel_draws").get<unsigned long long>()
    << " lifetime draws without a cancel.\n\n";
  o << "Everything in this file is computed from `validation.json`.\n";
  return o.str();
}

}  // namespace calib
