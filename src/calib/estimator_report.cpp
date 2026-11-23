// The output file name and the short human summary written beside the calibration JSON.
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "calib/estimator.hpp"

namespace calib {
namespace {

std::string fixed(double v, int decimals) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

std::string seconds(std::int64_t ns) {
  std::string frac = std::to_string(ns % kNsPerSecond);
  frac.insert(0, 9 - frac.size(), '0');
  return std::to_string(ns / kNsPerSecond) + "." + frac;
}

const char* side_name(int s) { return s == kBuy ? "buy" : "sell"; }

// The k most frequent values, "v (share%)", most frequent first, ties by value.
std::string most_common(const Pmf& p, std::size_t k) {
  std::vector<std::pair<std::uint64_t, std::int64_t>> by_count;
  for (std::size_t i = 0; i < p.values.size(); ++i) by_count.emplace_back(p.counts[i], p.values[i]);
  std::sort(by_count.begin(), by_count.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first > b.first : a.second < b.second;
  });
  const double total = static_cast<double>(p.total());
  std::string out;
  for (std::size_t i = 0; i < std::min(k, by_count.size()); ++i) {
    if (!out.empty()) out += ", ";
    out += std::to_string(by_count[i].second) + " (" + fixed(100.0 * static_cast<double>(by_count[i].first) / total, 1) +
           "%)";
  }
  return out.empty() ? "-" : out;
}

// The smallest values and their shares: the head of a size PMF.
std::string head(const Pmf& p, std::size_t k) {
  const double total = static_cast<double>(p.total());
  std::string out;
  for (std::size_t i = 0; i < std::min(k, p.values.size()); ++i) {
    if (!out.empty()) out += ", ";
    out += std::to_string(p.values[i]) + ": " + fixed(100.0 * static_cast<double>(p.counts[i]) / total, 1) + "%";
  }
  return out.empty() ? "-" : out;
}

// Nanoseconds as seconds with 3 decimals (truncated), by integer arithmetic.
std::string millis(std::int64_t ns) {
  std::string ms = std::to_string((ns % kNsPerSecond) / 1'000'000);
  ms.insert(0, 3 - ms.size(), '0');
  return std::to_string(ns / kNsPerSecond) + "." + ms;
}

std::string duration(const std::optional<std::int64_t>& ns) {
  if (!ns) return "not reached";
  return millis(*ns) + " s";
}

}  // namespace

std::string output_stem(const Calibration& c) {
  const std::int64_t from_s = c.window_from_ns / kNsPerSecond;
  const std::int64_t to_s = (c.window_to_ns + kNsPerSecond - 1) / kNsPerSecond;
  return c.ticker + "_" + c.date + "_" + std::to_string(from_s) + "-" + std::to_string(to_s);
}

