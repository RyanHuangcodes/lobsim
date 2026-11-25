// Parameter recovery (upgrade 3, C3): the comparison rules of spec section 4.3, the published
// window's depth, and the committed truth file.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>

#include "calib/calibration.hpp"
#include "calib/recovery.hpp"

namespace {

namespace fs = std::filesystem;
using calib::kBuy;
using calib::kSell;
using calib::Verdict;

const fs::path kTruth = fs::path(LOBSIM_SOURCE_DIR) / "calibration" / "recovery_truth.json";

calib::Calibration truth() { return calib::load(kTruth.string()); }

const calib::CompareRow& row(const calib::Comparison& c, const std::string& group, const std::string& item) {
  for (const calib::CompareRow& r : c.rows) {
    if (r.group == group && r.item == item) return r;
  }
  ADD_FAILURE() << "no row " << group << " / " << item;
  return c.rows.front();
}

// An estimate that is the truth itself, with sample counts: rates count-free, PMFs at the truth's
// weights (which are large, so every placement bucket has enough samples).
calib::Calibration estimate_like_truth() {
  calib::Calibration e = truth();
  e.source = "synthetic";
  return e;
}

// ---------------------------------------------------------------- the truth file

TEST(RecoveryTruth, IsTheHandMadeTruthOfTheSpec) {
  const calib::Calibration t = truth();
  EXPECT_EQ(t.source, "hand-made");
  EXPECT_EQ(t.tick_units, 1);
  EXPECT_EQ(t.median_mid_ticks, 10000);
  for (int s = 0; s < 2; ++s) {
    EXPECT_DOUBLE_EQ(t.rates.limit[s].per_s, 20.0);
    EXPECT_DOUBLE_EQ(t.rates.aggressive[s].per_s, 2.0);
    // Bucket "1": the power law on 0..30 only; "2": 10% at -1; "3+": 10% on -1..-4.
    const calib::Pmf& p1 = t.placement[s][0];
    EXPECT_EQ(p1.values.front(), 0);
    EXPECT_EQ(p1.values.back(), 30);
    const calib::Pmf& p2 = t.placement[s][1];
    EXPECT_EQ(p2.values.front(), -1);
    EXPECT_NEAR(static_cast<double>(p2.counts.front()) / static_cast<double>(p2.total()), 0.10, 1e-9);
    const calib::Pmf& p3 = t.placement[s][2];
    EXPECT_EQ(p3.values.front(), -4);
    EXPECT_NEAR(static_cast<double>(calib::restrict_pmf(p3, -4, -1).total()) / static_cast<double>(p3.total()), 0.10,
                1e-9);
    // (1 + Delta)^-1.6: the ratio of Delta = 0 to Delta = 1 is 2^1.6, up to the rounding to 10^9.
    EXPECT_NEAR(static_cast<double>(p1.counts[0]) / static_cast<double>(p1.counts[1]), std::pow(2.0, 1.6), 1e-6);
  }
  // Every bucket's curve has the median 20 ln(1.4) s = 6.7294 s, to within the grid (+0.008%).
  const double exact = 20.0 * std::log(1.4) * 1e9;
  for (const calib::Survival& s : t.lifetime) {
    ASSERT_TRUE(s.median_ns().has_value());
    EXPECT_EQ(*s.median_ns(), 6'730'000'000);
    EXPECT_LT(std::fabs(static_cast<double>(*s.median_ns()) / exact - 1.0), 0.005);
    EXPECT_EQ(s.survival.back(), 0.0);  // every order gets a cancel
  }
}

TEST(RecoveryTruth, PassesAgainstItself) {
  const calib::Comparison c = calib::compare_recovery(truth(), estimate_like_truth(), {});
  EXPECT_TRUE(c.passed()) << calib::comparison_text(c);
  EXPECT_EQ(c.count(Verdict::Fail), 0u);
  EXPECT_EQ(c.passed_in("rate"), 4u);
  EXPECT_EQ(c.passed_in("placement"), 4u);  // buckets 1 and 2 per side; 3+ is informational
  EXPECT_EQ(c.passed_in("size"), 2u);
  EXPECT_EQ(c.passed_in("lifetime median"), 4u);
}

// ---------------------------------------------------------------- rules

TEST(RecoveryCompare, RatesWithinThreePercentTimesTheScale) {
  calib::Calibration e = estimate_like_truth();
  e.rates.limit[kBuy].per_s = 20.0 * 1.029;
  e.rates.limit[kSell].per_s = 20.0 * 0.969;
  calib::Comparison c = calib::compare_recovery(truth(), e, {});
  EXPECT_EQ(row(c, "rate", "limit buy").verdict, Verdict::Pass);
  EXPECT_EQ(row(c, "rate", "limit sell").verdict, Verdict::Fail);
  calib::CompareOptions twice;
  twice.tolerance_scale = 2.0;
  c = calib::compare_recovery(truth(), e, twice);
  EXPECT_EQ(row(c, "rate", "limit sell").verdict, Verdict::Pass);
  calib::CompareOptions filtered;
  filtered.filtered = true;
  c = calib::compare_recovery(truth(), e, filtered);
  EXPECT_EQ(row(c, "rate", "limit sell").verdict, Verdict::Info);  // the bias is reported
  EXPECT_NEAR(row(c, "rate", "limit sell").value, -0.031, 1e-12);
}

TEST(RecoveryCompare, PlacementIsComparedWithTheTruthRestrictedToTheAllowedRange) {
  // Bucket 3+ as the simulator would produce it at s = 3: the truth without -3 and -4, renormalized.
  calib::Calibration e = estimate_like_truth();
  for (int s = 0; s < 2; ++s) e.placement[s][2] = calib::restrict_pmf(e.placement[s][2], -2, std::nullopt);
  calib::Comparison c = calib::compare_recovery(truth(), e, {});
  const calib::CompareRow r3 = row(c, "placement", "buy s=3+");
  EXPECT_EQ(r3.verdict, Verdict::Info);
  EXPECT_NEAR(r3.value, 0.0, 1e-12);
  EXPECT_EQ(r3.truth, "truth on [-2, inf], renormalized");
  // The raw truth in bucket 3+ is 5% away from that: -3 and -4 carry 2.5% each.
  c = calib::compare_recovery(truth(), estimate_like_truth(), {});
  EXPECT_NEAR(row(c, "placement", "buy s=3+").value, 0.05, 1e-9);
  // Bucket 2 judged: moving 4% of the mass from Delta = 0 to Delta = 5 is TV 0.04.
  calib::Calibration m = estimate_like_truth();
  calib::Pmf& p = m.placement[kSell][1];
  const std::uint64_t shift = p.total() / 25;
  p.counts[1] -= shift;  // values[1] is Delta = 0
  p.counts[6] += shift;  // values[6] is Delta = 5
  c = calib::compare_recovery(truth(), m, {});
  EXPECT_NEAR(row(c, "placement", "sell s=2").value, 0.04, 1e-6);
  EXPECT_EQ(row(c, "placement", "sell s=2").verdict, Verdict::Fail);
  EXPECT_EQ(row(c, "placement", "buy s=2").verdict, Verdict::Pass);
}

TEST(RecoveryCompare, ABucketWithFewSamplesIsSkippedNotJudged) {
  calib::Calibration e = estimate_like_truth();
  e.placement[kBuy][1] = calib::Pmf{{0, 1}, {1000, 999}};
  const calib::Comparison c = calib::compare_recovery(truth(), e, {});
  EXPECT_EQ(row(c, "placement", "buy s=2").verdict, Verdict::Skipped);
  EXPECT_NE(row(c, "placement", "buy s=2").note.find("1999 samples"), std::string::npos);
}

TEST(RecoveryCompare, FilteredPlacementStopsAtTheWindowDepthAndReportsTheTail) {
  // The estimate lost everything above Delta = 10, as a 10-level window would hide it.
  calib::Calibration e = estimate_like_truth();
  for (int s = 0; s < 2; ++s) {
    for (int b = 0; b < calib::kSpreadBuckets; ++b) e.placement[s][b] = calib::restrict_pmf(e.placement[s][b], -4, 10);
  }
  calib::CompareOptions o;
  o.filtered = true;
  o.max_delta = {10, 10};
  const calib::Comparison c = calib::compare_recovery(truth(), e, o);
  EXPECT_EQ(row(c, "placement", "buy s=1").verdict, Verdict::Pass);
  EXPECT_NEAR(row(c, "placement", "buy s=1").value, 0.0, 1e-12);
  const calib::CompareRow tail = row(c, "placement tail", "buy s=1 Delta>10");
  EXPECT_EQ(tail.verdict, Verdict::Info);
  EXPECT_DOUBLE_EQ(tail.value, -1.0);  // none of the truth's tail is seen
  // Unfiltered, the same estimate fails: the tail is missing.
  EXPECT_EQ(row(calib::compare_recovery(truth(), e, {}), "placement", "buy s=1").verdict, Verdict::Fail);
}

TEST(RecoveryCompare, SizesAndLifetimeMedians) {
  calib::Calibration e = estimate_like_truth();
  e.aggressive_size.counts.back() += e.aggressive_size.total() / 40;  // about 2.4% more at 5000
  e.lifetime[0] = calib::Survival{};                                  // never reaches 0.5
  e.lifetime[1].times_ns = {7'400'000'000};
  e.lifetime[1].survival = {0.4};
  e.lifetime[2].times_ns = {7'500'000'000};
  e.lifetime[2].survival = {0.4};
  calib::Comparison c = calib::compare_recovery(truth(), e, {});
  EXPECT_EQ(row(c, "size", "limit_size").verdict, Verdict::Pass);
  EXPECT_EQ(row(c, "size", "aggressive_size").verdict, Verdict::Fail);
  EXPECT_EQ(row(c, "lifetime median", "<=0").verdict, Verdict::Fail);
  EXPECT_EQ(row(c, "lifetime median", "<=0").estimate, "not reached");
  EXPECT_EQ(row(c, "lifetime median", "1-2").verdict, Verdict::Pass);  // +9.96%
  EXPECT_EQ(row(c, "lifetime median", "3-9").verdict, Verdict::Fail);  // +11.4%
  calib::CompareOptions filtered;
  filtered.filtered = true;
  c = calib::compare_recovery(truth(), e, filtered);
  EXPECT_EQ(row(c, "lifetime median", "3-9").verdict, Verdict::Pass);  // within 20%
  EXPECT_NE(calib::comparison_markdown(c).find("result: FAIL"), std::string::npos);
}

TEST(RecoveryCompare, BadScaleIsRejected) {
  calib::CompareOptions o;
  o.tolerance_scale = 0.0;
  EXPECT_THROW(calib::compare_recovery(truth(), truth(), o), std::invalid_argument);
}

// ---------------------------------------------------------------- window depth

TEST(WindowDepth, NearestRank95thPercentilePerSide) {
  // L = 2. Bid depth (best - 2nd) over 20 full rows: 1 nineteen times and 7 once: the 95th
  // percentile by nearest rank is the 19th of 20, i.e. 1. Asks show one level only: no depth.
  std::string text;
  for (int i = 0; i < 19; ++i) text += "105,1,100,1,0,0,99,1\n";
  text += "105,1,100,1,0,0,93,1\n";
  text += "105,1,100,1,0,0,0,0\n";  // fewer bid levels
  text += "104,1,100,1,103,1,99,1\n";  // asks out of order: malformed, skipped
  std::istringstream in(text);
  const calib::WindowDepth d = calib::window_depth(in, 2, 1);
  ASSERT_TRUE(d.p95[kBuy].has_value());
  EXPECT_EQ(*d.p95[kBuy], 1);
  EXPECT_EQ(d.rows[kBuy], 20u);
  EXPECT_EQ(d.fewer_levels[kBuy], 1u);
  EXPECT_FALSE(d.p95[kSell].has_value());
  EXPECT_EQ(d.fewer_levels[kSell], 21u);
  EXPECT_EQ(d.malformed, 1u);
  // With 21 full rows the rank is 21 - 1 = 20: still 1; one more deep row makes rank 20 of 22 a 7.
  std::istringstream more(text + "105,1,100,1,0,0,93,1\n" + "105,1,100,1,0,0,93,1\n");
  EXPECT_EQ(*calib::window_depth(more, 2, 1).p95[kBuy], 7);
  // In ticks: the same rows at 50 units per tick.
  std::istringstream ticks("1050,1,1000,1,0,0,650,1\n");
  EXPECT_EQ(*calib::window_depth(ticks, 2, 50).p95[kBuy], 7);
  std::istringstream bad("1,2,3\n");
  EXPECT_THROW(calib::window_depth(bad, 2, 1), std::runtime_error);
}

}  // namespace
