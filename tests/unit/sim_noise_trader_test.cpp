#include <gtest/gtest.h>

#include <algorithm>
#include <variant>
#include <vector>

#include "sim/agents/noise_trader.hpp"
#include "support/fake_context.hpp"

using namespace sim;

namespace {

using simtest::FakeCtx;

NoiseTraderParams params() {
  NoiseTraderParams p;
  p.lambda = 1.0;
  p.p_limit = 1.0;
  p.order_lifetime_mean_s = 0.001;
  p.D = 1;
  p.max_size = 1;
  p.initial_price = 500;
  p.max_price = 1000;
  return p;
}

const lob::NewOrder& new_order(const lob::Request& r) { return std::get<lob::NewOrder>(r); }

lob::Price priced(const lob::NewOrder& o) { return o.price; }

}  // namespace

TEST(NoiseTrader, ReferencePriceFallsBackFromMidToLastTradeToInitial) {
  NoiseTrader nt(params(), Xoshiro256StarStar(1));
  FakeCtx ctx(1);
  EXPECT_EQ(nt.reference_price(), 500);
  nt.on_market_data(ctx, TradePrint{480, 2, lob::Side::Sell});
  EXPECT_EQ(nt.reference_price(), 480);
  nt.on_market_data(ctx, BBO{100, 5, 103, 5});
  EXPECT_EQ(nt.reference_price(), 101) << "floor((100 + 103) / 2)";
  nt.on_market_data(ctx, BBO{100, 5, 101, 5});
  EXPECT_EQ(nt.reference_price(), 100);
  nt.on_market_data(ctx, BBO{100, 5, 0, 0});  // one-sided: back to the last trade
  EXPECT_EQ(nt.reference_price(), 480);
  nt.on_market_data(ctx, TradePrint{90, 1, lob::Side::Buy});
  EXPECT_EQ(nt.reference_price(), 90);
  nt.on_market_data(ctx, BBO{0, 0, 0, 0});
  EXPECT_EQ(nt.reference_price(), 90);
}

TEST(NoiseTrader, LimitPriceIsRefMinusOrPlusDAndClamped) {
  // D = 1: buy at ref - 1, sell at ref + 1.
  {
    NoiseTrader nt(params(), Xoshiro256StarStar(1));
    FakeCtx ctx(3);
    nt.on_start(ctx);
    nt.on_market_data(ctx, BBO{200, 1, 203, 1});
    bool buy = false, sell = false;
    for (int i = 0; i < 60; ++i) ctx.fire(nt);
    for (const lob::Request& r : ctx.sent) {
      const auto& o = new_order(r);
      EXPECT_EQ(o.type, lob::OrderType::Limit);
      EXPECT_EQ(o.tif, lob::TIF::GTC);
      EXPECT_FALSE(o.post_only);
      EXPECT_EQ(o.owner, 7u);
      if (o.side == lob::Side::Buy) {
        EXPECT_EQ(priced(o), 200);  // ref = 201
        buy = true;
      } else {
        EXPECT_EQ(priced(o), 202);
        sell = true;
      }
    }
    EXPECT_TRUE(buy && sell);
  }
  // Near the bottom: ref = 2, D = 10 -> buys clamp to 1; sells stay in [3, 12].
  {
    NoiseTraderParams p = params();
    p.D = 10;
    p.initial_price = 2;
    NoiseTrader nt(p, Xoshiro256StarStar(1));
    FakeCtx ctx(4);
    nt.on_start(ctx);
    int at_floor = 0;
    for (int i = 0; i < 300; ++i) ctx.fire(nt);
    for (const lob::Request& r : ctx.sent) {
      const auto& o = new_order(r);
      ASSERT_GE(o.price, 1);
      if (o.side == lob::Side::Buy) at_floor += o.price == 1;
      else {
        EXPECT_GE(o.price, 3);
        EXPECT_LE(o.price, 12);
      }
    }
    EXPECT_GT(at_floor, 0);
  }
  // Near the top: ref = max_price = 1000, D = 10 -> sells clamp to 1000; buys in [990, 999].
  {
    NoiseTraderParams p = params();
    p.D = 10;
    p.initial_price = 1000;
    NoiseTrader nt(p, Xoshiro256StarStar(1));
    FakeCtx ctx(5);
    nt.on_start(ctx);
    int at_cap = 0;
    for (int i = 0; i < 300; ++i) ctx.fire(nt);
    for (const lob::Request& r : ctx.sent) {
      const auto& o = new_order(r);
      ASSERT_LE(o.price, 1000);
      if (o.side == lob::Side::Sell) at_cap += o.price == 1000;
      else {
        EXPECT_GE(o.price, 990);
        EXPECT_LE(o.price, 999);
      }
    }
    EXPECT_GT(at_cap, 0);
  }
}

