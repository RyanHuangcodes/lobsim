#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

#include "sim/agents/as_market_maker.hpp"
#include "support/fake_context.hpp"

using namespace sim;
using simtest::FakeCtx;
using lob::RejectReason;
using lob::RequestKind;
using lob::Side;
using Kind = AvellanedaStoikovMM::PendingKind;
using Working = AvellanedaStoikovMM::WorkingOrder;

namespace {

constexpr TimeNs kSec = 1'000'000'000;
constexpr TimeNs kHour = 3600 * kSec;

// The SPEC 5.8 parameters.
MmParams spec_params() {
  MmParams p;
  p.gamma = 0.0001;
  p.kappa = 1.5;
  p.sigma = 1.0;
  p.quote_size = 5;
  p.q_max = 50;
  p.max_price = 1'000'000;
  return p;
}

BBO book(lob::Price bid, lob::Price ask) { return BBO{bid, 10, ask, 10}; }

}  // namespace

// ---------------------------------------------------------------------------------------
// compute_quotes, with hand-computed numbers. Constants used below, SPEC 5.8 parameters:
//   (2 / gamma) ln(1 + gamma / kappa) = 20000 ln(1.0000666...) = 1.3332889 ticks
//   gamma sigma^2 T = 0.0001 * 3600 = 0.36 ticks (per lot of inventory, for r)
// ---------------------------------------------------------------------------------------

TEST(MmQuotes, SpecSanityCheckNumbers) {
  const MmParams p = spec_params();
  // SPEC 5.8: at t = 0 the total spread is 0.36 + 1.33 = about 1.69 ticks...
  EXPECT_NEAR(total_spread(p, 3600.0), 1.6932889, 1e-6);
  // ...and narrows to about 1.33 ticks at the end.
  EXPECT_NEAR(total_spread(p, 0.0), 1.3332889, 1e-6);
  // One lot of inventory shifts r by 0.36 ticks at t = 0; one 5-lot fill by 1.8 ticks.
  EXPECT_NEAR(reservation_price(p, 10000.0, 0, 3600.0), 10000.0, 1e-12);
  EXPECT_NEAR(reservation_price(p, 10000.0, 1, 3600.0), 9999.64, 1e-9);
  EXPECT_NEAR(reservation_price(p, 10000.0, 5, 3600.0), 9998.2, 1e-9);
  EXPECT_NEAR(reservation_price(p, 10000.0, -1, 3600.0), 10000.36, 1e-9);
  // Half-way through the session the shift per lot is halved.
  EXPECT_NEAR(reservation_price(p, 10000.0, 1, 1800.0), 9999.82, 1e-9);
}

TEST(MmQuotes, FlatInventoryAtTheStart) {
  const MmParams p = spec_params();
  // s = 10000, delta/2 = 0.8466: bid = floor(9999.153) = 9999, ask = ceil(10000.847) = 10001.
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 0, 0, kHour), (Quotes{9999, 10001}));
  // s = 10000.5: bid = floor(9999.653) = 9999, ask = ceil(10001.347) = 10002.
  EXPECT_EQ(compute_quotes(p, book(9990, 10011), 0, 0, kHour), (Quotes{9999, 10002}));
}

TEST(MmQuotes, OneLotShiftsTheBidAcrossATick) {
  const MmParams p = spec_params();
  // q = 1: r = 9999.64; bid = floor(9998.793) = 9998, ask = ceil(10000.487) = 10001.
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 1, 0, kHour), (Quotes{9998, 10001}));
  // q = 3: r = 9998.92; bid = floor(9998.073) = 9998, ask = ceil(9999.767) = 10000.
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 3, 0, kHour), (Quotes{9998, 10000}));
}

TEST(MmQuotes, LongAndShortInventory) {
  const MmParams p = spec_params();
  // Long 10: r = 10000 - 3.6 = 9996.4; bid = floor(9995.553) = 9995, ask = ceil(9997.247) = 9998.
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 10, 0, kHour), (Quotes{9995, 9998}));
  // Short 10: r = 10003.6; bid = floor(10002.753) = 10002, ask = ceil(10004.447) = 10005.
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), -10, 0, kHour), (Quotes{10002, 10005}));
}

TEST(MmQuotes, ClampsToTheLocalBbo) {
  const MmParams p = spec_params();
  // Short 10 with BBO 9999 / 10001: the desired bid 10002 is clamped to best ask - 1.
  EXPECT_EQ(compute_quotes(p, book(9999, 10001), -10, 0, kHour), (Quotes{10000, 10005}));
  // Long 10: the desired ask 9998 is clamped to best bid + 1.
  EXPECT_EQ(compute_quotes(p, book(9999, 10001), 10, 0, kHour), (Quotes{9995, 10000}));
}

