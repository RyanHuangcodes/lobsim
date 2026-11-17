// The metrics of SPEC 6: pure helpers on hand-made data, and the observer on tiny scripted
// markets whose trades are known in advance. Every expected number is worked out in a
// comment. Scripted agents use zero latency, so every request arrives at the second the
// script names; scripted events sit on half seconds where the periodic Sample events (on
// whole seconds) must not tie with them.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/metrics.hpp"
#include "support/sim_support.hpp"

using namespace sim;
using lob::Side;
using stats::Series;

namespace {

constexpr TimeNs sec(double s) { return static_cast<TimeNs>(s * 1e9); }

std::vector<std::string> split(const std::string& line, char sep = ',') {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string l;
  while (std::getline(in, l)) out.push_back(l);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------
TEST(MetricsStats, MeanAndSampleSd) {
  // {2,4,4,4,5,5,7,9}: mean 5; squared deviations 9+1+1+1+0+0+4+16 = 32; sd = sqrt(32/7).
  const std::vector<double> x{2, 4, 4, 4, 5, 5, 7, 9};
  EXPECT_DOUBLE_EQ(*stats::mean(x), 5.0);
  EXPECT_NEAR(*stats::sample_sd(x), std::sqrt(32.0 / 7.0), 1e-12);
  EXPECT_FALSE(stats::mean({}));
  EXPECT_FALSE(stats::sample_sd(std::vector<double>{3.0}));
}

TEST(MetricsStats, ExcessKurtosisOfKnownSamples) {
  // {1,2,3,4,5}: mean 3, deviations -2..2, m2 = 10/5 = 2, m4 = (16+1+0+1+16)/5 = 6.8,
  // kurtosis 6.8 / 4 = 1.7, excess 1.7 - 3 = -1.3.
  EXPECT_NEAR(*stats::excess_kurtosis(std::vector<double>{1, 2, 3, 4, 5}), -1.3, 1e-12);
  // {1,1,0,0}: mean .5, every deviation +-.5, m2 = .25, m4 = .0625, ratio 1, excess -2.
  EXPECT_NEAR(*stats::excess_kurtosis(std::vector<double>{1, 1, 0, 0}), -2.0, 1e-12);
  // {0,0,0,0,0,0,0,0,0,10}: mean 1; deviations nine of -1, one of 9; m2 = (9 + 81)/10 = 9;
  // m4 = (9 + 6561)/10 = 657; 657 / 81 = 8.1111...; excess = 5.1111...
  EXPECT_NEAR(*stats::excess_kurtosis(std::vector<double>{0, 0, 0, 0, 0, 0, 0, 0, 0, 10}), 657.0 / 81.0 - 3.0, 1e-12);
  // No variance, or fewer than four values: undefined.
  EXPECT_FALSE(stats::excess_kurtosis(std::vector<double>{2, 2, 2, 2}));
  EXPECT_FALSE(stats::excess_kurtosis(std::vector<double>{1, 2, 3}));
}

TEST(MetricsStats, AutocorrelationOfAnAlternatingSeries) {
  // {1,-1,1,-1,1,-1}: mean 0, denominator 6. Lag 1: five pairs of product -1 -> -5/6.
  // Lag 2: four pairs of product +1 -> 4/6.
  const Series x{1.0, -1.0, 1.0, -1.0, 1.0, -1.0};
  EXPECT_NEAR(*stats::autocorrelation(x, 1), -5.0 / 6.0, 1e-12);
  EXPECT_NEAR(*stats::autocorrelation(x, 2), 4.0 / 6.0, 1e-12);
}

TEST(MetricsStats, AutocorrelationOfAConstantDifferenceSeries) {
  // {1,2,3,4,5}: mean 3, deviations -2,-1,0,1,2, denominator 10.
  // Lag 1: (-2)(-1) + (-1)(0) + (0)(1) + (1)(2) = 4 -> 0.4.
  // Lag 2: (-2)(0) + (-1)(1) + (0)(2) = -1 -> -0.1.
  const Series x{1.0, 2.0, 3.0, 4.0, 5.0};
  EXPECT_NEAR(*stats::autocorrelation(x, 1), 0.4, 1e-12);
  EXPECT_NEAR(*stats::autocorrelation(x, 2), -0.1, 1e-12);
  EXPECT_FALSE(stats::autocorrelation(x, 5)) << "no pair at a lag as long as the series";
}

TEST(MetricsStats, AutocorrelationUsesOnlyPairsWhereBothTermsExist) {
  // {2, -, 4, 6}: present values 2,4,6, mean 4, deviations -2,0,2, denominator 8.
  // Lag 1: pairs (1,2) and (0,1) lack a term; (2,3) has both: (0)(2) = 0 -> 0.
  // Lag 3: pair (0,3): (-2)(2) = -4 -> -0.5.
  const Series x{2.0, std::nullopt, 4.0, 6.0};
  EXPECT_NEAR(*stats::autocorrelation(x, 1), 0.0, 1e-12);
  EXPECT_NEAR(*stats::autocorrelation(x, 3), -0.5, 1e-12);
  EXPECT_FALSE(stats::autocorrelation(Series{3.0, 3.0, 3.0}, 1)) << "zero variance";
}

TEST(MetricsStats, LogReturnsExistOnlyBetweenConsecutivePresentSamples) {
  const Series mids{100.0, std::nullopt, 110.0, 121.0};
  const Series r = stats::log_returns(mids);
  ASSERT_EQ(r.size(), 3u);
  EXPECT_FALSE(r[0]);  // 100 -> missing
  EXPECT_FALSE(r[1]);  // missing -> 110
  EXPECT_NEAR(*r[2], std::log(1.1), 1e-12);
}

TEST(MetricsStats, TimeWeightedMeanAndPercentilesOfAStepFunction) {
  // The spread was 2 for 5 time units, 3 for 2 units and 4 for 1 unit (total 8).
  // Mean (10 + 6 + 4) / 8 = 2.5. A quantile is the smallest value whose cumulative weight
  // reaches p * 8: p=.5 -> 4: cumulative 5 at value 2 -> 2; p=.75 -> 6: 5 < 6, 7 >= 6 -> 3;
  // p=.9 -> 7.2: 7 < 7.2, 8 -> 4; p=.99 and p=1 -> 4.
  const stats::Weighted w{{2, 5}, {3, 2}, {4, 1}};
  EXPECT_DOUBLE_EQ(*stats::weighted_mean(w), 2.5);
  EXPECT_EQ(*stats::weighted_quantile(w, 0.5), 2);
  EXPECT_EQ(*stats::weighted_quantile(w, 0.75), 3);
  EXPECT_EQ(*stats::weighted_quantile(w, 0.9), 4);
  EXPECT_EQ(*stats::weighted_quantile(w, 0.99), 4);
  EXPECT_EQ(*stats::weighted_quantile(w, 1.0), 4);
  EXPECT_FALSE(stats::weighted_mean({}));
  EXPECT_FALSE(stats::weighted_quantile({}, 0.5));
}

TEST(MetricsStats, ConfidenceIntervalFormula) {
  // n = 100, mean 2, sd 5: half-width 1.96 * 5 / sqrt(100) = 0.98.
  const auto ci = stats::confidence_interval_95(100, 2.0, 5.0);
  ASSERT_TRUE(ci);
  EXPECT_NEAR(ci->lo, 1.02, 1e-12);
  EXPECT_NEAR(ci->hi, 2.98, 1e-12);
  // n = 4, sd 3: half-width 1.96 * 3 / 2 = 2.94.
  EXPECT_NEAR(stats::confidence_interval_95(4, 0.0, 3.0)->hi, 2.94, 1e-12);
  EXPECT_FALSE(stats::confidence_interval_95(1, 2.0, 5.0));
  EXPECT_FALSE(stats::confidence_interval_95(0, 0.0, 0.0));
}

TEST(MetricsStats, MarkoutSignForABuyAndForASell) {
  // Bought at 100 and the mid is later 101.5: +1.5. Sold at 100 and the mid is later 101.5:
  // the price rose against the seller: -1.5. A falling mid reverses both.
  EXPECT_DOUBLE_EQ(stats::markout_per_lot(Side::Buy, 100, 101.5), 1.5);
  EXPECT_DOUBLE_EQ(stats::markout_per_lot(Side::Sell, 100, 101.5), -1.5);
  EXPECT_DOUBLE_EQ(stats::markout_per_lot(Side::Buy, 100, 99.0), -1.0);
  EXPECT_DOUBLE_EQ(stats::markout_per_lot(Side::Sell, 100, 99.0), 1.0);
}

// ---------------------------------------------------------------------------------------
// Scripted markets
// ---------------------------------------------------------------------------------------
namespace {

using Script = std::vector<std::pair<TimeNs, lob::Request>>;

struct Scripted {
  SimConfig cfg;
  MetricsSummary summary;
  std::string mm_fills_csv, bbo_csv, summary_json;
  std::int64_t inventory{0};
};

// Agent 0 is the market maker (owner 1) with fees maker -0.002 and taker +0.003 ticks per
// lot; the other scripts are counterparties (owners 2, 3, ...).
Scripted run_script(double duration_s, double warmup_s, std::vector<double> horizons_s, Script mm,
                    std::vector<Script> others) {
  SimConfig cfg = simtest::empty_config(duration_s);
  cfg.warmup_s = warmup_s;
  cfg.markout_horizons_s = std::move(horizons_s);
  finalize(cfg);
  Simulator s(cfg);
  s.add_agent(std::make_unique<simtest::ScriptAgent>(std::move(mm)), LatencySpec{}, StreamRole::Test, 0);
  std::uint32_t i = 1;
  for (auto& script : others)
    s.add_agent(std::make_unique<simtest::ScriptAgent>(std::move(script)), LatencySpec{}, StreamRole::Test, i++);

  MetricsOptions o = make_metrics_options(cfg);
  o.mm_owner = 1;
  o.maker_fee_ticks = -0.002;
  o.taker_fee_ticks = 0.003;
  o.roles = {Role::MarketMaker, Role::Noise, Role::Noise};
  MetricsObserver metrics(o);
  s.add_observer(&metrics);
  s.run();

  Scripted r;
  r.cfg = cfg;
  r.summary = metrics.summary();
  std::ostringstream fills, bbo;
  metrics.write_mm_fills_csv(fills);
  metrics.write_bbo_csv(bbo);
  r.mm_fills_csv = fills.str();
  r.bbo_csv = bbo.str();
  r.summary_json = metrics.summary_json(cfg, s.exchange().event_hash());
  r.inventory = metrics.mm_inventory();
  return r;
}

lob::Request mkt(Side side, lob::Qty qty) { return simtest::market(side, qty); }
lob::Request lim(Side side, lob::Price p, lob::Qty q) { return simtest::limit(side, p, q); }

// The ledger scenario. Market maker M (owner 1), quoter C1 (owner 2), aggressor C2 (owner 3).
//   t=1  C1 rests bid 100 x10 (id 1) and ask 104 x10 (id 2)             mid 102
//   t=2  M market buy 3  -> trade 1: 3 @ 104, M is taker (before the warm-up of 2.5 s)
//   t=3  M market sell 3 -> trade 2: 3 @ 100, M is taker
//   t=4  M rests bid 101 x4                                              mid 102.5
//   t=5  C2 market sell 4 -> trade 3: 4 @ 101, M is maker                mid 102 again
Scripted ledger() {
  return run_script(100, 2.5, {1.0, 5.0},
                    {{sec(2), mkt(Side::Buy, 3)}, {sec(3), mkt(Side::Sell, 3)}, {sec(4), lim(Side::Buy, 101, 4)}},
                    {{{sec(1), lim(Side::Buy, 100, 10)}, {sec(1), lim(Side::Sell, 104, 10)}},
                     {{sec(5), mkt(Side::Sell, 4)}}});
}

}  // namespace

TEST(MetricsScenario, CashInventoryFeesAndPnlAfterABuyThenASell) {
  const Scripted r = ledger();
  const auto& t = r.summary.mm_total;
  // Cash: -3*104 + 3*100 - 4*101 = -312 + 300 - 404 = -416.
  EXPECT_EQ(t.cash, -416);
  // Inventory: +3 -3 +4 = 4. Fees: 3 * .003 + 3 * .003 + 4 * (-.002) = .018 - .008 = .010.
  EXPECT_EQ(t.final_inventory, 4);
  EXPECT_NEAR(t.fees, 0.010, 1e-12);
  // Last mid: bid 100 x7, ask 104 x7 -> 102. pnl = -416 + 4 * 102 - .010 = -8.010.
  EXPECT_NEAR(t.inventory_value, 408.0, 1e-12);
  EXPECT_NEAR(t.pnl_total, -8.010, 1e-9);
  EXPECT_EQ(t.fills, 3u);
  EXPECT_EQ(t.buy_fills, 2u);
  EXPECT_EQ(t.sell_fills, 1u);
  EXPECT_EQ(t.volume_buy, 7);
  EXPECT_EQ(t.volume_sell, 3);
  EXPECT_EQ(t.max_abs_inventory, 4);  // 3, 0, 4
  EXPECT_EQ(r.inventory, 4);
}

TEST(MetricsScenario, FillsBeforeTheWarmupAreOutsideTheWindowButMoveInventory) {
  const Scripted r = ledger();
  const auto& w = r.summary.mm_window;
  // The t=2 buy precedes the warm-up (2.5 s): the window holds the sell and the last buy.
  EXPECT_EQ(w.fills, 2u);
  EXPECT_EQ(w.buy_fills, 1u);
  EXPECT_EQ(w.sell_fills, 1u);
  EXPECT_EQ(w.volume_buy, 4);
  EXPECT_EQ(w.volume_sell, 3);
  // pnl(2.5 s) = -312 + 3 * 102 - .009 = -6.009, so pnl after warm-up = -8.010 + 6.009 = -2.001.
  EXPECT_NEAR(w.pnl_after_warmup, -2.001, 1e-9);
  // Inventory at the start of the window is 3 (the early fill moved it), then 0, then 4.
  EXPECT_EQ(w.max_abs_inventory, 4);
  // The early fill is in mm_fills.csv with in_window 0, and its inventory_after is 3.
  const auto rows = lines(r.mm_fills_csv);
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[0], "time_ns,trade_id,side,price,qty,role,inventory_after,in_window,mid_before,markout_1s,markout_5s");
  EXPECT_EQ(rows[1], "2000000000,1,buy,104,3,taker,3,0,102.0,-2.0,-2.0");
}

TEST(MetricsScenario, AMarkoutResolvedAtExactlyTPlusHUsesTheMidAfterAllEventsAtThatTime) {
  const Scripted r = ledger();
  // The t=3 sell at 100 with h = 1 s is due at t = 4, the very second M rests its bid 101:
  // the mid after that event is (101 + 104) / 2 = 102.5, so the markout is 100 - 102.5 = -2.5.
  // (The mid before that event was 102, which would give -2.0.) With h = 5 s it is due at 8:
  // mid 102 -> -2.0.
  const auto rows = lines(r.mm_fills_csv);
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[2], "3000000000,2,sell,100,3,taker,0,1,102.0,-2.5,-2.0");
  // The maker fill at t=5: bought 4 @ 101; mid 102 at t=6 and at t=10 -> +1.0 and +1.0.
  EXPECT_EQ(rows[3], "5000000000,3,buy,101,4,maker,4,1,102.5,1.0,1.0");
}

