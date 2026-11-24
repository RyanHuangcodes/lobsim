// The market fingerprint (upgrade 3, C4) on tiny hand-written LOBSTER files in tests/fixtures/fingerprint/.
// Every expected value below is worked out by hand in the comments (toy A) or by an independent
// computation of the stated formula (toy B1, B2: the numbers are the logs of the price ratios listed).
#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>

#include "calib/fingerprint.hpp"
#include "sim/rng.hpp"

namespace {
using namespace calib;
const std::string kDir = std::string(LOBSIM_SOURCE_DIR) + "/tests/fixtures/fingerprint/";
const std::string kStem = "_2012-06-21_34200000_57600000_";
constexpr std::int64_t kNs = 1'000'000'000;

Prepared load(const std::string& toy, int levels, std::int64_t tick, FingerprintWindow w = {}) {
  return Prepared::from_files(kDir + toy + kStem + "message_" + std::to_string(levels) + ".csv",
                              kDir + toy + kStem + "orderbook_" + std::to_string(levels) + ".csv", tick, w);
}
double get(const Fingerprint& f, const std::string& name) {
  for (const NamedScalar& s : scalars(f))
    if (s.name == name) return s.value;
  ADD_FAILURE() << "no scalar " << name;
  return 0;
}
FingerprintWindow win(std::int64_t from_s, std::int64_t to_s) { return {from_s * kNs, to_s * kNs}; }

// ---------------------------------------------------------------------------------------------
// Toy A (L = 2, tick 100, window [10 s, 20 s)). Rows (ask1 price/size, bid1, ask2, bid2), by message:
//   t= 9  type 1 id 1       R1: 10200/5 10000/10 10300/7 9900/4   (before the window: the carry-in book)
//   t=12  type 1 id 2 buy 20 @10000  R2: bid1 10000/30
//   t=13  type 1 id 3 sell 6 @10400  R3 = R2 (outside the top 2 levels)
//   t=14  type 1 id 4 sell 5 @10100  R4: ask1 10100/5, ask2 10200/5
//   t=15  type 4 id 2 size 8 (buy resting)  R5: bid1 22      } one burst: aggressor sells
//   t=15  type 4 id 2 size 7                R6: bid1 15      }
//   t=17  type 4 id 4 size 5 (sell resting) R7: asks 10200/5 10300/7, bid1 15  (burst 2: aggressor buys)
//   t=18  type 5 (hidden)                   R8 = R7
//   t=19  type 4 id 2 size 5                R9: bid1 10        (burst 3: aggressor sells)
//   t=20  type 1 id 9                       R10 (at the window's end: not inside [10, 20))
// ---------------------------------------------------------------------------------------------
TEST(FingerprintToyA, SpreadPmfAndTimeAccounting) {
  const Fingerprint f = load("toyA", 2, 100, win(10, 20)).fingerprint();
  // Time in force: R1 [10,12) 2 s, R2 [12,13), R3 [13,14), R4 [14,15) 1 s each, R5 [15,15) 0 s,
  // R6 [15,17) 2 s, R7 [17,18), R8 [18,19), R9 [19,20) 1 s each = 10 s.
  // Spread in ticks: R1,R2,R3 = (10200-10000)/100 = 2; R4,R5,R6 = (10100-10000)/100 = 1; R7,R8,R9 = 2.
  // spread 1: R4 1 s + R6 2 s = 3 s; spread 2: 2 + 1 + 1 + 1 + 1 + 1 = 7 s.
  EXPECT_EQ(f.spread_pmf.values, (std::vector<std::int64_t>{1, 2}));
  EXPECT_EQ(f.spread_pmf.counts, (std::vector<std::uint64_t>{3 * kNs, 7 * kNs}));
  EXPECT_EQ(f.total_ns, 10 * kNs);
  EXPECT_EQ(f.two_sided_ns, 10 * kNs);
  EXPECT_EQ(f.one_sided_ns, 0);
  EXPECT_EQ(f.no_state_ns, 0);
  EXPECT_DOUBLE_EQ(f.spread_mean, (3 * 1 + 7 * 2) / 10.0);  // 1.7
  EXPECT_DOUBLE_EQ(f.spread_one_tick_share, 0.3);
  EXPECT_EQ(f.messages, 8u);  // t = 12, 13, 14, 15, 15, 17, 18, 19
  EXPECT_EQ(f.off_grid_rows, 0u);
}

TEST(FingerprintToyA, DepthProfile) {
  const Fingerprint f = load("toyA", 2, 100, win(10, 20)).fingerprint();
  // Weights (seconds) R1 2, R2 1, R3 1, R4 1, R6 2, R7 1, R8 1, R9 1 (R5 has 0).
  // bid level 1: 10*2 + 30 + 30 + 30 + 15*2 + 15 + 15 + 10 = 180 -> 18.0
  // bid level 2: 4 in every row -> 4.0.  ask level 1: 5 in every row -> 5.0
  // ask level 2: 7*2 + 7 + 7 + 5 + 5*2 + 7 + 7 + 7 = 64 -> 6.4
  EXPECT_DOUBLE_EQ(f.depth[kBuy][0], 18.0);
  EXPECT_DOUBLE_EQ(f.depth[kBuy][1], 4.0);
  EXPECT_DOUBLE_EQ(f.depth[kSell][0], 5.0);
  EXPECT_DOUBLE_EQ(f.depth[kSell][1], 6.4);
  EXPECT_DOUBLE_EQ(get(f, "depth_sell_L2"), 6.4);
}

TEST(FingerprintToyA, TradeSignsAndRates) {
  const Fingerprint f = load("toyA", 2, 100, win(10, 20)).fingerprint();
  // Bursts: {t=15 id2 8, t=15 id2 7} (same time and direction +1 = resting buy) -> aggressor sells, sign -1;
  // t=17 resting sell -> +1; t=19 resting buy (the hidden type 5 at 18 is not a trade) -> -1.
  EXPECT_EQ(f.bursts, 3u);
  // signs (-1, +1, -1): mean -1/3, centered (-2/3, 4/3, -2/3). Lag 1: pairs (c1c2, c2c3) = (-8/9, -8/9),
  // mean -8/9; variance sum(c^2)/3 = (4/9 + 16/9 + 4/9)/3 = 8/9; rho_1 = -1.
  EXPECT_NEAR(f.sign_acf[0], -1.0, 1e-12);
  EXPECT_TRUE(std::isnan(f.sign_acf[1]));  // no pair at lag 10 or 100
  EXPECT_TRUE(std::isnan(f.sign_acf[2]));
  // Rates over 10 s: limit orders buy 1 (id 2), sell 2 (ids 3, 4); bursts: buy aggressor 1, sell aggressor 2.
  EXPECT_DOUBLE_EQ(f.lambda_limit[kBuy], 0.1);
  EXPECT_DOUBLE_EQ(f.lambda_limit[kSell], 0.2);
  EXPECT_DOUBLE_EQ(f.lambda_limit_total, 0.3);
  EXPECT_DOUBLE_EQ(f.lambda_market[kBuy], 0.1);
  EXPECT_DOUBLE_EQ(f.lambda_market[kSell], 0.2);
  EXPECT_DOUBLE_EQ(f.lambda_market_total, 0.3);
}

TEST(FingerprintToyA, FillProbabilityAndExecutedShare) {
  const Fingerprint f = load("toyA", 2, 100, win(10, 20)).fingerprint();
  // id 2: buy 20 @10000, best bid before 10000 -> Delta 0 (bucket <=0); executed 8+7+5 = 20 -> filled.
  // id 3: sell 6 @10400, best ask before 10200 -> Delta 2 (bucket 1-2); never executed.
  // id 4: sell 5 @10100, best ask before 10200 -> Delta -1 (bucket <=0); executed 5 -> filled.
  // id 1 (t=9) and id 9 (t=20) are outside [10, 20).
  EXPECT_EQ(f.fill[0].orders, 2u);
  EXPECT_EQ(f.fill[0].filled, 2u);
  EXPECT_DOUBLE_EQ(f.fill[0].prob, 1.0);
  EXPECT_EQ(f.fill[1].orders, 1u);
  EXPECT_EQ(f.fill[1].filled, 0u);
  EXPECT_DOUBLE_EQ(f.fill[1].prob, 0.0);
  EXPECT_EQ(f.fill[2].orders, 0u);
  EXPECT_TRUE(std::isnan(f.fill[2].prob));
  EXPECT_DOUBLE_EQ(f.fill_all.prob, 2.0 / 3.0);
  EXPECT_EQ(f.placement_unknown, 0u);
  // Shares: submitted 20 + 6 + 5 = 31, executed 20 + 5 = 25.
  EXPECT_EQ(f.submitted_shares, 31u);
  EXPECT_EQ(f.executed_shares, 25u);
  EXPECT_DOUBLE_EQ(f.executed_share, 25.0 / 31.0);
}

TEST(FingerprintToyA, WindowRestriction) {
  const Prepared whole = load("toyA", 2, 100);  // default window: [9 s, 20 s + 1 ns)
  const Fingerprint all = whole.fingerprint();
  EXPECT_EQ(all.from_ns, 9 * kNs);
  EXPECT_EQ(all.to_ns, 20 * kNs + 1);
  EXPECT_EQ(all.messages, 10u);  // every message
  EXPECT_EQ(all.fill_all.orders, 5u);  // ids 1, 2, 3, 4, 9
  // A narrower window [14 s, 18 s): messages at 14, 15, 15, 17 only; the book at 14 is R3 from t=13 (carry-in).
  const Fingerprint w = load("toyA", 2, 100, win(14, 18)).fingerprint();
  EXPECT_EQ(w.messages, 4u);
  EXPECT_EQ(w.total_ns, 4 * kNs);
  // R4 [14,15) spread 1 (1 s), R5 0 s, R6 [15,17) spread 1 (2 s), R7 [17,18) spread 2 (1 s).
  EXPECT_EQ(w.spread_pmf.counts, (std::vector<std::uint64_t>{3 * kNs, 1 * kNs}));
  EXPECT_EQ(w.fill_all.orders, 1u);       // only id 4 was submitted inside; its fill at 17 is inside
  EXPECT_EQ(w.fill_all.filled, 1u);
  EXPECT_EQ(w.bursts, 2u);
  EXPECT_DOUBLE_EQ(w.lambda_market_total, 2.0 / 4.0);
  // The carry-in book gives the placement distance of the first window message: window [13.5, 14.5)
  // holds only id 4 (t=14), whose Delta -1 is measured against R3 (t=13), outside the window.
  const Fingerprint c = Prepared::from_files(kDir + "toyA" + kStem + "message_2.csv", kDir + "toyA" + kStem + "orderbook_2.csv", 100,
                                              FingerprintWindow{13'500'000'000, 14'500'000'000})
                            .fingerprint();
  EXPECT_EQ(c.fill[0].orders, 1u);
  EXPECT_EQ(c.placement_unknown, 0u);
  // A window before the first row: time without a book is counted, not hidden.
  const Fingerprint early = load("toyA", 2, 100, win(7, 11)).fingerprint();
  EXPECT_EQ(early.no_state_ns, 2 * kNs);  // [7, 9)
  EXPECT_EQ(early.two_sided_ns, 2 * kNs);
}

// ---------------------------------------------------------------------------------------------
// Toy B1 (L = 1, tick 1, window [0 s, 8 s)): one message per second, the row after the message at
// second t has bid b_t = 100, 101, 101, 103, (no ask), 102, 104, 103 and ask b_t + 2 (t = 4: the ask level
// is empty, price 9999999999, size 0). Mid sums ask + bid at the clock times 0..8 (time 8 = row of t = 7):
//   202, 204, 204, 208, -, 206, 210, 208, 208
// 1 s returns r_j = ln(S_{j+1} / S_j), j = 0..7:
//   ln(204/202) = 0.0098522964, 0, ln(208/204) = 0.0194180859, missing, missing,
//   ln(210/206) = 0.0192313619, ln(208/210) = -0.0095694510, 0
// ---------------------------------------------------------------------------------------------
TEST(FingerprintToyB1, VolatilityLag1AutocorrelationAndMissingIntervals) {
  const Fingerprint f = load("toyB1", 1, 1, win(0, 8)).fingerprint();
  EXPECT_EQ(f.vol[0].n, 6u);
  EXPECT_EQ(f.vol[0].skipped, 2u);  // the intervals 3->4 and 4->5 have no two-sided mid at one end
  // mean = 0.0390... / 6; sample std (n - 1) = 0.011686991932 (sum of squared deviations / 5).
  EXPECT_NEAR(f.vol[0].stdev, 0.011686991932210324, 1e-12);
  // ACF at lag 1: valid pairs (j, j+1): (0,1) (1,2) (5,6) (6,7), N = 4; rho = (sum of products / 4) /
  // (sum of squared deviations / 6) = -0.452785762384.
  EXPECT_NEAR(f.acf_ret_1s_lag1, -0.4527857623838216, 1e-12);
  // No 10 s or 60 s return fits in 8 s.
  EXPECT_EQ(f.vol[1].n, 0u);
  EXPECT_TRUE(std::isnan(f.vol[1].stdev));
  EXPECT_TRUE(std::isnan(f.kurtosis_10s));
  // The row of t = 4 is one-sided for 1 s; the spread PMF carries the other 7 s (spread 2).
  EXPECT_EQ(f.one_sided_ns, 1 * kNs);
  EXPECT_DOUBLE_EQ(f.one_sided_share, 1.0 / 8.0);
  EXPECT_EQ(f.spread_pmf.values, (std::vector<std::int64_t>{2}));
  EXPECT_EQ(f.spread_pmf.counts, (std::vector<std::uint64_t>{7 * kNs}));
}

// Toy B2 (L = 1, tick 1, window [0 s, 60 s)): messages at 0, 10, ..., 50 s with bid 100, 102, 101, 104, 103,
// 103 and ask b + 2. Mid sums at the 10 s clock times 0..60: 202, 206, 204, 210, 208, 208, 208.
// 10 s returns: ln(206/202) = 0.0196084714, ln(204/206) = -0.0097561749, ln(210/204) = 0.0289875369,
// ln(208/210) = -0.0095694510, 0, 0. n = 6, population moments: excess kurtosis = -1.2275872102,
// sample std = 0.0159294334. The 60 s clock has one return, so its std is undefined.
TEST(FingerprintToyB2, TenSecondMomentsAndAbsoluteAutocorrelation) {
  const Fingerprint f = load("toyB2", 1, 1, win(0, 60)).fingerprint();
  EXPECT_EQ(f.vol[1].n, 6u);
  EXPECT_NEAR(f.vol[1].stdev, 0.015929433384877196, 1e-12);
  EXPECT_NEAR(f.kurtosis_10s, -1.2275872101534633, 1e-10);
  // ACF of |r|, centered at the mean of |r|, pair-count normalized (see docs/FINGERPRINT.md):
  const double want[5] = {0.1427358250851597, -0.07238213010174459, -0.6124885030864647, -0.3553429969289258,
                          -0.8759991019015748};
  for (int k = 0; k < 5; ++k) EXPECT_NEAR(f.acf_abs_10s[static_cast<std::size_t>(k)], want[k], 1e-12) << "lag " << k + 1;
  for (int k = 5; k < 10; ++k) EXPECT_TRUE(std::isnan(f.acf_abs_10s[static_cast<std::size_t>(k)])) << "lag " << k + 1;
  EXPECT_EQ(f.vol[2].n, 1u);
  EXPECT_TRUE(std::isnan(f.vol[2].stdev));
  // 1 s returns: 60 of them, nonzero only at the four 10 s boundaries where the bid moves (j = 9, 19, 29, 39).
  EXPECT_EQ(f.vol[0].n, 60u);
  EXPECT_EQ(f.vol[0].skipped, 0u);
}

TEST(FingerprintInput, Rejects) {
  std::istringstream m("1.000000000,1,1,10,100,1\n"), b("102,5,100,5\n102,5,100,5\n");
  EXPECT_THROW(Prepared::from_streams(m, b, 1), std::runtime_error);  // the orderbook has more rows
  std::istringstream m2("2.000000000,1,1,10,100,1\n1.000000000,1,2,10,100,1\n"), b2("102,5,100,5\n102,5,100,5\n");
  EXPECT_THROW(Prepared::from_streams(m2, b2, 1), std::runtime_error);  // time goes backwards
  std::istringstream m3("1.000000000,1,1,10,100,1\n"), b3("102,5,100\n");
  EXPECT_THROW(Prepared::from_streams(m3, b3, 1), std::runtime_error);  // not a multiple of 4 columns
}

TEST(FingerprintInput, BurstRules) {
  // type 4 messages: a burst is a maximal run of consecutive type 4 with the same time and direction.
  //  t=1 d+1, t=1 d+1            -> burst A (2 messages)
  //  t=1 d-1                     -> burst B (same time, other direction)
  //  t=2 d-1, t=2 d-1            -> burst C (other time)
  //  t=3 d+1 | t=3 type 5 | t=3 d+1  -> bursts D and E (a hidden execution in between breaks the run)
  const char* rows[] = {"1.000000000,4,1,1,100,1", "1.000000000,4,1,1,100,1", "1.000000000,4,2,1,101,-1",
                        "2.000000000,4,2,1,101,-1", "2.000000000,4,2,1,101,-1", "3.000000000,4,1,1,100,1",
                        "3.000000000,5,0,1,100,1", "3.000000000,4,1,1,100,1"};
  std::string m, b;
  for (const char* r : rows) {
    m += std::string(r) + "\n";
    b += "101,5,100,5\n";
  }
  std::istringstream ms(m), bs(b);
  const Fingerprint f = compute_fingerprint(ms, bs, 1, win(1, 4));
  EXPECT_EQ(f.bursts, 5u);
  // aggressor signs: A -1 (resting buy), B +1, C +1, D -1, E -1
  EXPECT_DOUBLE_EQ(f.lambda_market[kBuy], 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(f.lambda_market[kSell], 3.0 / 3.0);
}

TEST(FingerprintFills, AnIdReusedAfterItsOrderEndedIsANewOrder) {
  // The rule of the estimator (D-309): a type 1 whose id is still resting keeps the first order and is
  // counted; one whose order has ended (deleted, or used up by cancels or fills) is a new order.
  const char* rows[] = {
      "0.500000000,1,99,1,90,1",   // before the window: only the book in force when it opens
      "1.000000000,1,1,10,100,1",  // id 1, buy 10 at the best bid: Delta 0
      "2.000000000,3,1,10,100,1",  // deleted
      "3.000000000,1,1,10,100,1",  // the same id again: a new order
      "4.000000000,4,1,4,100,1",   // executed 4 -> only the new order is filled
      "5.000000000,1,2,10,100,1",  // id 2
      "6.000000000,1,2,10,100,1",  // id 2 is still resting: duplicate, the first order keeps it
      "7.000000000,4,2,3,100,1",   // fills id 2's first (only) order
      "8.000000000,1,3,5,100,1",   // id 3
      "9.000000000,4,3,5,100,1",   // used up by a fill: ends
      "10.000000000,1,3,5,100,1",  // a new order
      "11.000000000,1,4,5,100,1",  // id 4
      "12.000000000,2,4,5,100,1",  // used up by a cancel: ends
      "13.000000000,1,4,5,100,1",  // a new order, never filled
  };
  std::string m, b;
  for (const char* r : rows) {
    m += std::string(r) + "\n";
    b += "101,5,100,5\n";
  }
  std::istringstream ms(m), bs(b);
  const Fingerprint f = compute_fingerprint(ms, bs, 1, win(1, 14));
  // Orders: 1 (deleted), 1 again (filled), 2 (filled), 3 (filled), 3 again, 4 (cancelled), 4 again = 7; filled 3.
  EXPECT_EQ(f.fill[0].orders, 7u);
  EXPECT_EQ(f.fill[0].filled, 3u);
  EXPECT_EQ(f.fill_all.orders, 7u);
  EXPECT_EQ(f.reused_order_ids, 3u);
  EXPECT_EQ(f.duplicate_order_ids, 1u);  // id 2's second submission
  // Executed 4 + 3 + 5 = 12 of the submitted 10 + 10 + 10 + 5 + 5 + 5 + 5 = 50 (the duplicate is not an order).
  EXPECT_DOUBLE_EQ(f.executed_share, 12.0 / 50.0);
  // Every type 1 is still an event of the input rate (8 of them, as the estimator counts them) over 13 s.
  EXPECT_DOUBLE_EQ(f.lambda_limit_total, 8.0 / 13.0);
}

TEST(FingerprintDistances, HandChecked) {
  Fingerprint a, b;
  a.spread_pmf = {{1, 2}, {3, 7}};
  b.spread_pmf = {{1, 2}, {7, 3}};
  a.depth = {std::vector<double>{1.0, 1.0}, std::vector<double>{}};
  b.depth = {std::vector<double>{3.0, 1.0}, std::vector<double>{}};
  const DistributionDistances d = distances(a, b);
  // P = (0.3, 0.7), Q = (0.7, 0.3): TV = 0.5 * (0.4 + 0.4) = 0.4; W1 = |0.3 - 0.7| at x = 1 = 0.4 (and 0 at 2).
  EXPECT_TRUE(d.spread.valid);
  EXPECT_NEAR(d.spread.tv, 0.4, 1e-12);
  EXPECT_NEAR(d.spread.w1, 0.4, 1e-12);
  // Depth profiles normalized: (1/2, 1/2) vs (3/4, 1/4): TV = 0.25; W1 = |0.5 - 0.75| = 0.25 levels.
  EXPECT_TRUE(d.depth[kBuy].valid);
  EXPECT_NEAR(d.depth[kBuy].tv, 0.25, 1e-12);
  EXPECT_NEAR(d.depth[kBuy].w1, 0.25, 1e-12);
  EXPECT_FALSE(d.depth[kSell].valid);
}

TEST(FingerprintRuns, CombineRuns) {
  std::vector<Fingerprint> runs(3);
  for (std::size_t i = 0; i < 3; ++i) {
    runs[i].spread_mean = static_cast<double>(i + 1);  // 1, 2, 3
    runs[i].depth = {std::vector<double>{1.0}, std::vector<double>{1.0}};
  }
  const auto s = combine_runs(runs);
  ASSERT_EQ(s[0].name, "spread_mean");
  // mean 2, sd 1, t(0.975, 2) = 4.303: CI half-width 4.303 / sqrt(3) = 2.484; run percentiles 1.05 and 2.95.
  EXPECT_DOUBLE_EQ(s[0].mean, 2.0);
  EXPECT_DOUBLE_EQ(s[0].sd, 1.0);
  EXPECT_NEAR(s[0].ci_hi - 2.0, 4.303 / std::sqrt(3.0), 1e-12);
  EXPECT_NEAR(s[0].ci_lo, 2.0 - 4.303 / std::sqrt(3.0), 1e-12);
  EXPECT_NEAR(s[0].run_lo, 1.05, 1e-12);
  EXPECT_NEAR(s[0].run_hi, 2.95, 1e-12);
  EXPECT_EQ(s[0].n, 3u);
  EXPECT_DOUBLE_EQ(student_t975(30), 2.042);
  EXPECT_NEAR(student_t975(1000), 1.962, 0.001);
}

// ---------------------------------------------------------------------------------------------
// The bootstrap on a generated, well-behaved day: 1 hour, a message every 0.5 s, the bid a random walk.
// ---------------------------------------------------------------------------------------------
struct Generated {
  std::string messages, book;
};
Generated generate(int seconds, std::uint64_t seed) {
  sim::Xoshiro256StarStar rng(seed);
  std::ostringstream m, b;
  std::int64_t bid = 10000;
  std::int64_t id = 1;
  for (int k = 0; k < 2 * seconds; ++k) {
    const std::int64_t t_ms = static_cast<std::int64_t>(k) * 500;
    bid += rng.uniform_int(-1, 1) * 100;
    const std::int64_t ask = bid + 100 * rng.uniform_int(1, 3);
    const int type = rng.uniform_int(0, 5) == 0 ? 4 : 1;
    const int dir = rng.uniform_int(0, 1) == 0 ? -1 : 1;
    const std::int64_t size = rng.uniform_int(1, 20);
    m << t_ms / 1000 << '.' << (t_ms % 1000 == 0 ? "000000000" : "500000000") << ',' << type << ',' << (type == 4 ? id - 1 : id++)
      << ',' << size << ',' << (dir == 1 ? bid : ask) << ',' << dir << '\n';
    b << ask << ',' << rng.uniform_int(1, 30) << ',' << bid << ',' << rng.uniform_int(1, 30) << '\n';
  }
  return {m.str(), b.str()};
}

Prepared prepared_walk(int seconds = 3600, std::uint64_t seed = 5) {
  const Generated g = generate(seconds, seed);
  std::istringstream m(g.messages), b(g.book);
  return Prepared::from_streams(m, b, 100, win(0, seconds));
}

bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

TEST(FingerprintBootstrap, SameSeedSameIntervalsDifferentSeedDiffers) {
  const Prepared p = prepared_walk();
  BootstrapOptions o;
  o.resamples = 200;
  const BootstrapResult a = p.bootstrap(o), b = p.bootstrap(o);
  ASSERT_EQ(a.scalars.size(), b.scalars.size());
  for (std::size_t i = 0; i < a.scalars.size(); ++i) {
    EXPECT_TRUE(same(a.scalars[i].lo, b.scalars[i].lo)) << a.scalars[i].name;
    EXPECT_TRUE(same(a.scalars[i].hi, b.scalars[i].hi)) << a.scalars[i].name;
  }
  o.seed += 1;
  const BootstrapResult c = p.bootstrap(o);
  bool differs = false;
  for (std::size_t i = 0; i < a.scalars.size(); ++i) differs = differs || !same(a.scalars[i].lo, c.scalars[i].lo);
  EXPECT_TRUE(differs);
  EXPECT_EQ(a.blocks_per_resample, 12);  // 3600 s / 300 s
  EXPECT_FALSE(a.degenerate);
}

TEST(FingerprintBootstrap, IntervalContainsPointEstimateAndIsNotEmpty) {
  const Prepared p = prepared_walk();
  BootstrapOptions o;
  o.resamples = 400;
  const BootstrapResult r = p.bootstrap(o);
  int checked = 0;
  for (const ScalarInterval& s : r.scalars) {
    if (s.name != "spread_mean" && s.name != "depth_buy_L1" && s.name != "vol_1s" && s.name != "vol_10s" &&
        s.name != "executed_share" && s.name != "lambda_limit_total" && s.name != "fill_all" && s.name != "lambda_market_total")
      continue;
    ++checked;
    EXPECT_LE(s.lo, s.value) << s.name;
    EXPECT_GE(s.hi, s.value) << s.name;
    EXPECT_LT(s.lo, s.hi) << s.name;
    EXPECT_EQ(s.defined, 400u) << s.name;
  }
  EXPECT_EQ(checked, 8);
}

TEST(FingerprintBootstrap, WindowNoLongerThanOneBlockIsDegenerate) {
  const Prepared p = prepared_walk(120);
  BootstrapOptions o;
  o.resamples = 30;
  const BootstrapResult r = p.bootstrap(o);
  EXPECT_TRUE(r.degenerate);
  for (const ScalarInterval& s : r.scalars)
    if (std::isfinite(s.value)) {
      EXPECT_DOUBLE_EQ(s.lo, s.value) << s.name;  // every resample is the data itself
      EXPECT_DOUBLE_EQ(s.hi, s.value) << s.name;
    }
}

TEST(FingerprintBootstrap, PointEstimateIsTheSumOfItsHalvesAndHalvesYardstick) {
  const Prepared p = prepared_walk();
  const Fingerprint all = p.fingerprint();
  const Fingerprint h1 = p.fingerprint_of_bins(0, 1800), h2 = p.fingerprint_of_bins(1800, 3600);
  EXPECT_EQ(all.total_ns, h1.total_ns + h2.total_ns);
  EXPECT_EQ(all.messages, h1.messages + h2.messages);
  EXPECT_EQ(all.bursts, h1.bursts + h2.bursts);
  EXPECT_EQ(all.fill_all.orders, h1.fill_all.orders + h2.fill_all.orders);
  BootstrapOptions o;
  o.resamples = 100;
  const HalvesYardstick y = p.halves(o), y2 = p.halves(o);
  const DistributionDistances d = distances(h1, h2);
  EXPECT_DOUBLE_EQ(y.point.spread.w1, d.spread.w1);
  EXPECT_DOUBLE_EQ(y.point.spread.tv, d.spread.tv);
  EXPECT_EQ(y.spread_w1.lo, y2.spread_w1.lo);
  EXPECT_EQ(y.spread_w1.hi, y2.spread_w1.hi);
  EXPECT_GT(y.spread_w1.defined, 90u);
  EXPECT_GE(y.spread_w1.hi, y.spread_w1.lo);
  EXPECT_EQ(y.blocks_per_half[0], 6);
}

TEST(FingerprintJson, WritesEveryScalarAndNullForUndefined) {
  const Fingerprint f = load("toyB1", 1, 1, win(0, 8)).fingerprint();
  const nlohmann::json j = nlohmann::json::parse(to_json(f).dump());
  EXPECT_EQ(j.at("scalars").at("vol_1s").get<double>(), f.vol[0].stdev);
  EXPECT_TRUE(j.at("scalars").at("kurtosis_10s").is_null());
  std::ostringstream os;
  write_json(os, f);
  nlohmann::json parsed;
  EXPECT_NO_THROW(parsed = nlohmann::json::parse(os.str()));
  EXPECT_TRUE(parsed.contains("scalars"));
}
}  // namespace