TEST(MmQuotes, BidNeverReachesItsOwnRestingAsk) {
  const MmParams p = spec_params();
  // The local BBO is the market maker's own quotes, 9999 / 10000 (s = 9999.5). Short 10:
  // r = 10003.1, the desired bid floor(10002.253) = 10002 is at or above its own ask, so
  // it is clamped to 9999 = own ask - 1; the ask is ceil(10003.947) = 10004.
  EXPECT_EQ(compute_quotes(p, book(9999, 10000), -10, 0, kHour), (Quotes{9999, 10004}));
  // Mirror: long 10 (r = 9995.9): the desired ask ceil(9996.747) = 9997 is clamped up to
  // 10000 = own bid + 1; the bid is floor(9995.053) = 9995.
  EXPECT_EQ(compute_quotes(p, book(9999, 10000), 10, 0, kHour), (Quotes{9995, 10000}));
}

TEST(MmQuotes, BidStaysBelowAskEvenWhenRoundingMeetsInTheMiddle) {
  // sigma = 0 and kappa huge: delta = 2 ln(1 + 1e-13) = 2e-13, far below the spacing of
  // doubles near 10000 (1.8e-12), so floor(r - delta/2) = ceil(r + delta/2) = 10000.
  MmParams p = spec_params();
  p.gamma = 1.0;
  p.kappa = 1e13;
  p.sigma = 0.0;
  EXPECT_EQ(compute_quotes(p, book(9998, 10002), 0, 0, kHour), (Quotes{9999, 10000}));
}

TEST(MmQuotes, EndOfSession) {
  const MmParams p = spec_params();
  // tau = 0: no inventory term, delta = 1.3333: bid = floor(9999.333), ask = ceil(10000.667).
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 0, kHour, kHour), (Quotes{9999, 10001}));
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 40, kHour, kHour), (Quotes{9999, 10001}));
  // tau = 1 ms: the shift for 40 lots is 4e-6 ticks, still the same quotes.
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 40, kHour - 1'000'000, kHour), (Quotes{9999, 10001}));
  // Past the end tau is taken as 0, never negative.
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 40, kHour + kSec, kHour), (Quotes{9999, 10001}));
}

TEST(MmQuotes, InventoryLimitsAtExactlyQmax) {
  const MmParams p = spec_params();  // q_max = 50; at tau = 0 the prices are 9999 / 10001
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 49, kHour, kHour), (Quotes{9999, 10001}));
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 50, kHour, kHour), (Quotes{std::nullopt, 10001}));
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), 51, kHour, kHour), (Quotes{std::nullopt, 10001}));
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), -49, kHour, kHour), (Quotes{9999, 10001}));
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), -50, kHour, kHour), (Quotes{9999, std::nullopt}));
  EXPECT_EQ(compute_quotes(p, book(9990, 10010), -51, kHour, kHour), (Quotes{9999, std::nullopt}));
}

TEST(MmQuotes, NoQuotesWithoutATwoSidedBbo) {
  const MmParams p = spec_params();
  EXPECT_EQ(compute_quotes(p, BBO{}, 0, 0, kHour), Quotes{});
  EXPECT_EQ(compute_quotes(p, BBO{9990, 10, 0, 0}, 0, 0, kHour), Quotes{});
  EXPECT_EQ(compute_quotes(p, BBO{0, 0, 10010, 10}, 0, 0, kHour), Quotes{});
}

TEST(MmQuotes, ASideOutsideThePriceRangeIsNotQuoted) {
  MmParams p = spec_params();
  // s = 1.5 at tau = 0: bid = floor(0.833) = 0 is below 1; ask = ceil(2.167) = 3.
  EXPECT_EQ(compute_quotes(p, book(1, 2), 0, kHour, kHour), (Quotes{std::nullopt, 3}));
  // max_price 10, s = 9.5: ask = ceil(10.167) = 11 is above it; bid = floor(8.833) = 8.
  p.max_price = 10;
  EXPECT_EQ(compute_quotes(p, book(9, 10), 0, kHour, kHour), (Quotes{8, std::nullopt}));
}

// ---------------------------------------------------------------------------------------
// The agent, driven by a scripted context. The test plays the exchange.
// ---------------------------------------------------------------------------------------