std::string summary_markdown(const Calibration& c, const std::vector<std::string>& warnings) {
  std::string o;
  o += "# Calibration " + c.ticker + " " + c.date + ", " + output_stem(c).substr(c.ticker.size() + c.date.size() + 2) +
       " s\n\n";
  if (c.source == "synthetic") {
    o += "**SYNTHETIC data, not market data.** Every number below describes a simulator-generated day.\n\n";
  } else if (c.source == "real") {
    o += "Source: real market data (LOBSTER files).\n\n";
  } else {
    o += "Source: " + c.source + ".\n\n";
  }
  o += "- Window: " + seconds(c.window_from_ns) + " to " + seconds(c.window_to_ns) + " s after midnight, " +
       millis(c.observed_ns) + " s observed. " + c.window_note + ".\n";
  for (const InputFile& f : c.inputs) o += "- Input: `" + f.name + "`, SHA-256 `" + f.sha256 + "`\n";
  o += "- Code commit: `" + c.code_commit + "`; L = " + std::to_string(c.levels) + "\n";
  o += "- Tick: " + std::to_string(c.tick_units) + " file price units; median mid " + std::to_string(c.median_mid_ticks) +
       " ticks\n\n";
  if (!warnings.empty()) {
    o += "Warnings:\n";
    for (const std::string& w : warnings) o += "- " + w + "\n";
    o += "\n";
  }

  o += "## Rates (events per second)\n\n| | buy | sell |\n|---|---|---|\n";
  o += "| limit orders (type 1) | " + fixed(c.rates.limit[kBuy].per_s, 3) + " (" +
       std::to_string(c.rates.limit[kBuy].count) + ") | " + fixed(c.rates.limit[kSell].per_s, 3) + " (" +
       std::to_string(c.rates.limit[kSell].count) + ") |\n";
  o += "| aggressive orders (bursts, by aggressor) | " + fixed(c.rates.aggressive[kBuy].per_s, 3) + " (" +
       std::to_string(c.rates.aggressive[kBuy].count) + ") | " + fixed(c.rates.aggressive[kSell].per_s, 3) + " (" +
       std::to_string(c.rates.aggressive[kSell].count) + ") |\n\n";

  o += "## Placement distance Delta (ticks behind the same-side best)\n\n| side | spread | samples | most common "
       "Delta (share) |\n|---|---|---|---|\n";
  for (int s = 0; s < 2; ++s) {
    for (int b = 0; b < kSpreadBuckets; ++b) {
      const Pmf& p = c.placement[s][b];
      o += std::string("| ") + side_name(s) + " | " + spread_bucket_name(b) + " | " + std::to_string(p.total()) + " | " +
           most_common(p, 5) + " |\n";
    }
  }
  o += "\nPower-law fit (report only, the simulator samples the empirical PMF): ";
  for (int s = 0; s < 2; ++s) {
    const auto& f = c.placement_fit[s];
    o += std::string(side_name(s)) + " " +
         (f ? "delta0 = " + fixed(f->delta0, 3) + ", mu = " + fixed(f->mu, 3) + " (" + std::to_string(f->samples) +
                  " samples)"
            : std::string("no fit"));
    if (f && (f->mu <= kFitMuLow + 1e-6 || f->mu >= kFitMuHigh - 1e-6)) o += " (mu at its search bound: not power-law shaped)";
    o += s == kBuy ? "; " : ".\n\n";
  }

  o += "## Sizes (shares)\n\n";
  o += "- Limit orders: " + std::to_string(c.limit_size.total()) + " samples; smallest sizes " + head(c.limit_size, 10) +
       "; " + std::to_string(c.pooled_limit_size) + " pooled above the 99.9th percentile\n";
  o += "- Aggressive orders (total per burst): " + std::to_string(c.aggressive_size.total()) + " samples; smallest " +
       head(c.aggressive_size, 10) + "; " + std::to_string(c.pooled_aggressive_size) + " pooled\n\n";

  o += "## Lifetimes (Kaplan-Meier, submission to full cancellation)\n\n| Delta | orders | cancelled | censored: "
       "executed | window end | left the window | median |\n|---|---|---|---|---|---|---|\n";
  for (int b = 0; b < kDistanceBuckets; ++b) {
    const Survival& s = c.lifetime[b];
    o += std::string("| ") + distance_bucket_name(b) + " | " + std::to_string(s.orders) + " | " +
         std::to_string(s.cancelled) + " | " + std::to_string(s.censored_executed) + " | " +
         std::to_string(s.censored_window_end) + " | " + std::to_string(s.censored_left_window) + " | " +
         duration(s.median_ns()) + " |\n";
  }

  const nlohmann::json& r = c.report;
  o += "\n## Excluded and skipped\n\n";
  if (r.contains("placement")) o += "- Placement exclusions: " + r["placement"]["excluded"].dump() + "\n";
  if (r.contains("tick")) {
    o += "- Subpenny limit orders excluded: " + r["tick"]["subpenny_limit_orders_excluded"].dump() + "\n";
  }
  if (r.contains("lifetime")) {
    const nlohmann::json& l = r["lifetime"];
    o += "- Messages naming an order not tracked (submitted before the window, a seed, or filtered): " +
         l["unknown_id"].dump() + "; after its censoring: " + l["after_censoring"].dump() + "\n";
  }
  if (r.contains("sign_autocorrelation")) {
    const nlohmann::json& a = r["sign_autocorrelation"];
    o += "- Aggressive-order sign autocorrelation (lags 1, 10, 100): " + a["lag_1"].dump() + ", " + a["lag_10"].dump() +
         ", " + a["lag_100"].dump() + "\n";
  }
  if (r.contains("observed")) {
    o += "- Observed message times: " + r["observed"]["first_message_ns"].dump() + " to " +
         r["observed"]["last_message_ns"].dump() + " ns after midnight\n";
  }
  o += "\nThe JSON file beside this one holds every count (its `report`). Known bias: with window-filtered data, "
       "submissions placed outside the published window are never seen, so the limit-order rate and the far tail of "
       "the placement PMF are underestimated.\n";
  return o;
}

}  // namespace calib