TEST(MetricsScenario, MarkoutStatisticsOverTheWindowFills) {
  const Scripted r = ledger();
  ASSERT_EQ(r.summary.markouts.size(), 2u);
  // h = 1 s: window markouts -2.5 (qty 3) and +1.0 (qty 4). mean -0.75; deviations -1.75
  // and +1.75, sample sd = sqrt(2 * 1.75^2) = 2.4749; weighted (-7.5 + 4) / 7 = -0.5;
  // CI half-width 1.96 * 2.4749 / sqrt(2) = 3.43.
  const MarkoutStats& m1 = r.summary.markouts[0];
  EXPECT_EQ(m1.n, 2u);
  EXPECT_EQ(m1.skipped, 0u);
  EXPECT_NEAR(*m1.mean, -0.75, 1e-12);
  EXPECT_NEAR(*m1.sd, std::sqrt(2.0) * 1.75, 1e-12);
  EXPECT_NEAR(*m1.mean_qty_weighted, -0.5, 1e-12);
  EXPECT_NEAR(*m1.ci_hi - *m1.mean, 1.96 * std::sqrt(2.0) * 1.75 / std::sqrt(2.0), 1e-12);
  EXPECT_NEAR(*m1.mean - *m1.ci_lo, *m1.ci_hi - *m1.mean, 1e-12);
  // h = 5 s: -2.0 (qty 3) and +1.0 (qty 4): mean -0.5, weighted (-6 + 4) / 7 = -2/7.
  const MarkoutStats& m5 = r.summary.markouts[1];
  EXPECT_NEAR(*m5.mean, -0.5, 1e-12);
  EXPECT_NEAR(*m5.mean_qty_weighted, -2.0 / 7.0, 1e-12);
  EXPECT_DOUBLE_EQ(m5.horizon_s, 5.0);
}