TEST(NoiseTrader, DrawOrderOnAWakeupMatchesTheDesign) {
  NoiseTraderParams p = params();
  p.p_limit = 0.5;
  p.D = 10;
  p.max_size = 10;
  p.lambda = 5.0;
  NoiseTrader nt(p, Xoshiro256StarStar(1));
  FakeCtx ctx(77);
  Xoshiro256StarStar mirror = ctx.r;  // the same stream, drawn by hand

  nt.on_start(ctx);
  ASSERT_EQ(ctx.pending.size(), 1u);
  EXPECT_EQ(ctx.pending[0], seconds_to_ns(mirror.exponential(p.lambda)));
  EXPECT_EQ(ctx.r, mirror);

  for (int i = 0; i < 200; ++i) {
    const TimeNs fired = ctx.earliest();
    const bool is_limit = mirror.uniform01() < p.p_limit;
    const bool buy = mirror.uniform_int(0, 1) == 0;
    const lob::Price d = is_limit ? mirror.uniform_int(1, p.D) : 0;
    const lob::Qty size = mirror.uniform_int(1, p.max_size);
    const TimeNs next = fired + seconds_to_ns(mirror.exponential(p.lambda));

    const std::size_t before = ctx.sent.size();
    ctx.fire(nt);
    ASSERT_EQ(ctx.sent.size(), before + 1);
    const auto& o = new_order(ctx.sent.back());
    EXPECT_EQ(o.type, is_limit ? lob::OrderType::Limit : lob::OrderType::Market);
    EXPECT_EQ(o.side, buy ? lob::Side::Buy : lob::Side::Sell);
    EXPECT_EQ(o.qty, size);
    EXPECT_EQ(o.client_order_id, static_cast<std::uint64_t>(i + 1));
    if (is_limit) EXPECT_EQ(o.price, buy ? 500 - d : 500 + d);
    else EXPECT_EQ(o.tif, lob::TIF::IOC);
    ASSERT_EQ(ctx.pending.size(), 1u);
    EXPECT_EQ(ctx.pending[0], next);
    EXPECT_EQ(ctx.r, mirror) << "draw " << i;
  }
}

TEST(NoiseTrader, AckOfALimitOrderSchedulesOneCancelFromTheLifetimeStream) {
  const NoiseTraderParams p = params();
  const Xoshiro256StarStar lifetime_seed(1234);
  NoiseTrader nt(p, lifetime_seed);
  Xoshiro256StarStar lifetime_mirror = lifetime_seed;
  FakeCtx ctx(9);
  nt.on_start(ctx);
  ctx.fire(nt);  // sends client order 1, schedules the next trading wakeup
  ASSERT_EQ(ctx.sent.size(), 1u);
  ASSERT_EQ(ctx.pending.size(), 1u);
  const Xoshiro256StarStar decisions_before = ctx.r;

  nt.on_private(ctx, lob::Ack{55, 7, 1});
  const TimeNs expected = ctx.t + seconds_to_ns(lifetime_mirror.exponential(1.0 / p.order_lifetime_mean_s));
  ASSERT_EQ(ctx.pending.size(), 2u) << "exactly one new timer";
  EXPECT_EQ(ctx.pending[1], expected);
  EXPECT_EQ(ctx.r, decisions_before) << "ctx.rng() untouched by on_private";
  EXPECT_EQ(nt.open_orders(), 1u);

  // A repeated or unknown Ack schedules nothing.
  nt.on_private(ctx, lob::Ack{56, 7, 99});
  EXPECT_EQ(ctx.pending.size(), 2u);

  // The cancel fires (about 1 ms, long before the next trading wakeup at ~1 s).
  ASSERT_LT(expected, ctx.pending[0]);
  ctx.fire(nt);
  ASSERT_EQ(ctx.sent.size(), 2u);
  EXPECT_EQ(std::get<lob::Cancel>(ctx.sent[1]), (lob::Cancel{7, 55}));
}

TEST(NoiseTrader, MarketOrderAckSchedulesNoTimer) {
  NoiseTraderParams p = params();
  p.p_limit = 0.0;
  NoiseTrader nt(p, Xoshiro256StarStar(1));
  FakeCtx ctx(9);
  nt.on_start(ctx);
  ctx.fire(nt);
  ASSERT_EQ(new_order(ctx.sent[0]).type, lob::OrderType::Market);
  const std::size_t timers = ctx.pending.size();
  nt.on_private(ctx, lob::Ack{1, 7, 1});
  EXPECT_EQ(ctx.pending.size(), timers);
  EXPECT_EQ(nt.open_orders(), 0u);
}