namespace {

constexpr lob::OwnerId kSelf = 7;  // FakeCtx::self()
constexpr lob::OwnerId kOther = 99;
constexpr TimeNs kInterval = 100'000'000;  // 100 ms
constexpr TimeNs kEnd = 10 * kSec;

// Small parameters for the state-machine tests. With tau <= 10 s the inventory term is at
// most 0.001 tick per lot, so for a BBO whose mid m is an integer the quotes are always
// m - 1 and m + 1, whatever the inventory: prices move only when the test moves the BBO.
// q_max = 12 and quote_size = 5: the bid may be repriced by Modify while q + 5 <= 12.
MmParams small_params() {
  MmParams p = spec_params();
  p.q_max = 12;
  return p;
}

lob::Event ack(lob::OrderId id, std::uint64_t cid) { return lob::Ack{id, kSelf, cid}; }
lob::Event reject_new(std::uint64_t cid, RejectReason r) { return lob::Reject{kSelf, RequestKind::New, cid, 0, r}; }
lob::Event reject(RequestKind k, lob::OrderId id, RejectReason r) { return lob::Reject{kSelf, k, 0, id, r}; }
lob::Event modified(lob::OrderId id, lob::Price p, lob::Qty q) { return lob::Modified{id, kSelf, p, q, false}; }
lob::Event cancelled(lob::OrderId id, lob::Qty q) {
  return lob::Cancelled{id, kSelf, q, lob::CancelReason::User};
}
// Another owner trades `qty` lots against the market maker's resting order `id` on `side`.
lob::Event fill(Side side, lob::OrderId id, lob::Price price, lob::Qty qty) {
  return lob::Trade{1, price, qty, id, 5000, kSelf, kOther, lob::opposite(side)};
}

void expect_new(const lob::Request& r, Side side, lob::Price price) {
  const auto* o = std::get_if<lob::NewOrder>(&r);
  ASSERT_NE(o, nullptr) << "expected a NewOrder";
  EXPECT_EQ(o->owner, kSelf);
  EXPECT_EQ(o->side, side);
  EXPECT_EQ(o->price, price);
  EXPECT_EQ(o->qty, 5);
  EXPECT_EQ(o->type, lob::OrderType::Limit);
  EXPECT_EQ(o->tif, lob::TIF::GTC);
  EXPECT_TRUE(o->post_only);
}
void expect_modify(const lob::Request& r, lob::OrderId id, lob::Price price) {
  EXPECT_EQ(r, (lob::Request{lob::Modify{kSelf, id, price, 5}}));
}
void expect_cancel(const lob::Request& r, lob::OrderId id) { EXPECT_EQ(r, (lob::Request{lob::Cancel{kSelf, id}})); }
std::uint64_t cid(const lob::Request& r) { return std::get<lob::NewOrder>(r).client_order_id; }

struct Harness {
  explicit Harness(const MmParams& p = small_params(), TimeNs end = kEnd) : mm(p, kInterval, end) { mm.on_start(ctx); }

  AvellanedaStoikovMM mm;
  FakeCtx ctx{11};
  std::size_t seen{0};

  // The requests sent since the previous call.
  std::vector<lob::Request> sent() {
    std::vector<lob::Request> out(ctx.sent.begin() + static_cast<std::ptrdiff_t>(seen), ctx.sent.end());
    seen = ctx.sent.size();
    return out;
  }
  void bbo(lob::Price b, lob::Price a) { mm.on_market_data(ctx, book(b, a)); }
  void deliver(const lob::Event& e) { mm.on_private(ctx, e); }
  void wake(int n) {
    for (int i = 0; i < n; ++i) ctx.fire(mm);
  }
  // Quotes at BBO {b, a} (integer mid m: bid m - 1, ask m + 1) and acks both orders.
  void establish(lob::Price b, lob::Price a, lob::OrderId bid_id = 101, lob::OrderId ask_id = 102) {
    bbo(b, a);
    const auto s = sent();
    ASSERT_EQ(s.size(), 2u);
    const lob::Price m = (b + a) / 2;
    expect_new(s[0], Side::Buy, m - 1);
    expect_new(s[1], Side::Sell, m + 1);
    deliver(ack(bid_id, cid(s[0])));
    deliver(ack(ask_id, cid(s[1])));
    EXPECT_TRUE(sent().empty());
  }
  std::optional<Working> bid() const { return mm.working(Side::Buy); }
  std::optional<Working> ask() const { return mm.working(Side::Sell); }
  void expect_quiet_and_clean() {
    EXPECT_EQ(mm.pending_kind(Side::Buy), Kind::None);
    EXPECT_EQ(mm.pending_kind(Side::Sell), Kind::None);
    EXPECT_EQ(mm.anomalies(), 0u);
  }
};

}  // namespace

