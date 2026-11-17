#include <gtest/gtest.h>

#include <sstream>
#include <tuple>
#include <variant>
#include <vector>

#include "sim/agents/informed_trader.hpp"
#include "support/fake_context.hpp"
#include "support/sim_support.hpp"

using namespace sim;
using simtest::FakeCtx;

namespace {

constexpr double kTheta = 2.0;
constexpr lob::Qty kSize = 5;

InformedTraderParams params() {
  InformedTraderParams p;
  p.lambda = 1.0;
  p.theta_ticks = kTheta;
  p.size = kSize;
  return p;
}

// A fundamental that never moves (sigma = 0): V = `v` for the whole test.
Fundamental fixed(double v) { return Fundamental(v, 0.0, 0.1, Xoshiro256StarStar(1)); }

// Starts the trader, gives it `bbo`, fires one wakeup, and returns the orders sent.
std::vector<lob::Request> wake_with(double v, const BBO& bbo) {
  const Fundamental f = fixed(v);
  InformedTrader t(params(), f);
  FakeCtx ctx(5);
  t.on_start(ctx);
  t.on_market_data(ctx, bbo);
  ctx.fire(t);
  return ctx.sent;
}

const lob::NewOrder& order(const lob::Request& r) { return std::get<lob::NewOrder>(r); }

}  // namespace

TEST(InformedTrader, NoBboYetMeansNoOrder) {
  EXPECT_TRUE(wake_with(1000.0, BBO{}).empty());
  EXPECT_TRUE(wake_with(1000.0, BBO{0, 0, 0, 0}).empty());
}

TEST(InformedTrader, OneSidedBboEvaluatesOnlyThatSide) {
  // Only an ask, V far above it: buys.
  auto s = wake_with(1000.0, BBO{0, 0, 990, 3});
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(order(s[0]).side, lob::Side::Buy);
  // Only an ask, V far below it: the sell side cannot be evaluated, so nothing.
  EXPECT_TRUE(wake_with(1000.0, BBO{0, 0, 1010, 3}).empty());
  // Only a bid, V far below it: sells.
  s = wake_with(1000.0, BBO{1010, 3, 0, 0});
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(order(s[0]).side, lob::Side::Sell);
  // Only a bid, V far above it: nothing.
  EXPECT_TRUE(wake_with(1000.0, BBO{990, 3, 0, 0}).empty());
}

TEST(InformedTrader, BuysAtExactlyThetaAndNotJustBelow) {
  // V - ask == theta exactly.
  auto s = wake_with(1000.0, BBO{990, 1, 998, 1});
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(order(s[0]).side, lob::Side::Buy);
  // V - ask == theta - 1 tick: nothing (and the bid is below V, so no sell either).
  EXPECT_TRUE(wake_with(1000.0, BBO{990, 1, 999, 1}).empty());
  // A fractional shortfall: V - ask = 1.9999 < theta.
  EXPECT_TRUE(wake_with(1000.9999, BBO{990, 1, 999, 1}).empty());
  // And V - ask = 2.0001 trades.
  EXPECT_EQ(wake_with(1001.0001, BBO{990, 1, 999, 1}).size(), 1u);
}

TEST(InformedTrader, SellsAtExactlyThetaAndNotJustBelow) {
  auto s = wake_with(1000.0, BBO{1002, 1, 1010, 1});  // bid - V == theta
  ASSERT_EQ(s.size(), 1u);
  EXPECT_EQ(order(s[0]).side, lob::Side::Sell);
  EXPECT_TRUE(wake_with(1000.0, BBO{1001, 1, 1010, 1}).empty());
  EXPECT_TRUE(wake_with(999.0001, BBO{1001, 1, 1010, 1}).empty());  // bid - V = 1.9999
  EXPECT_EQ(wake_with(998.9999, BBO{1001, 1, 1010, 1}).size(), 1u);
}

TEST(InformedTrader, ValueBetweenBidAndAskDoesNothing) {
  EXPECT_TRUE(wake_with(1000.0, BBO{999, 5, 1001, 5}).empty());
  EXPECT_TRUE(wake_with(1000.0, BBO{995, 5, 1005, 5}).empty());
}

TEST(InformedTrader, OrderIsAMarketIocOfExactlySizeLotsOnTheRightSide) {
  const Fundamental f = fixed(1000.0);
  InformedTrader t(params(), f);
  FakeCtx ctx(9);
  t.on_start(ctx);
  t.on_market_data(ctx, BBO{990, 1, 995, 1});
  ctx.fire(t);  // buy
  t.on_market_data(ctx, BBO{1005, 1, 1010, 1});
  ctx.fire(t);  // sell
  ASSERT_EQ(ctx.sent.size(), 2u);
  for (const lob::Request& r : ctx.sent) {
    const lob::NewOrder& o = order(r);
    EXPECT_EQ(o.type, lob::OrderType::Market);
    EXPECT_EQ(o.tif, lob::TIF::IOC);
    EXPECT_EQ(o.qty, kSize);
    EXPECT_FALSE(o.post_only);
    EXPECT_EQ(o.owner, 7u);
    EXPECT_EQ(o.stp, lob::STP::CancelTaking);
  }
  EXPECT_EQ(order(ctx.sent[0]).side, lob::Side::Buy);
  EXPECT_EQ(order(ctx.sent[1]).side, lob::Side::Sell);
  EXPECT_NE(order(ctx.sent[0]).client_order_id, order(ctx.sent[1]).client_order_id);
  EXPECT_EQ(t.buys_sent(), 1u);
  EXPECT_EQ(t.sells_sent(), 1u);
}