TEST(NoiseTrader, MessageHandlersNeverTouchTheDecisionStream) {
  NoiseTrader nt(params(), Xoshiro256StarStar(5));
  FakeCtx ctx(11);
  nt.on_start(ctx);
  ctx.fire(nt);
  const Xoshiro256StarStar before = ctx.r;
  nt.on_market_data(ctx, BBO{10, 1, 12, 1});
  nt.on_market_data(ctx, TradePrint{11, 1, lob::Side::Buy});
  nt.on_private(ctx, lob::Ack{1, 7, 1});
  nt.on_private(ctx, lob::Trade{1, 10, 1, 1, 2, 7, 8, lob::Side::Sell});
  nt.on_private(ctx, lob::Cancelled{1, 7, 1, lob::CancelReason::User});
  nt.on_private(ctx, lob::Reject{7, lob::RequestKind::Cancel, 0, 1, lob::RejectReason::UnknownOrder});
  EXPECT_EQ(ctx.r, before);
}

TEST(NoiseTrader, CancelIsSentOnlyIfTheOrderIsStillOpenByItsOwnRecords) {
  NoiseTraderParams p = params();
  p.max_size = 3;
  for (int scenario = 0; scenario < 3; ++scenario) {
    NoiseTrader nt(p, Xoshiro256StarStar(1));
    FakeCtx ctx(21);
    nt.on_start(ctx);
    ctx.fire(nt);
    const lob::Qty size = new_order(ctx.sent[0]).qty;
    nt.on_private(ctx, lob::Ack{55, 7, 1});
    ASSERT_EQ(nt.open_orders(), 1u);
    if (scenario == 0) {  // filled as the resting maker
      nt.on_private(ctx, lob::Trade{1, 10, size, 55, 90, 7, 8, lob::Side::Buy});
      EXPECT_EQ(nt.open_orders(), 0u);
    } else if (scenario == 1) {  // cancelled by the exchange (self-trade prevention)
      nt.on_private(ctx, lob::Cancelled{55, 7, size, lob::CancelReason::SelfTrade});
      EXPECT_EQ(nt.open_orders(), 0u);
    } else if (size > 1) {  // a partial fill leaves it open (size 1: untouched)
      nt.on_private(ctx, lob::Trade{1, 10, 1, 90, 55, 8, 7, lob::Side::Sell});
      EXPECT_EQ(nt.open_orders(), 1u);
    }
    // Fire the cancel timer (the earliest one).
    const std::size_t sent_before = ctx.sent.size();
    ctx.fire(nt);
    // Filled or cancelled: no cancel goes out. Untouched or partly filled: one does.
    EXPECT_EQ(ctx.sent.size(), sent_before + (scenario == 2 ? 1u : 0u)) << "scenario " << scenario;
  }
}

TEST(NoiseTrader, UnknownOrderRejectIsIgnored) {
  NoiseTrader nt(params(), Xoshiro256StarStar(1));
  FakeCtx ctx(31);
  nt.on_start(ctx);
  ctx.fire(nt);
  nt.on_private(ctx, lob::Ack{55, 7, 1});
  ctx.fire(nt);  // the cancel goes out
  ASSERT_EQ(ctx.sent.size(), 2u);
  const std::size_t sent = ctx.sent.size();
  const std::size_t timers = ctx.pending.size();
  EXPECT_NO_THROW(
      nt.on_private(ctx, lob::Reject{7, lob::RequestKind::Cancel, 0, 55, lob::RejectReason::UnknownOrder}));
  EXPECT_EQ(ctx.sent.size(), sent);
  EXPECT_EQ(ctx.pending.size(), timers);
  EXPECT_EQ(nt.open_orders(), 0u);
  EXPECT_EQ(nt.ignored_rejects(), 1u);
}

TEST(NoiseTrader, WakeupWithoutATimerThrows) {
  NoiseTrader nt(params(), Xoshiro256StarStar(1));
  FakeCtx ctx(1);
  EXPECT_THROW(nt.on_wakeup(ctx), std::logic_error);
}

namespace {

// Runs `n` wakeups of a limit-only trader with the given mode and BBO (if any, and a last
// trade if any); returns the orders and the final generator state.
struct Sent {
  std::vector<lob::NewOrder> orders;
  Xoshiro256StarStar rng;
};

Sent run_mode(ReferencePrice mode, const BBO* bbo, const TradePrint* trade, int n = 80) {
  NoiseTraderParams p = params();
  p.D = 5;
  p.max_price = 100'000;
  p.reference_price = mode;
  NoiseTrader nt(p, Xoshiro256StarStar(1));
  FakeCtx ctx(31);
  nt.on_start(ctx);
  if (trade) nt.on_market_data(ctx, *trade);
  if (bbo) nt.on_market_data(ctx, *bbo);
  for (int i = 0; i < n; ++i) ctx.fire(nt);
  Sent out{{}, ctx.r};
  for (const lob::Request& r : ctx.sent) out.orders.push_back(new_order(r));
  return out;
}

}  // namespace