TEST(MarketMaker, QuotesOnlyAfterATwoSidedBbo) {
  Harness h;
  EXPECT_EQ(h.ctx.pending, std::vector<TimeNs>{kInterval}) << "first requote wakeup at requote_interval";
  h.wake(3);
  EXPECT_TRUE(h.sent().empty());
  EXPECT_EQ(h.ctx.pending, std::vector<TimeNs>{4 * kInterval}) << "each wakeup schedules the next";
  h.mm.on_market_data(h.ctx, BBO{});
  h.mm.on_market_data(h.ctx, BBO{990, 10, 0, 0});
  h.mm.on_market_data(h.ctx, BBO{0, 0, 1010, 10});
  h.mm.on_market_data(h.ctx, TradePrint{1000, 1, Side::Buy});
  h.wake(1);
  EXPECT_TRUE(h.sent().empty());

  h.bbo(990, 1010);
  const auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_new(s[0], Side::Buy, 999);
  expect_new(s[1], Side::Sell, 1001);
  EXPECT_NE(cid(s[0]), cid(s[1]));
  EXPECT_EQ(h.mm.pending_kind(Side::Buy), Kind::New);
  EXPECT_EQ(h.mm.pending_kind(Side::Sell), Kind::New);
  EXPECT_EQ(h.mm.news_sent(), 2u);
}

TEST(MarketMaker, PendingNewBlocksItsSideOnly) {
  Harness h;
  h.bbo(990, 1010);
  const auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  for (lob::Price b = 980; b < 990; ++b) h.bbo(b, b + 20);  // the mid moves 10 times
  h.wake(5);
  EXPECT_TRUE(h.sent().empty()) << "no second request while the New is unanswered";

  // The bid's Ack arrives: the bid is now working at 999 but should be at m - 1 = 998
  // (last BBO 989 / 1009, m = 999): repriced at once. The ask is still pending.
  h.deliver(ack(101, cid(s[0])));
  auto s2 = h.sent();
  ASSERT_EQ(s2.size(), 1u);
  expect_modify(s2[0], 101, 998);
  // Now the ask's Ack: repriced to the latest desired ask, m + 1 = 1000.
  h.deliver(ack(102, cid(s[1])));
  s2 = h.sent();
  ASSERT_EQ(s2.size(), 1u);
  expect_modify(s2[0], 102, 1000);
  EXPECT_EQ(h.mm.anomalies(), 0u);
}

TEST(MarketMaker, PendingModifyBlocksUntilModified) {
  Harness h;
  h.establish(990, 1010);
  h.bbo(992, 1012);  // m = 1002
  auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_modify(s[0], 101, 1001);
  expect_modify(s[1], 102, 1003);
  h.bbo(994, 1014);
  h.wake(4);
  h.bbo(996, 1016);  // m = 1006
  EXPECT_TRUE(h.sent().empty());

  h.deliver(modified(101, 1001, 5));
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_modify(s[0], 101, 1005);  // acts on the latest BBO
  EXPECT_EQ(h.bid(), (Working{101, 1001, 5}));
  h.deliver(modified(102, 1003, 5));
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_modify(s[0], 102, 1007);
  h.deliver(modified(101, 1005, 5));
  h.deliver(modified(102, 1007, 5));
  EXPECT_TRUE(h.sent().empty());
  EXPECT_EQ(h.bid(), (Working{101, 1005, 5}));
  EXPECT_EQ(h.ask(), (Working{102, 1007, 5}));
  EXPECT_EQ(h.mm.modifies_sent(), 4u);
  h.expect_quiet_and_clean();
}

TEST(MarketMaker, PendingCancelBlocksUntilCancelled) {
  Harness h;
  h.establish(990, 1010);
  h.mm.on_market_data(h.ctx, BBO{990, 10, 0, 0});  // one-sided: no quotes, cancel both
  auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_cancel(s[0], 101);
  expect_cancel(s[1], 102);
  h.bbo(980, 1000);
  h.wake(3);
  h.bbo(970, 990);  // m = 980
  EXPECT_TRUE(h.sent().empty());

  h.deliver(cancelled(101, 5));
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Buy, 979);
  EXPECT_FALSE(h.bid().has_value());
  h.deliver(cancelled(102, 5));
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Sell, 981);
  EXPECT_EQ(h.mm.cancels_sent(), 2u);
  EXPECT_EQ(h.mm.anomalies(), 0u);
}