TEST(MetricsScenario, MarkoutSplitsIntoSpreadCaptureAndMidMoveUsingThePreTradeMid) {
  const Scripted r = ledger();
  // Window fills: the sell at t=3 and the maker buy at t=5.
  // Sell 3 @ 100 at t=3: mid before = 102 (bid 100, ask 104). spread_capture = 100 - 102 = -2.
  //   h=1: m(4) = 102.5 -> mid_move = 102 - 102.5 = -0.5, markout -2.5. h=5: m(8) = 102 -> 0, markout -2.
  // Buy 4 @ 101 at t=5 (M's own bid, the best bid, is fully filled): mid BEFORE = (101 + 104) / 2
  //   = 102.5, mid AFTER the trade = (100 + 104) / 2 = 102. spread_capture = 102.5 - 101 = 1.5
  //   (using the post-trade mid would give 1.0). h=1: m(6) = 102 -> mid_move = 102 - 102.5 = -0.5,
  //   markout 1.5 - 0.5 = 1.0; h=5: m(10) = 102 -> -0.5, markout 1.0.
  // Spread capture over the window: -2 and 1.5, mean -0.25, qty-weighted (-6 + 6) / 7 = 0.
  const SpreadCaptureStats& c = r.summary.spread_capture;
  EXPECT_EQ(c.n, 2u);
  EXPECT_NEAR(*c.mean, -0.25, 1e-12);
  EXPECT_NEAR(*c.mean_qty_weighted, 0.0, 1e-12);
  const Decomposition& d1 = r.summary.markouts[0].decomposition;
  EXPECT_EQ(d1.n, 2u);
  EXPECT_NEAR(*d1.markout_mean, -0.75, 1e-12);
  EXPECT_NEAR(*d1.spread_capture_mean, -0.25, 1e-12);
  EXPECT_NEAR(*d1.mid_move_mean, -0.5, 1e-12);  // (-0.5 + -0.5) / 2
  EXPECT_NEAR(*d1.mid_move_sd, 0.0, 1e-12);
  EXPECT_NEAR(*d1.mid_move_mean_qty_weighted, -0.5, 1e-12);
  EXPECT_NEAR(*d1.spread_capture_mean + *d1.mid_move_mean, *d1.markout_mean, 1e-12);
  const Decomposition& d5 = r.summary.markouts[1].decomposition;
  EXPECT_NEAR(*d5.mid_move_mean, -0.25, 1e-12);  // (0 + -0.5) / 2
  EXPECT_NEAR(*d5.spread_capture_mean + *d5.mid_move_mean, *d5.markout_mean, 1e-12);
  EXPECT_NEAR(*d5.markout_mean, -0.5, 1e-12);
  // The JSON carries the same block.
  const auto j = nlohmann::json::parse(r.summary_json);
  EXPECT_EQ(j.at("spread_capture").at("n").get<int>(), 2);
  EXPECT_NEAR(j.at("markouts").at(0).at("decomposition").at("mid_move_mean").get<double>(), -0.5, 1e-9);
}

