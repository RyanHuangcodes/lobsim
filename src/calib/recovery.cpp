// Parameter recovery: the comparison of an estimate with the truth it was simulated from (spec
// upgrade 3, section 4.3), and the published window's depth for the filtered comparison.
#include "calib/recovery.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <istream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

#include "replay/lobster.hpp"

namespace calib {
namespace {

const char* side_name(int s) { return s == kBuy ? "buy" : "sell"; }

std::string fixed(double v, int decimals) {
  if (!std::isfinite(v)) return "n/a";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

std::string percent(double v) { return fixed(100.0 * v, 2) + "%"; }

std::string seconds(std::int64_t ns) {
  std::string ms = std::to_string((ns % 1'000'000'000) / 1'000'000);
  ms.insert(0, 3 - ms.size(), '0');
  return std::to_string(ns / 1'000'000'000) + "." + ms + " s";
}

// Share of p's samples above hi.
double share_above(const Pmf& p, std::int64_t hi) {
  std::uint64_t above = 0;
  for (std::size_t i = 0; i < p.values.size(); ++i) {
    if (p.values[i] > hi) above += p.counts[i];
  }
  const std::uint64_t n = p.total();
  return n == 0 ? 0.0 : static_cast<double>(above) / static_cast<double>(n);
}

CompareRow judged(std::string group, std::string item, std::string truth, std::string estimate, std::string measure,
                  double value, double tolerance, std::string note = "") {
  CompareRow r{std::move(group), std::move(item), std::move(truth), std::move(estimate), std::move(measure),
               value, tolerance, Verdict::Pass, std::move(note)};
  r.verdict = std::isfinite(value) && std::fabs(value) <= tolerance ? Verdict::Pass : Verdict::Fail;
  return r;
}

CompareRow info(std::string group, std::string item, std::string truth, std::string estimate, std::string measure,
                double value, std::string note = "") {
  return CompareRow{std::move(group), std::move(item), std::move(truth), std::move(estimate), std::move(measure),
                    value, std::nullopt, Verdict::Info, std::move(note)};
}

CompareRow skipped(std::string group, std::string item, std::string note) {
  return CompareRow{std::move(group), std::move(item), "", "", "", std::numeric_limits<double>::quiet_NaN(),
                    std::nullopt, Verdict::Skipped, std::move(note)};
}

std::string value_text(const CompareRow& r) {
  if (r.verdict == Verdict::Skipped) return "";
  if (r.measure == "relative error" || r.measure == "relative bias") return percent(r.value);
  return fixed(r.value, 4);
}

std::string tolerance_text(const CompareRow& r) {
  if (!r.tolerance) return "";
  if (r.measure == "relative error") return "<= " + percent(*r.tolerance);
  return "<= " + fixed(*r.tolerance, 4);
}

}  // namespace

const char* to_string(Verdict v) noexcept {
  switch (v) {
    case Verdict::Pass: return "PASS";
    case Verdict::Fail: return "FAIL";
    case Verdict::Info: return "info";
    case Verdict::Skipped: return "skipped";
  }
  return "?";
}

std::size_t Comparison::count(Verdict v) const {
  return static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [&](const CompareRow& r) { return r.verdict == v; }));
}

std::size_t Comparison::passed_in(const std::string& group) const {
  return static_cast<std::size_t>(std::count_if(
      rows.begin(), rows.end(), [&](const CompareRow& r) { return r.group == group && r.verdict == Verdict::Pass; }));
}

Pmf restrict_pmf(const Pmf& p, std::int64_t lo, std::optional<std::int64_t> hi) {
  Pmf out;
  for (std::size_t i = 0; i < p.values.size(); ++i) {
    if (p.values[i] < lo || (hi && p.values[i] > *hi)) continue;
    out.values.push_back(p.values[i]);
    out.counts.push_back(p.counts[i]);
  }
  return out;
}

Comparison compare_recovery(const Calibration& truth, const Calibration& est, const CompareOptions& o) {
  if (!std::isfinite(o.tolerance_scale) || o.tolerance_scale <= 0.0) {
    throw std::invalid_argument("the tolerance scale must be a positive number");
  }
  const double k = o.tolerance_scale;
  Comparison c;

  // Rates: per kind and side, relative error of the estimate's per-second rate.
  for (const bool limit : {true, false}) {
    for (int s = 0; s < 2; ++s) {
      const Rate& t = limit ? truth.rates.limit[s] : truth.rates.aggressive[s];
      const Rate& e = limit ? est.rates.limit[s] : est.rates.aggressive[s];
      const std::string item = std::string(limit ? "limit " : "aggressive ") + side_name(s);
      if (t.per_s <= 0.0) {
        c.rows.push_back(skipped("rate", item, "the truth's rate is 0"));
        continue;
      }
      const double rel = e.per_s / t.per_s - 1.0;
      const std::string tt = fixed(t.per_s, 3) + " /s", et = fixed(e.per_s, 3) + " /s (" + std::to_string(e.count) + ")";
      if (o.filtered) c.rows.push_back(info("rate", item, tt, et, "relative bias", rel, "filtered: the bias is reported"));
      else c.rows.push_back(judged("rate", item, tt, et, "relative error", rel, kRateTolerance * k));
    }
  }

  // Placement. The simulator redraws a Delta below -(s - 1), so the PMF observed in a bucket is the
  // truth restricted to the bucket's allowed range and renormalized: [0, inf) for s = 1,
  // [-1, inf) for s = 2. Bucket 3+ mixes spreads with different ranges; it is compared with the
  // range of its smallest spread, s = 3, i.e. [-2, inf), and reported only.
  for (int s = 0; s < 2; ++s) {
    for (int b = 0; b < kSpreadBuckets; ++b) {
      const std::string item = std::string(side_name(s)) + " s=" + spread_bucket_name(b);
      const Pmf& observed_all = est.placement[s][b];
      const std::uint64_t n = observed_all.total();
      const std::int64_t lo = -b;  // -(s - 1) with s = b + 1: 0, -1, -2
      const std::optional<std::int64_t> hi = o.filtered ? o.max_delta[s] : std::nullopt;
      const Pmf allowed = restrict_pmf(truth.placement[s][b], lo, std::nullopt);
      if (allowed.empty()) {
        c.rows.push_back(skipped("placement", item, "the truth has no Delta in the allowed range"));
        continue;
      }
      if (n < o.min_samples) {
        c.rows.push_back(skipped("placement", item,
                                 std::to_string(n) + " samples, fewer than " + std::to_string(o.min_samples)));
        continue;
      }
      const Pmf expected = restrict_pmf(allowed, lo, hi);
      const Pmf observed = hi ? restrict_pmf(observed_all, std::numeric_limits<std::int64_t>::min(), hi) : observed_all;
      const std::string against = std::string("truth on [") + std::to_string(lo) + ", " +
                                  (hi ? std::to_string(*hi) : std::string("inf")) + "], renormalized";
      if (observed.empty()) {
        c.rows.push_back(CompareRow{"placement", item, against, std::to_string(n) + " samples, none in range", "TV",
                                    1.0, b < 2 ? std::optional(kPlacementTolerance * k) : std::nullopt,
                                    b < 2 ? Verdict::Fail : Verdict::Info, ""});
        continue;
      }
      const double tv = total_variation(observed, expected);
      const std::string et = std::to_string(observed.total()) + " samples";
      if (b < 2) {
        c.rows.push_back(judged("placement", item, against, et, "TV", tv, kPlacementTolerance * k));
      } else {
        c.rows.push_back(info("placement", item, against, et, "TV", tv,
                              "s >= 3 mixes allowed ranges: compared with the s = 3 range, informational"));
      }
      if (hi) {
        // The tail the window comparison leaves out: the estimate's share above the limit against
        // the truth's share above it (within the allowed range).
        const double es = share_above(observed_all, *hi), ts = share_above(allowed, *hi);
        c.rows.push_back(info("placement tail", item + " Delta>" + std::to_string(*hi), percent(ts), percent(es),
                              "relative bias", ts > 0.0 ? es / ts - 1.0 : std::numeric_limits<double>::quiet_NaN(),
                              "share of the bucket above the window's p95 depth"));
      }
    }
  }

  // Sizes.
  for (const bool limit : {true, false}) {
    const Pmf& t = limit ? truth.limit_size : truth.aggressive_size;
    const Pmf& e = limit ? est.limit_size : est.aggressive_size;
    const std::string item = limit ? "limit_size" : "aggressive_size";
    const std::uint64_t pooled = limit ? est.pooled_limit_size : est.pooled_aggressive_size;
    if (t.empty()) {
      c.rows.push_back(skipped("size", item, "the truth's PMF is empty"));
      continue;
    }
    if (e.empty()) {
      c.rows.push_back(CompareRow{"size", item, "", "no samples", "TV", 1.0, kSizeTolerance * k, Verdict::Fail, ""});
      continue;
    }
    c.rows.push_back(judged("size", item, std::to_string(t.values.size()) + " values",
                            std::to_string(e.total()) + " samples", "TV", total_variation(e, t), kSizeTolerance * k,
                            pooled > 0 ? std::to_string(pooled) + " pooled into the 99.9th percentile" : ""));
  }

  // Lifetime medians.
  const double life_tol = (o.filtered ? kLifetimeFilteredTolerance : kLifetimeTolerance) * k;
  for (int b = 0; b < kDistanceBuckets; ++b) {
    const std::string item = distance_bucket_name(b);
    const std::optional<std::int64_t> tm = truth.lifetime[b].median_ns();
    const std::optional<std::int64_t> em = est.lifetime[b].median_ns();
    const Survival& s = est.lifetime[b];
    const std::string counts = std::to_string(s.cancelled) + " cancelled, censored: " +
                               std::to_string(s.censored_executed) + " executed, " +
                               std::to_string(s.censored_left_window) + " left the window, " +
                               std::to_string(s.censored_window_end) + " at the end";
    if (!tm) {
      c.rows.push_back(skipped("lifetime median", item, "the truth's curve never reaches 0.5"));
      continue;
    }
    if (!em) {
      c.rows.push_back(CompareRow{"lifetime median", item, seconds(*tm), "not reached", "relative error",
                                  std::numeric_limits<double>::quiet_NaN(), life_tol, Verdict::Fail, counts});
      continue;
    }
    const double rel = static_cast<double>(*em) / static_cast<double>(*tm) - 1.0;
    c.rows.push_back(judged("lifetime median", item, seconds(*tm), seconds(*em), "relative error", rel, life_tol, counts));
  }

  // The power-law fit, reported only (the simulator samples the empirical PMF).
  for (int s = 0; s < 2; ++s) {
    const auto& t = truth.placement_fit[s];
    const auto& e = est.placement_fit[s];
    if (!t || !e) continue;
    c.rows.push_back(info("fit", side_name(s), "delta0 " + fixed(t->delta0, 3) + ", mu " + fixed(t->mu, 3),
                          "delta0 " + fixed(e->delta0, 3) + ", mu " + fixed(e->mu, 3), "mu difference", e->mu - t->mu,
                          "fitted on Delta >= 0 of all buckets, report only"));
  }
  return c;
}

std::string comparison_summary(const Comparison& c) {
  return std::string("result: ") + (c.passed() ? "PASS" : "FAIL") + " (" + std::to_string(c.count(Verdict::Pass)) +
         " passed, " + std::to_string(c.count(Verdict::Fail)) + " failed, " + std::to_string(c.count(Verdict::Info)) +
         " informational, " + std::to_string(c.count(Verdict::Skipped)) + " skipped)";
}

std::string comparison_text(const Comparison& c) {
  std::string out;
  char line[512];
  std::snprintf(line, sizeof line, "%-16s %-22s %-32s %-30s %-10s %-11s %-8s %s\n", "group", "item", "truth",
                "estimate", "value", "tolerance", "verdict", "note");
  out += line;
  for (const CompareRow& r : c.rows) {
    std::snprintf(line, sizeof line, "%-16s %-22s %-32s %-30s %-10s %-11s %-8s %s\n", r.group.c_str(),
                  r.item.c_str(), r.truth.c_str(), r.estimate.c_str(), value_text(r).c_str(),
                  tolerance_text(r).c_str(), to_string(r.verdict), r.note.c_str());
    out += line;
  }
  return out + comparison_summary(c) + "\n";
}

std::string comparison_markdown(const Comparison& c) {
  std::string out = "| group | item | truth | estimate | measure | value | tolerance | verdict | note |\n"
                    "|---|---|---|---|---|---|---|---|---|\n";
  for (const CompareRow& r : c.rows) {
    out += "| " + r.group + " | " + r.item + " | " + r.truth + " | " + r.estimate + " | " + r.measure + " | " +
           value_text(r) + " | " + tolerance_text(r) + " | " + to_string(r.verdict) + " | " + r.note + " |\n";
  }
  return out + "\n" + comparison_summary(c) + "\n";
}

WindowDepth window_depth(std::istream& orderbook, int levels, std::int64_t tick_units) {
  if (levels < 1) throw std::invalid_argument("window_depth: levels must be at least 1");
  if (tick_units < 1) throw std::invalid_argument("window_depth: tick_units must be at least 1");
  WindowDepth d;
  d.levels = levels;
  std::array<std::map<std::int64_t, std::uint64_t>, 2> hist{};
  std::string text;
  std::uint64_t line = 0;
  const auto L = static_cast<std::size_t>(levels);
  while (std::getline(orderbook, text)) {
    ++line;
    const replay::Parsed<replay::BookRow> row = replay::parse_book_line(text, levels);
    if (!row.ok()) throw std::runtime_error("orderbook line " + std::to_string(line) + ": " + row.error);
    if (row.oracle_malformed) {
      ++d.malformed;
      continue;
    }
    const std::vector<replay::BookLevel>& bids = row.value->bids;
    const std::vector<replay::BookLevel>& asks = row.value->asks;
    // A well-formed side shows all L levels exactly when the L-th is not empty.
    if (!bids[L - 1].empty()) {
      ++hist[kBuy][(bids[0].price - bids[L - 1].price) / tick_units];
      ++d.rows[kBuy];
    } else {
      ++d.fewer_levels[kBuy];
    }
    if (!asks[L - 1].empty()) {
      ++hist[kSell][(asks[L - 1].price - asks[0].price) / tick_units];
      ++d.rows[kSell];
    } else {
      ++d.fewer_levels[kSell];
    }
  }
  if (orderbook.bad()) throw std::runtime_error("read error in the orderbook file");
  for (int s = 0; s < 2; ++s) {
    const std::uint64_t n = d.rows[s];
    if (n == 0) continue;
    const std::uint64_t rank = n - n / 20;  // ceil(0.95 n)
    std::uint64_t cum = 0;
    for (const auto& [depth, count] : hist[s]) {
      cum += count;
      if (cum >= rank) {
        d.p95[s] = depth;
        break;
      }
    }
  }
  return d;
}

}  // namespace calib