TEST(MarketMaker, RepricesByModifyAwayFromTheLimit) {
  Harness h;
  h.establish(990, 1010);
  h.deliver(fill(Side::Buy, 101, 999, 2));  // q = 2, bid remaining 3, same price: nothing
  EXPECT_TRUE(h.sent().empty());
  EXPECT_EQ(h.bid(), (Working{101, 999, 3}));
  h.bbo(990, 1012);  // m = 1001
  const auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_modify(s[0], 101, 1000);
  expect_modify(s[1], 102, 1002);
}

namespace {

// Brings the bid side to inventory `q` (> 5, < 10) with a working bid of 10 - q lots at
// 999: fill the first bid (5), which is replaced by order 103 at 999, then fill q - 5 of it.
void buy_up_to(Harness& h, lob::Qty q) {
  h.establish(990, 1010);
  h.deliver(fill(Side::Buy, 101, 999, 5));
  auto s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Buy, 999);
  h.deliver(ack(103, cid(s[0])));
  h.deliver(fill(Side::Buy, 103, 999, q - 5));
  EXPECT_TRUE(h.sent().empty());
  ASSERT_EQ(h.mm.inventory(), q);
  ASSERT_EQ(h.bid(), (Working{103, 999, 10 - q}));
}
void sell_down_to(Harness& h, lob::Qty q) {  // mirror: q < 0
  h.establish(990, 1010);
  h.deliver(fill(Side::Sell, 102, 1001, 5));
  auto s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Sell, 1001);
  h.deliver(ack(103, cid(s[0])));
  h.deliver(fill(Side::Sell, 103, 1001, -q - 5));
  EXPECT_TRUE(h.sent().empty());
  ASSERT_EQ(h.mm.inventory(), q);
  ASSERT_EQ(h.ask(), (Working{103, 1001, 10 + q}));
}

}  // namespace

TEST(MarketMaker, BidAtTheExactBoundaryStillUsesModify) {
  Harness h;
  buy_up_to(h, 7);  // q + quote_size = 12 = q_max
  h.bbo(992, 1012);
  const auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_modify(s[0], 103, 1001);
  expect_modify(s[1], 102, 1003);
}

TEST(MarketMaker, BidOneLotPastTheBoundaryIsCancelThenNew) {
  Harness h;
  buy_up_to(h, 8);  // q + quote_size = 13 > q_max
  h.bbo(992, 1012);
  auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_cancel(s[0], 103);
  expect_modify(s[1], 102, 1003);  // the ask is far from its limit
  h.deliver(cancelled(103, 2));
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Buy, 1001);  // the New follows from the ordinary requote
  h.deliver(ack(104, cid(s[0])));
  h.deliver(modified(102, 1003, 5));
  EXPECT_EQ(h.bid(), (Working{104, 1001, 5}));
  EXPECT_EQ(h.ask(), (Working{102, 1003, 5}));
  EXPECT_EQ(h.mm.inventory(), 8);
  h.expect_quiet_and_clean();
}

TEST(MarketMaker, AskAtTheExactBoundaryStillUsesModify) {
  Harness h;
  sell_down_to(h, -7);
  h.bbo(988, 1008);  // m = 998
  const auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_modify(s[0], 101, 997);
  expect_modify(s[1], 103, 999);
}

TEST(MarketMaker, AskOneLotPastTheBoundaryIsCancelThenNew) {
  Harness h;
  sell_down_to(h, -8);
  h.bbo(988, 1008);
  auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_modify(s[0], 101, 997);
  expect_cancel(s[1], 103);
  h.deliver(cancelled(103, 2));
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Sell, 999);
  h.deliver(ack(104, cid(s[0])));
  h.deliver(modified(101, 997, 5));
  EXPECT_EQ(h.bid(), (Working{101, 997, 5}));
  EXPECT_EQ(h.ask(), (Working{104, 999, 5}));
  EXPECT_EQ(h.mm.inventory(), -8);
  h.expect_quiet_and_clean();
}