TEST(MetricsScenario, AFillWithNoMidBeforeIsSkippedInTheDecompositionButKeepsItsMarkout) {
  // t=1 C1 rests only an ask 104 x10: no mid has ever existed. t=2 M buys 1 @ 104: mid_before
  // undefined. t=3 C1 rests bid 100: mid 102. h = 1 s: due t=3, m = 102, markout = 102 - 104 = -2.
  const Scripted r = run_script(20, 0, {1.0}, {{sec(2), mkt(Side::Buy, 1)}},
                                {{{sec(1), lim(Side::Sell, 104, 10)}, {sec(3), lim(Side::Buy, 100, 10)}}});
  const MarkoutStats& m = r.summary.markouts[0];
  EXPECT_EQ(m.n, 1u);
  EXPECT_NEAR(*m.mean, -2.0, 1e-12);
  EXPECT_EQ(m.decomposition.n, 0u);
  EXPECT_FALSE(m.decomposition.mid_move_mean);
  EXPECT_EQ(r.summary.spread_capture.n, 0u);
  EXPECT_FALSE(r.summary.spread_capture.mean);
  EXPECT_EQ(lines(r.mm_fills_csv)[1], "2000000000,1,buy,104,1,taker,1,1,,-2.0");
}

TEST(MetricsScenario, AFillWhileTheBookIsOneSidedHasNoMidBeforeEvenThoughAMidExistedEarlier) {
  // t=1 C1 rests bid 100 x10 and ask 104 x1: mid 102. t=2 C2 buys 1 @ 104: the ask side is empty,
  // no mid in force. t=3 M sells 1 @ 100 into the lone bid: mid_before must be undefined (not the
  // stale 102), so no spread capture is counted. t=4 C1 rests ask 106: mid 103. h = 1 s: due t=4,
  // m = 103, markout of the sell = 100 - 103 = -3, still reported.
  const Scripted r = run_script(20, 0, {1.0}, {{sec(3), mkt(Side::Sell, 1)}},
                                {{{sec(1), lim(Side::Buy, 100, 10)}, {sec(1), lim(Side::Sell, 104, 1)},
                                  {sec(4), lim(Side::Sell, 106, 10)}},
                                 {{sec(2), mkt(Side::Buy, 1)}}});
  const MarkoutStats& m = r.summary.markouts[0];
  EXPECT_EQ(m.n, 1u);
  EXPECT_NEAR(*m.mean, -3.0, 1e-12);
  EXPECT_EQ(m.decomposition.n, 0u);
  EXPECT_EQ(r.summary.spread_capture.n, 0u);
  EXPECT_FALSE(r.summary.spread_capture.mean);
  const auto rows = lines(r.mm_fills_csv);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[1].substr(0, 10), "3000000000");
  EXPECT_NE(rows[1].find(",sell,100,1,taker,-1,1,,-3.0"), std::string::npos) << rows[1];
}

