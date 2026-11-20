// Unit tests for the viz exporter core (upgrade 4, phase V1). Fast and self-contained: the
// end-to-end hash / PnL / bundle-size checks are exercised by FEAT-004 and the manual proofs.
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "viz_core.hpp"

namespace {

// Worked example from the spec: 100 resting for the first 100 ms of a 250 ms column, then 300
// for the remaining 150 ms, then nothing => (100*100 + 300*150) / 250 = 220.
TEST(viz_heatmap, TimeWeightedMeanWorkedExample) {
  const viz::TimeNs ms = 1'000'000;
  std::vector<std::pair<double, viz::TimeNs>> segments = {
      {100.0, 100 * ms},  // 0..100 ms
      {300.0, 150 * ms},  // 100..250 ms
  };
  EXPECT_DOUBLE_EQ(viz::time_weighted_mean(segments, 250 * ms), 220.0);
}

TEST(viz_heatmap, TimeWeightedMeanEmptyIsZero) {
  EXPECT_DOUBLE_EQ(viz::time_weighted_mean({}, 250), 0.0);
  EXPECT_DOUBLE_EQ(viz::time_weighted_mean({{500.0, 10}}, 0), 0.0);
}

// Log encoding: 0 -> 0, v_max -> 255, and a tiny positive value maps to at least 1.
TEST(viz_encoding, LogScaleEndpoints) {
  const double v_max = 500.0;
  EXPECT_EQ(viz::encode_log_u8(0.0, v_max), 0);
  EXPECT_EQ(viz::encode_log_u8(v_max, v_max), 255);
  EXPECT_GE(viz::encode_log_u8(1e-6, v_max), 1);
  EXPECT_GE(viz::encode_log_u8(1.0, v_max), 1);
  // Monotone non-decreasing between the endpoints.
  EXPECT_LE(viz::encode_log_u8(10.0, v_max), viz::encode_log_u8(100.0, v_max));
}

TEST(viz_encoding, Int32OverflowIsHardError) {
  EXPECT_NO_THROW(viz::to_i32_checked(2'000'000'000, "x"));
  EXPECT_THROW(viz::to_i32_checked(3'000'000'000LL, "x"), std::overflow_error);
}

TEST(viz_encoding, TimeBeyond2Pow53IsHardError) {
  EXPECT_NO_THROW(viz::time_to_f64_checked(1'000'000'000));
  EXPECT_THROW(viz::time_to_f64_checked(std::int64_t{1} << 53), std::overflow_error);
}

// A small hand-made orders.log, replayed independently against an OrderBook: the exporter's
// ladder snapshot at three chosen times must equal OrderBook::depth() at the matching state.
TEST(viz_ladder, SnapshotEqualsDepth) {
  // Header then plain resting limit orders (type 0 Limit, tif 0 GTC, post_only 0, stp 0).
  //   fields: <time> N <owner> <coid> <side 0=buy/1=sell> <type> <tif> <price> <qty> <post> <stp>
  const std::string log =
      "# lobsim orders.log v1 max_price=1000000\n"
      "1000 N 1 1 0 0 0 100 5 0 0\n"   // buy 100 x5
      "2000 N 2 2 0 0 0 99 7 0 0\n"    // buy 99 x7
      "3000 N 3 3 1 0 0 105 4 0 0\n"   // sell 105 x4
      "4000 N 4 4 1 0 0 106 9 0 0\n";  // sell 106 x9

  // Replay directly to get the depth at the same three boundaries (after each listed request).
  auto depth_at = [&](std::size_t requests_applied) {
    lob::OrderBook book(lob::BookConfig{1000000});
    lob::EventBuffer ev;
    struct Req { lob::Side side; lob::Price px; lob::Qty qty; };
    const std::vector<Req> reqs = {{lob::Side::Buy, 100, 5},
                                   {lob::Side::Buy, 99, 7},
                                   {lob::Side::Sell, 105, 4},
                                   {lob::Side::Sell, 106, 9}};
    for (std::size_t i = 0; i < requests_applied; ++i) {
      ev.clear();
      lob::NewOrder n;
      n.owner = static_cast<lob::OwnerId>(i + 1);
      n.client_order_id = i + 1;
      n.side = reqs[i].side;
      n.price = reqs[i].px;
      n.qty = reqs[i].qty;
      book.submit(n, ev);
    }
    std::vector<std::pair<lob::Price, lob::Qty>> bids, asks;
    for (const auto& lv : book.depth(lob::Side::Buy, 10)) bids.emplace_back(lv.price, lv.total_qty);
    for (const auto& lv : book.depth(lob::Side::Sell, 10)) asks.emplace_back(lv.price, lv.total_qty);
    return std::make_pair(bids, asks);
  };

  // Times chosen so each falls strictly between request boundaries (state after N requests).
  const std::vector<viz::TimeNs> times = {1500, 2500, 5000};  // after 1, after 2, after 4
  const auto snaps = viz::sample_ladder(log, times, 10);
  ASSERT_EQ(snaps.size(), 3u);

  const auto [b1, a1] = depth_at(1);
  EXPECT_EQ(snaps[0].bids, b1);
  EXPECT_EQ(snaps[0].asks, a1);

  const auto [b2, a2] = depth_at(2);
  EXPECT_EQ(snaps[1].bids, b2);
  EXPECT_EQ(snaps[1].asks, a2);

  const auto [b4, a4] = depth_at(4);
  EXPECT_EQ(snaps[2].bids, b4);
  EXPECT_EQ(snaps[2].asks, a4);
}

}  // namespace