TEST(MarketMaker, BidIsCancelledAtQmaxAndReturnsWhenInventoryDrops) {
  Harness h;
  buy_up_to(h, 9);  // bid 103 has 1 lot left
  h.deliver(fill(Side::Buy, 103, 999, 1));  // q = 10, order gone: a New at q = 10 < 12
  auto s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Buy, 999);
  h.deliver(ack(104, cid(s[0])));
  h.deliver(fill(Side::Buy, 104, 999, 2));  // q = 12 = q_max: the bid must go
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_cancel(s[0], 104);
  h.deliver(cancelled(104, 3));
  h.bbo(992, 1012);  // the ask reprices; no bid at q_max
  h.wake(3);
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_modify(s[0], 102, 1003);
  h.deliver(modified(102, 1003, 5));
  EXPECT_FALSE(h.bid().has_value());
  EXPECT_TRUE(h.sent().empty());

  h.deliver(fill(Side::Sell, 102, 1003, 1));  // q = 11: the bid comes back
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Buy, 1001);
  EXPECT_EQ(h.mm.inventory(), 11);
}

TEST(MarketMaker, AskIsCancelledAtMinusQmaxAndReturnsWhenInventoryRises) {
  Harness h;
  sell_down_to(h, -9);
  h.deliver(fill(Side::Sell, 103, 1001, 1));  // q = -10
  auto s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Sell, 1001);
  h.deliver(ack(104, cid(s[0])));
  h.deliver(fill(Side::Sell, 104, 1001, 2));  // q = -12
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_cancel(s[0], 104);
  h.deliver(cancelled(104, 3));
  h.bbo(988, 1008);
  h.wake(3);
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_modify(s[0], 101, 997);
  h.deliver(modified(101, 997, 5));
  EXPECT_FALSE(h.ask().has_value());
  EXPECT_TRUE(h.sent().empty());

  h.deliver(fill(Side::Buy, 101, 997, 1));  // q = -11
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Sell, 999);
  EXPECT_EQ(h.mm.inventory(), -11);
}

// ---------------------------------------------------------------------------------------
// Races, each an explicit message script in the order the exchange emitted the events.
// ---------------------------------------------------------------------------------------

TEST(MarketMakerRace, A_FullFillThenModifyRejectedUnknownOrder) {
  Harness h;
  h.establish(990, 1010);
  h.bbo(992, 1012);  // Modify both
  ASSERT_EQ(h.sent().size(), 2u);
  // Exchange: bid 101 fully filled at 999 before the modify arrived; the modify is
  // rejected; the ask's modify succeeds.
  h.deliver(fill(Side::Buy, 101, 999, 5));
  EXPECT_FALSE(h.bid().has_value());
  EXPECT_EQ(h.mm.pending_kind(Side::Buy), Kind::Modify) << "a trade never clears the pending flag";
  EXPECT_TRUE(h.sent().empty());
  h.deliver(reject(RequestKind::Modify, 101, RejectReason::UnknownOrder));
  auto s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Buy, 1001);
  h.deliver(modified(102, 1003, 5));
  h.deliver(ack(103, cid(s[0])));
  EXPECT_EQ(h.mm.inventory(), 5);
  EXPECT_EQ(h.bid(), (Working{103, 1001, 5}));
  EXPECT_EQ(h.ask(), (Working{102, 1003, 5}));
  EXPECT_EQ(h.mm.rejects(RejectReason::UnknownOrder), 1u);
  h.expect_quiet_and_clean();
}

TEST(MarketMakerRace, B_PartialFillThenModifiedRestoresTheQuantity) {
  Harness h;
  h.establish(990, 1010);
  h.bbo(992, 1012);
  ASSERT_EQ(h.sent().size(), 2u);
  h.deliver(fill(Side::Buy, 101, 999, 2));  // before the modify arrived
  EXPECT_EQ(h.bid(), (Working{101, 999, 3}));
  h.deliver(modified(101, 1001, 5));  // the exchange set remaining = new_qty = 5
  h.deliver(modified(102, 1003, 5));
  EXPECT_TRUE(h.sent().empty());
  EXPECT_EQ(h.mm.inventory(), 2);
  EXPECT_EQ(h.bid(), (Working{101, 1001, 5}));
  EXPECT_EQ(h.ask(), (Working{102, 1003, 5}));
  h.expect_quiet_and_clean();
}

TEST(MarketMakerRace, C_FillThenCancelRejectedUnknownOrder) {
  Harness h;
  h.establish(990, 1010);
  h.mm.on_market_data(h.ctx, BBO{0, 0, 1010, 10});  // no quotes: cancel both
  ASSERT_EQ(h.sent().size(), 2u);
  h.deliver(fill(Side::Sell, 102, 1001, 5));  // the ask filled before its cancel arrived
  h.deliver(reject(RequestKind::Cancel, 102, RejectReason::UnknownOrder));
  h.deliver(cancelled(101, 5));
  EXPECT_TRUE(h.sent().empty()) << "still no two-sided BBO";
  EXPECT_EQ(h.mm.inventory(), -5);
  EXPECT_FALSE(h.bid().has_value());
  EXPECT_FALSE(h.ask().has_value());
  h.expect_quiet_and_clean();
  h.bbo(990, 1010);
  EXPECT_EQ(h.sent().size(), 2u);
}