TEST(MetricsScenario, ReturnMeanAndSdInSummaryJsonKeepSixSignificantDigits) {
  // Mid 102 from t=0.5, then 101 from t=1.5 (a new ask at 102): sampled returns exist.
  const Scripted r = run_script(4, 0, {1.0}, {},
                                {{{sec(0.5), lim(Side::Buy, 100, 10)}, {sec(0.5), lim(Side::Sell, 104, 10)},
                                  {sec(1.5), lim(Side::Sell, 102, 10)}}});
  ASSERT_TRUE(r.summary.returns.mean);
  ASSERT_TRUE(r.summary.returns.sd);
  const auto j = nlohmann::json::parse(r.summary_json);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6e", *r.summary.returns.mean);
  EXPECT_EQ(j.at("returns").at("mean").get<double>(), std::strtod(buf, nullptr));
  std::snprintf(buf, sizeof buf, "%.6e", *r.summary.returns.sd);
  EXPECT_EQ(j.at("returns").at("sd").get<double>(), std::strtod(buf, nullptr));
}

TEST(MetricsScenario, BboRowsCarryInventoryAndMarkToMarketPnl) {
  const Scripted r = ledger();
  const auto rows = lines(r.bbo_csv);
  ASSERT_GT(rows.size(), 8u);
  EXPECT_EQ(rows[0], "time_ns,bid,bid_qty,ask,ask_qty,mid,fundamental,mm_inventory,mm_pnl");
  // First data row: t = 0, an empty book: empty bid, ask and mid fields, inventory 0, pnl 0.
  const auto first = split(rows[1]);
  ASSERT_EQ(first.size(), 9u);
  EXPECT_EQ(first[0], "0");
  EXPECT_EQ(first[1], "");
  EXPECT_EQ(first[5], "");
  EXPECT_EQ(first[6], "10000.000000");
  EXPECT_EQ(first[7], "0");
  // The sample at t = 6: bid 100 x7, ask 104 x7, mid 102.0, inventory 4, pnl -8.010.
  std::vector<std::string> at6;
  for (std::size_t i = 1; i < rows.size(); ++i)
    if (rows[i].rfind("6000000000,", 0) == 0) at6 = split(rows[i]);
  ASSERT_EQ(at6.size(), 9u);
  EXPECT_EQ(at6[1], "100");
  EXPECT_EQ(at6[2], "7");
  EXPECT_EQ(at6[3], "104");
  EXPECT_EQ(at6[4], "7");
  EXPECT_EQ(at6[5], "102.0");
  EXPECT_EQ(at6[7], "4");
  EXPECT_EQ(at6[8], "-8.010000");
}