TEST(InformedTrader, DecisionFollowsTheNewestBboAndIgnoresTradePrints) {
  const Fundamental f = fixed(1000.0);
  InformedTrader t(params(), f);
  FakeCtx ctx(11);
  t.on_start(ctx);
  t.on_market_data(ctx, BBO{990, 1, 995, 1});
  t.on_market_data(ctx, BBO{999, 1, 1001, 1});  // newer: V is inside the spread
  t.on_market_data(ctx, TradePrint{900, 4, lob::Side::Sell});
  EXPECT_EQ(t.local_bbo(), (BBO{999, 1, 1001, 1}));
  ctx.fire(t);
  EXPECT_TRUE(ctx.sent.empty()) << "the older BBO would have triggered a buy";
  t.on_market_data(ctx, BBO{990, 1, 995, 1});
  ctx.fire(t);
  EXPECT_EQ(ctx.sent.size(), 1u);
}

TEST(InformedTrader, SchedulesTheNextWakeupEveryTimeAndDrawsOnlyInWakeups) {
  const Fundamental f = fixed(1000.0);
  InformedTrader t(params(), f);
  FakeCtx ctx(13);
  t.on_start(ctx);
  ASSERT_EQ(ctx.pending.size(), 1u);

  // A reference generator replays the draws the trader must make: one exponential each.
  Xoshiro256StarStar ref(13);
  EXPECT_EQ(ctx.pending[0], seconds_to_ns(ref.exponential(1.0)));

  // Messages must not advance the stream.
  const Xoshiro256StarStar before = ctx.r;
  t.on_market_data(ctx, BBO{990, 1, 995, 1});
  t.on_market_data(ctx, TradePrint{995, 1, lob::Side::Buy});
  t.on_private(ctx, lob::Cancelled{});
  EXPECT_TRUE(ctx.r == before) << "on_market_data / on_private consumed ctx.rng()";

  // Wakeups: trading (BBO with an edge) and not trading (empty BBO) both reschedule.
  for (int i = 0; i < 20; ++i) {
    const TimeNs now_before = ctx.earliest();
    ctx.fire(t);
    ASSERT_EQ(ctx.pending.size(), 1u) << "exactly one wakeup pending after each wakeup";
    EXPECT_GT(ctx.earliest(), now_before);
    EXPECT_EQ(ctx.earliest(), now_before + seconds_to_ns(ref.exponential(1.0)));
    if (i == 9) t.on_market_data(ctx, BBO{1000, 1, 1001, 1});  // no edge from now on
  }
  EXPECT_GT(t.buys_sent(), 0u);
  EXPECT_LT(t.buys_sent(), 20u) << "some wakeups did not trade";
}

TEST(InformedTrader, WakeupScheduleDoesNotDependOnTheMarket) {
  const Fundamental f = fixed(1000.0);
  auto times = [&](const BBO& bbo) {
    InformedTrader t(params(), f);
    FakeCtx ctx(21);
    t.on_start(ctx);
    t.on_market_data(ctx, bbo);
    std::vector<TimeNs> out;
    for (int i = 0; i < 30; ++i) {
      ctx.fire(t);
      out.push_back(ctx.t);
    }
    return out;
  };
  EXPECT_EQ(times(BBO{}), times(BBO{990, 1, 995, 1}));
}

TEST(InformedTraderSim, ShortRunTradesBothWaysAndIsDeterministic) {
  SimConfig cfg = simtest::default_config();
  cfg.duration_s = 300;
  cfg.warmup_s = 0;
  cfg.mm.reset();
  finalize(cfg);
  ASSERT_TRUE(cfg.informed.has_value());

  auto run_once = [&] {
    std::ostringstream log;
    Simulator s(cfg, SimulatorOptions{&log});
    s.run();
    EXPECT_EQ(s.agent_count(), cfg.noise_traders.count + 1u);
    const auto* inf = dynamic_cast<const InformedTrader*>(&s.agent(cfg.noise_traders.count));
    EXPECT_NE(inf, nullptr);
    return std::tuple(s.exchange().event_hash(), inf->buys_sent(), inf->sells_sent(), log.str());
  };
  const auto a = run_once();
  const auto b = run_once();
  EXPECT_GT(std::get<1>(a), 0u);
  EXPECT_GT(std::get<2>(a), 0u);
  EXPECT_EQ(std::get<0>(a), std::get<0>(b));
  EXPECT_EQ(std::get<3>(a), std::get<3>(b));
}

TEST(InformedTraderSim, RemovingTheInformedSectionReproducesTheNoiseOnlyLog) {
  SimConfig with = simtest::default_config();
  with.duration_s = 60;
  with.warmup_s = 0;
  with.mm.reset();
  finalize(with);
  SimConfig without = with;
  without.informed.reset();
  finalize(without);

  const simtest::RunResult noise_only = simtest::run(simtest::noise_config(10, 42, 60.0));
  const simtest::RunResult stripped = simtest::run(without);
  EXPECT_EQ(stripped.log, noise_only.log);
  EXPECT_EQ(stripped.hash, noise_only.hash);
  EXPECT_NE(simtest::run(with).hash, noise_only.hash) << "the informed trader does trade";
}