TEST(MarketMakerRace, D_PartialFillThenCancelledForTheRest) {
  Harness h;
  h.establish(990, 1010);
  h.mm.on_market_data(h.ctx, BBO{990, 10, 0, 0});
  ASSERT_EQ(h.sent().size(), 2u);
  h.deliver(fill(Side::Buy, 101, 999, 2));
  EXPECT_EQ(h.bid(), (Working{101, 999, 3}));
  h.deliver(cancelled(101, 3));
  h.deliver(fill(Side::Sell, 102, 1001, 4));
  h.deliver(cancelled(102, 1));
  EXPECT_TRUE(h.sent().empty());
  EXPECT_EQ(h.mm.inventory(), -2);
  EXPECT_FALSE(h.bid().has_value());
  EXPECT_FALSE(h.ask().has_value());
  h.expect_quiet_and_clean();
}

TEST(MarketMakerRace, E_PostOnlyRejectOfNewThenFreshBboRequotes) {
  Harness h;
  h.bbo(990, 1010);
  auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  // The real book has an ask at 998 the agent has not seen yet: the bid at 999 would cross.
  h.deliver(reject_new(cid(s[0]), RejectReason::PostOnlyWouldCross));
  h.deliver(ack(102, cid(s[1])));
  EXPECT_EQ(h.mm.pending_kind(Side::Sell), Kind::None);
  // The next requote acts on the BBO it has, which is still the stale one: the same bid.
  auto s2 = h.sent();
  ASSERT_EQ(s2.size(), 1u);
  expect_new(s2[0], Side::Buy, 999);
  // The fresh BBO (m = 994) arrives while that New is pending: only the ask reprices.
  h.bbo(990, 998);
  auto s3 = h.sent();
  ASSERT_EQ(s3.size(), 1u);
  expect_modify(s3[0], 102, 995);
  h.deliver(reject_new(cid(s2[0]), RejectReason::PostOnlyWouldCross));
  auto s4 = h.sent();
  ASSERT_EQ(s4.size(), 1u);
  expect_new(s4[0], Side::Buy, 993);  // now from the fresh BBO
  h.deliver(ack(103, cid(s4[0])));
  h.deliver(modified(102, 995, 5));
  EXPECT_TRUE(h.sent().empty());
  EXPECT_EQ(h.bid(), (Working{103, 993, 5}));
  EXPECT_EQ(h.ask(), (Working{102, 995, 5}));
  EXPECT_EQ(h.mm.inventory(), 0);
  EXPECT_EQ(h.mm.rejects(RejectReason::PostOnlyWouldCross), 2u);
  h.expect_quiet_and_clean();
}

TEST(MarketMakerRace, F_PostOnlyRejectOfModifyLeavesTheOrderInPlace) {
  Harness h;
  h.establish(990, 1010);
  h.bbo(992, 1012);  // modify the ask 1001 -> 1003 and the bid 999 -> 1001
  ASSERT_EQ(h.sent().size(), 2u);
  // The bid at 1001 would cross a real ask at 1001: rejected, still resting at 999.
  h.deliver(reject(RequestKind::Modify, 101, RejectReason::PostOnlyWouldCross));
  EXPECT_EQ(h.bid(), (Working{101, 999, 5}));
  auto s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_modify(s[0], 101, 1001);  // same stale BBO, same request
  // A fresh BBO puts the desired bid back at 999, then the second modify is rejected too.
  h.bbo(990, 1010);
  h.deliver(reject(RequestKind::Modify, 101, RejectReason::PostOnlyWouldCross));
  h.deliver(modified(102, 1003, 5));
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_modify(s[0], 102, 1001);  // the ask follows the fresh BBO; the bid is already at 999
  h.deliver(modified(102, 1001, 5));
  EXPECT_TRUE(h.sent().empty());
  EXPECT_EQ(h.bid(), (Working{101, 999, 5}));
  EXPECT_EQ(h.ask(), (Working{102, 1001, 5}));
  EXPECT_EQ(h.mm.rejects(RejectReason::PostOnlyWouldCross), 2u);
  h.expect_quiet_and_clean();
}