TEST(MetricsScenario, SamplesWithAnUndefinedMidOrBeyondTheEndAreSkippedAndCounted) {
  // Market maker M (owner 1), quoter C1 (owner 2); duration 20 s, no warm-up, h = 1 and 5 s.
  //   t=1    C1 rests bid 100 x10 (id 1) and ask 104 x10 (id 2)
  //   t=10   M buys 2 @ 104 (fill 1)
  //   t=10.5 C1 cancels its ask (id 2): the book has no ask, the mid is undefined
  //   t=11.5 C1 rests ask 105 x5; mid (100 + 105) / 2 = 102.5
  //   t=12   M buys 1 @ 105 (fill 2)
  //   t=19   M sells 1 @ 100 (fill 3): due at 20 with h = 1 s: exactly T, resolved
  //   t=19.5 M sells 1 @ 100 (fill 4): due at 20.5 > T, skipped
  const Scripted r = run_script(
      20, 0, {1.0, 5.0},
      {{sec(10), mkt(Side::Buy, 2)}, {sec(12), mkt(Side::Buy, 1)}, {sec(19), mkt(Side::Sell, 1)}, {sec(19.5), mkt(Side::Sell, 1)}},
      {{{sec(1), lim(Side::Buy, 100, 10)}, {sec(1), lim(Side::Sell, 104, 10)}, {sec(10.5), lob::Cancel{0, 2}},
        {sec(11.5), lim(Side::Sell, 105, 5)}}});
  ASSERT_EQ(r.summary.markouts.size(), 2u);
  // h = 1 s. fill 1: due 11, the ask is gone -> skipped. fill 2: due 13, mid 102.5, bought 105
  // -> -2.5. fill 3: due 20 == T, mid 102.5, sold 100 -> 100 - 102.5 = -2.5. fill 4: due 20.5
  // > T -> skipped. n = 2, skipped = 2, mean -2.5, sd 0.
  const MarkoutStats& m1 = r.summary.markouts[0];
  EXPECT_EQ(m1.n, 2u);
  EXPECT_EQ(m1.skipped, 2u);
  EXPECT_NEAR(*m1.mean, -2.5, 1e-12);
  EXPECT_NEAR(*m1.sd, 0.0, 1e-12);
  // h = 5 s. fill 1: due 15, mid 102.5, bought 104 -> -1.5. fill 2: due 17 -> -2.5. fills 3
  // and 4 end after T. n = 2, skipped = 2, mean -2.0.
  const MarkoutStats& m5 = r.summary.markouts[1];
  EXPECT_EQ(m5.n, 2u);
  EXPECT_EQ(m5.skipped, 2u);
  EXPECT_NEAR(*m5.mean, -2.0, 1e-12);
  const auto rows = lines(r.mm_fills_csv);
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[1], "10000000000,1,buy,104,2,taker,2,1,102.0,,-1.5");
  EXPECT_EQ(rows[4], "19500000000,4,sell,100,1,taker,1,1,102.5,,");
  EXPECT_TRUE(r.summary_json.find("\"skipped\": 2") != std::string::npos);
}