TEST(NoiseTraderReference, EvenSpreadGivesTheSamePricesInBothModes) {
  const BBO bbo{9880, 3, 9882, 3};  // integer mid 9881
  const Sent f = run_mode(ReferencePrice::Floor, &bbo, nullptr);
  const Sent s = run_mode(ReferencePrice::Symmetric, &bbo, nullptr);
  ASSERT_EQ(f.orders.size(), s.orders.size());
  for (std::size_t i = 0; i < f.orders.size(); ++i) {
    EXPECT_EQ(f.orders[i].side, s.orders[i].side);
    EXPECT_EQ(f.orders[i].price, s.orders[i].price) << i;
  }
}

TEST(NoiseTraderReference, OddSpreadFloorUsesFloorMidSymmetricUsesCeilForBuys) {
  const BBO bbo{9879, 3, 9882, 3};  // mid 9880.5
  const Sent f = run_mode(ReferencePrice::Floor, &bbo, nullptr);
  const Sent s = run_mode(ReferencePrice::Symmetric, &bbo, nullptr);
  ASSERT_EQ(f.orders.size(), s.orders.size());
  bool buy = false, sell = false;
  for (std::size_t i = 0; i < f.orders.size(); ++i) {
    ASSERT_EQ(f.orders[i].side, s.orders[i].side);
    if (f.orders[i].side == lob::Side::Buy) {
      const lob::Price d = 9880 - f.orders[i].price;  // Floor: 9880 - d
      EXPECT_GE(d, 1);
      EXPECT_LE(d, 5);
      EXPECT_EQ(s.orders[i].price, 9881 - d);
      buy = true;
    } else {
      const lob::Price d = f.orders[i].price - 9880;  // Floor: 9880 + d
      EXPECT_GE(d, 1);
      EXPECT_LE(d, 5);
      EXPECT_EQ(s.orders[i].price, 9880 + d);
      sell = true;
    }
  }
  EXPECT_TRUE(buy && sell);
}

TEST(NoiseTraderReference, SymmetricWithSpreadOneAndDOneJoinsTheBestPricesAndNeverCrosses) {
  const BBO bbo{100, 4, 101, 4};
  NoiseTraderParams p = params();  // D = 1
  p.reference_price = ReferencePrice::Symmetric;
  NoiseTrader nt(p, Xoshiro256StarStar(1));
  FakeCtx ctx(17);
  nt.on_start(ctx);
  nt.on_market_data(ctx, bbo);
  for (int i = 0; i < 60; ++i) ctx.fire(nt);
  bool buy = false, sell = false;
  for (const lob::Request& r : ctx.sent) {
    const lob::NewOrder& o = new_order(r);
    if (o.side == lob::Side::Buy) {
      EXPECT_EQ(o.price, 100);
      buy = true;
    } else {
      EXPECT_EQ(o.price, 101);
      sell = true;
    }
  }
  EXPECT_TRUE(buy && sell);
}

TEST(NoiseTraderReference, FallbacksAreTheSameInBothModes) {
  const TradePrint trade{480, 2, lob::Side::Sell};
  const BBO one_sided{100, 5, 0, 0};
  for (const BBO* bbo : {static_cast<const BBO*>(nullptr), &one_sided}) {
    for (const TradePrint* t : {static_cast<const TradePrint*>(nullptr), &trade}) {
      const Sent f = run_mode(ReferencePrice::Floor, bbo, t);
      const Sent s = run_mode(ReferencePrice::Symmetric, bbo, t);
      ASSERT_EQ(f.orders.size(), s.orders.size());
      for (std::size_t i = 0; i < f.orders.size(); ++i) EXPECT_EQ(f.orders[i].price, s.orders[i].price);
      const lob::Price ref = t ? 480 : 500;  // last trade, else initial price
      for (const lob::NewOrder& o : s.orders) {
        if (o.side == lob::Side::Buy) EXPECT_LT(o.price, ref);
        else EXPECT_GT(o.price, ref);
      }
    }
  }
}

TEST(NoiseTraderReference, DrawSequenceIsIdenticalInBothModes) {
  const BBO bbo{9879, 3, 9882, 3};
  const Sent f = run_mode(ReferencePrice::Floor, &bbo, nullptr);
  const Sent s = run_mode(ReferencePrice::Symmetric, &bbo, nullptr);
  EXPECT_EQ(f.rng, s.rng);
  ASSERT_EQ(f.orders.size(), s.orders.size());
  for (std::size_t i = 0; i < f.orders.size(); ++i) {
    EXPECT_EQ(f.orders[i].side, s.orders[i].side);
    EXPECT_EQ(f.orders[i].qty, s.orders[i].qty);
  }
}