TEST(MarketMakerRace, G_FillOfOneSideWhileTheOtherIsPending) {
  Harness h;
  h.establish(990, 1010);
  h.bbo(990, 1012);  // m = 1001: modify both
  auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  h.deliver(modified(101, 1000, 5));  // the bid's reply; the ask's modify is still out
  h.deliver(fill(Side::Buy, 101, 1000, 2));
  EXPECT_TRUE(h.sent().empty());
  h.deliver(fill(Side::Buy, 101, 1000, 3));  // the bid is gone: a new one, ask still pending
  s = h.sent();
  ASSERT_EQ(s.size(), 1u);
  expect_new(s[0], Side::Buy, 1000);
  EXPECT_EQ(h.mm.pending_kind(Side::Sell), Kind::Modify);
  h.deliver(modified(102, 1002, 5));
  h.deliver(ack(103, cid(s[0])));
  EXPECT_TRUE(h.sent().empty());
  EXPECT_EQ(h.mm.inventory(), 5);
  EXPECT_EQ(h.bid(), (Working{103, 1000, 5}));
  EXPECT_EQ(h.ask(), (Working{102, 1002, 5}));
  h.expect_quiet_and_clean();
}

TEST(MarketMaker, MessagesThatFitNothingAreCountedNotTrusted) {
  Harness h;
  h.establish(990, 1010);
  h.deliver(ack(200, 999));                                              // no such New
  h.deliver(modified(101, 950, 5));                                      // no pending Modify
  h.deliver(reject(RequestKind::Cancel, 101, RejectReason::UnknownOrder)); // no pending Cancel
  h.deliver(reject_new(42, RejectReason::InvalidQty));                   // no such New
  EXPECT_EQ(h.mm.anomalies(), 4u);
  EXPECT_EQ(h.bid(), (Working{101, 999, 5}));
  EXPECT_EQ(h.ask(), (Working{102, 1001, 5}));
  EXPECT_EQ(h.mm.total_rejects(), 0u) << "replies that match nothing are not counted as rejects";
  // A trade for an unknown order still moves the inventory (it happened), and is counted.
  h.deliver(fill(Side::Buy, 777, 999, 1));
  EXPECT_EQ(h.mm.anomalies(), 5u);
  EXPECT_EQ(h.mm.inventory(), 1);
  EXPECT_EQ(h.bid(), (Working{101, 999, 5}));
}

TEST(MarketMaker, NeverDrawsRandomNumbers) {
  Harness h;
  h.establish(990, 1010);
  h.wake(5);
  h.bbo(992, 1012);
  h.deliver(fill(Side::Buy, 101, 999, 2));
  EXPECT_EQ(h.ctx.r, Xoshiro256StarStar(11));
}

TEST(MarketMaker, SessionLongQuotesStraddleTheMidAndShiftWithInventory) {
  // SPEC 5.8 parameters over a one-hour session, BBO 9990 / 10010 (s = 10000).
  Harness h(spec_params(), kHour);
  h.bbo(9990, 10010);
  auto s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_new(s[0], Side::Buy, 9999);  // flat: straddles the mid
  expect_new(s[1], Side::Sell, 10001);
  h.deliver(ack(1, cid(s[0])));
  h.deliver(ack(2, cid(s[1])));

  // Long 5 at t = 0: r = 9998.2, bid = floor(9997.353) = 9997, ask = ceil(9999.047) = 10000.
  h.deliver(fill(Side::Buy, 1, 9999, 5));
  s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_new(s[0], Side::Buy, 9997);
  expect_modify(s[1], 2, 10000);
  h.deliver(ack(3, cid(s[0])));
  h.deliver(modified(2, 10000, 5));
  // Long 10: r = 9996.4: bid 9995 and ask 9998, both below the mid.
  h.deliver(fill(Side::Buy, 3, 9997, 5));
  s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_new(s[0], Side::Buy, 9995);
  expect_modify(s[1], 2, 9998);
  h.deliver(ack(4, cid(s[0])));
  h.deliver(modified(2, 9998, 5));
  EXPECT_LT(h.ask()->price, 10000) << "ask below the mid";

  // Late in the session the inventory term fades: with 1 ms left, back to 9999 / 10001.
  h.ctx.t = kHour - 1'000'000;
  h.bbo(9990, 10010);
  s = h.sent();
  ASSERT_EQ(s.size(), 2u);
  expect_modify(s[0], 4, 9999);
  expect_modify(s[1], 2, 10001);
  EXPECT_EQ(h.mm.inventory(), 10);
}