TEST(MetricsScenario, AMarkoutWithASingleSampleHasNoConfidenceInterval) {
  // One fill, one resolvable horizon: n = 1 -> mean present, sd and CI null.
  const Scripted r = run_script(20, 0, {1.0}, {{sec(5), mkt(Side::Buy, 1)}},
                                {{{sec(1), lim(Side::Buy, 100, 10)}, {sec(1), lim(Side::Sell, 104, 10)}}});
  const MarkoutStats& m = r.summary.markouts[0];
  EXPECT_EQ(m.n, 1u);
  EXPECT_NEAR(*m.mean, -2.0, 1e-12);  // bought 104, mid 102
  EXPECT_FALSE(m.sd);
  EXPECT_FALSE(m.ci_lo);
  EXPECT_FALSE(m.ci_hi);
}

TEST(MetricsScenario, QuotingFractionAndQuotedSpreadForAScriptedSequenceOfQuotes) {
  // Only M trades; duration 20 s, warm-up 2 s (window 18 s). Orders on half seconds.
  //   t=3.5  bid 99 x2 (id 1)     t=5.5  ask 103 x2 (id 2): both sides from 5.5
  //   t=9.5  cancel the bid       t=12.5 bid 100 x2 (id 3)
  //   t=15.5 modify the ask to 102 (qty 2)
  // Quoting both: [5.5, 9.5) spread 4 for 4 s; [12.5, 15.5) spread 3 for 3 s;
  // [15.5, 20) spread 2 for 4.5 s. Total 11.5 s of 18 s = 0.638889; average
  // (4*4 + 3*3 + 2*4.5) / 11.5 = 34 / 11.5 = 2.956522. M is the whole book, so the market
  // spread is the quoted spread: median: cumulative 4.5 (spread 2) < 5.75, 7.5 (3) >= 5.75
  // -> 3; p90: target 10.35: 11.5 at spread 4 -> 4.
  const Scripted r = run_script(20, 2, {1.0},
                                {{sec(3.5), lim(Side::Buy, 99, 2)},
                                 {sec(5.5), lim(Side::Sell, 103, 2)},
                                 {sec(9.5), lob::Cancel{0, 1}},
                                 {sec(12.5), lim(Side::Buy, 100, 2)},
                                 {sec(15.5), lob::Modify{0, 2, 102, 2}}},
                                {});
  const auto& w = r.summary.mm_window;
  EXPECT_NEAR(*w.quoting_fraction, 11.5 / 18.0, 1e-9);
  EXPECT_NEAR(*w.avg_quoted_spread, 34.0 / 11.5, 1e-9);
  const SpreadStats& sp = r.summary.spread;
  EXPECT_NEAR(*sp.two_sided_fraction, 11.5 / 18.0, 1e-9);
  EXPECT_NEAR(*sp.mean, 34.0 / 11.5, 1e-9);
  EXPECT_EQ(*sp.median, 3);
  EXPECT_EQ(*sp.p90, 4);
  EXPECT_EQ(*sp.p99, 4);
  ASSERT_EQ(sp.histogram.size(), static_cast<std::size_t>(kSpreadHistogramMax));
  EXPECT_NEAR(sp.histogram[1], 4.5 / 11.5, 1e-9);  // spread 2
  EXPECT_NEAR(sp.histogram[2], 3.0 / 11.5, 1e-9);  // spread 3
  EXPECT_NEAR(sp.histogram[3], 4.0 / 11.5, 1e-9);  // spread 4
  EXPECT_NEAR(sp.overflow, 0.0, 1e-12);
  EXPECT_EQ(r.summary.mm_total.fills, 0u);
  EXPECT_EQ(r.summary.markouts[0].n, 0u);
  EXPECT_FALSE(r.summary.markouts[0].mean);
}

TEST(MetricsScenario, ReturnsFromTheSampledMidsSkipMissingSamples) {
  // Same market as above. Samples at k = 2..20 s (19, the last one taken at T). The mid is
  // defined at 6..9 (101), 13..15 (101.5), 16..20 (101): 12 present, 7 missing.
  // Returns exist between consecutive present samples: 6->7, 7->8, 8->9, 13->14, 14->15 (all
  // zero), 15->16 = ln(101 / 101.5), 16->17 .. 19->20 (four zeros): 10 returns, 9 zero.
  const Scripted r = run_script(20, 2, {1.0},
                                {{sec(3.5), lim(Side::Buy, 99, 2)},
                                 {sec(5.5), lim(Side::Sell, 103, 2)},
                                 {sec(9.5), lob::Cancel{0, 1}},
                                 {sec(12.5), lim(Side::Buy, 100, 2)},
                                 {sec(15.5), lob::Modify{0, 2, 102, 2}}},
                                {});
  const ReturnStats& rs = r.summary.returns;
  EXPECT_EQ(rs.samples, 19u);
  EXPECT_EQ(rs.missing, 7u);
  EXPECT_EQ(rs.returns, 10u);
  EXPECT_NEAR(*rs.zero_fraction, 0.9, 1e-12);
  EXPECT_NEAR(*rs.mean, std::log(101.0 / 101.5) / 10.0, 1e-12);
  EXPECT_EQ(rs.acf.size(), 5u);
  EXPECT_EQ(rs.abs_acf.size(), 10u);
}

TEST(MetricsScenario, WithoutAMarketMakerTheMmOutputsAreEmptyAndNothingCrashes) {
  const simtest::RunFiles f = simtest::run_with_metrics(simtest::noise_config(4, 5, 20.0));
  EXPECT_EQ(lines(f.mm_fills_csv).size(), 1u) << "header only";
  const auto bbo = lines(f.bbo_csv);
  ASSERT_GT(bbo.size(), 5u);
  EXPECT_EQ(bbo[5].substr(bbo[5].size() - 2), ",,") << "mm_inventory and mm_pnl are empty";
  const auto j = nlohmann::json::parse(f.summary_json);
  EXPECT_TRUE(j.at("market_maker").is_null());
  EXPECT_TRUE(j.at("markouts").empty());
  EXPECT_GT(j.at("counts").at("trades").get<int>(), 0);
}

TEST(MetricsRun, CountsAreConsistentInAFullRun) {
  SimConfig cfg = simtest::default_config();
  cfg.duration_s = 60;
  cfg.warmup_s = 10;
  finalize(cfg);
  Simulator s(cfg);
  MetricsObserver metrics(make_metrics_options(cfg));
  s.add_observer(&metrics);
  s.run();
  const MetricsSummary m = metrics.summary();
  const RunCounts& c = m.counts;
  EXPECT_EQ(c.trades, s.exchange().trades());
  EXPECT_EQ(c.requests_new + c.requests_cancel + c.requests_modify, s.exchange().requests());
  EXPECT_EQ(c.ev_ack + c.ev_reject + c.ev_trade + c.ev_cancelled + c.ev_modified, s.exchange().events());
  std::int64_t taker = 0, maker = 0;
  for (const RoleCounts& r : c.roles) {
    taker += r.taker_volume;
    maker += r.maker_volume;
  }
  EXPECT_EQ(taker, c.traded_volume);
  EXPECT_EQ(maker, c.traded_volume);
  ASSERT_TRUE(s.market_maker_index());
  EXPECT_EQ(m.mm_total.fills, m.mm_total.buy_fills + m.mm_total.sell_fills);
  EXPECT_EQ(m.mm_total.volume_buy - m.mm_total.volume_sell, m.mm_total.final_inventory);
  EXPECT_LE(m.mm_window.fills, m.mm_total.fills);
  EXPECT_EQ(c.roles[2].maker_volume + c.roles[2].taker_volume, m.mm_total.volume_buy + m.mm_total.volume_sell);
  // The windowed spread histogram sums to 1 over the two-sided time.
  double sum = m.spread.overflow;
  for (double h : m.spread.histogram) sum += h;
  EXPECT_NEAR(sum, 1.0, 1e-9);
  // The decomposition identity on every window fill of the written file, and in the summary.
  for (const MarkoutStats& k : m.markouts) {
    const Decomposition& d = k.decomposition;
    ASSERT_GT(d.n, 50u);
    EXPECT_LE(d.n, k.n);
    EXPECT_NEAR(*d.spread_capture_mean + *d.mid_move_mean, *d.markout_mean, 1e-9);
  }
  {
    std::ostringstream fills;
    metrics.write_mm_fills_csv(fills);
    const auto rows = lines(fills.str());
    int checked = 0;
    for (std::size_t i = 1; i < rows.size(); ++i) {
      const auto f = split(rows[i]);
      ASSERT_EQ(f.size(), 8u + 1u + 3u);
      if (f[8].empty() || f[9].empty()) continue;
      const double price = std::stod(f[3]), mid_before = std::stod(f[8]), markout = std::stod(f[9]);
      const double capture = f[2] == "buy" ? mid_before - price : price - mid_before;
      const double later = f[2] == "buy" ? markout + price : price - markout;  // m(t + 1 s)
      const double move = f[2] == "buy" ? later - mid_before : mid_before - later;
      EXPECT_NEAR(capture + move, markout, 1e-9) << rows[i];
      ++checked;
    }
    EXPECT_GT(checked, 50);
  }
  const auto js = nlohmann::json::parse(metrics.summary_json(cfg, 0));
  for (const auto& mo : js.at("markouts")) {
    const auto& d = mo.at("decomposition");
    EXPECT_NEAR(d.at("spread_capture_mean").get<double>() + d.at("mid_move_mean").get<double>(),
                d.at("markout_mean").get<double>(), 2e-6);
  }
  // pnl_total = cash + inventory value - fees.
  EXPECT_NEAR(m.mm_total.pnl_total, static_cast<double>(m.mm_total.cash) + m.mm_total.inventory_value - m.mm_total.fees, 1e-6);
}
