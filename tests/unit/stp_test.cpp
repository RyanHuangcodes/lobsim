// SPEC 3.4 self-trade prevention, including the FOK dry run.
#include "support/book_harness.hpp"
#include "unit/book_types.hpp"

using namespace lob;
using namespace lob::test;
using enum CancelReason;

template <class B>
class StpTest : public ::testing::Test {};
TYPED_TEST_SUITE(StpTest, BookTypes);

// Owner 1 is the taker in most tests; owner 2 and 3 are others.
// Layout: id1 owner2 sell 100x2, id2 owner1 sell 101x3, id3 owner3 sell 102x4.
template <class H>
void stp_asks(H& h) {
  h.submit(limit(2, Sell, 100, 2));
  h.submit(limit(1, Sell, 101, 3));
  h.submit(limit(3, Sell, 102, 4));
}

TYPED_TEST(StpTest, CancelTakingKeepsEarlierFillStopsAtOwnOrderAndLeavesLaterMakersUntouched) {
  BookHarness<TypeParam> h;
  stp_asks(h);
  h.submit(limit(1, Buy, 102, 10));  // id4, GTC
  EXPECT_EQ(h.last(), (Events{ack(4, 1),
                              trade(1, 100, 2, 1, 4, 2, 1, Buy),
                              cancelled(4, 1, 8, SelfTrade)}));
  EXPECT_EQ(h.book().order(4), std::nullopt);  // the GTC remainder does not rest
  EXPECT_EQ(h.book().best_bid(), std::nullopt);
  EXPECT_EQ(h.book().depth(Sell, 5), (std::vector<LevelView>{{101, 3}, {102, 4}}));
  EXPECT_EQ(h.book().order(2)->remaining, 3);
}

TYPED_TEST(StpTest, CancelRestingCancelsOwnMakerAndContinuesInMatchOrder) {
  BookHarness<TypeParam> h;
  stp_asks(h);
  h.submit(limit(1, Buy, 102, 10, STP::CancelResting));  // id4
  EXPECT_EQ(h.last(), (Events{ack(4, 1),
                              trade(1, 100, 2, 1, 4, 2, 1, Buy),
                              cancelled(2, 1, 3, SelfTrade),
                              trade(2, 102, 4, 3, 4, 3, 1, Buy)}));
  // 10 - 2 - 4 = 4 left, rests as a bid at its limit.
  EXPECT_EQ(h.book().order(4), (OrderView{4, 1, Buy, 102, 4, STP::CancelResting, false}));
  EXPECT_EQ(h.book().order(2), std::nullopt);
  EXPECT_EQ(h.book().best_ask(), std::nullopt);
}

TYPED_TEST(StpTest, TakersModeDecides_MakerRestingWithCancelRestingTakerCancelTaking) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3, STP::CancelResting));  // id1
  h.submit(limit(1, Buy, 100, 2, STP::CancelTaking));    // id2
  EXPECT_EQ(h.last(), (Events{ack(2, 1), cancelled(2, 1, 2, SelfTrade)}));
  EXPECT_EQ(h.book().order(1)->remaining, 3);  // maker untouched
}

TYPED_TEST(StpTest, TakersModeDecides_MakerRestingWithCancelTakingTakerCancelResting) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));  // id1, CancelTaking
  h.submit(limit(2, Sell, 100, 4));  // id2
  h.submit(limit(1, Buy, 100, 5, STP::CancelResting));  // id3
  EXPECT_EQ(h.last(), (Events{ack(3, 1),
                              cancelled(1, 1, 3, SelfTrade),
                              trade(1, 100, 4, 2, 3, 2, 1, Buy)}));
  EXPECT_EQ(h.book().order(3)->remaining, 1);  // rests at 100
}

TYPED_TEST(StpTest, MarketStoppedByCancelTakingBeforeAnyFillIsSelfTradeNotNoLiquidity) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));
  h.submit(market(1, Buy, 2));
  EXPECT_EQ(h.last(), (Events{ack(2, 1), cancelled(2, 1, 2, SelfTrade)}));
  EXPECT_EQ(h.book().order(1)->remaining, 3);
}

TYPED_TEST(StpTest, MarketCancelRestingWithOnlyOwnLiquidityCancelsThemThenNoLiquidity) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));  // id1
  h.submit(limit(1, Sell, 101, 2));  // id2
  h.submit(market(1, Buy, 4, STP::CancelResting));
  EXPECT_EQ(h.last(), (Events{ack(3, 1),
                              cancelled(1, 1, 3, SelfTrade),
                              cancelled(2, 1, 2, SelfTrade),
                              cancelled(3, 1, 4, NoLiquidity)}));
  EXPECT_EQ(h.book().resting_orders(), 0u);
}

TYPED_TEST(StpTest, FokCancelRestingSkipsOwnMakerWhenCountingAndExecutesCancellingIt) {
  BookHarness<TypeParam> h;
  stp_asks(h);
  h.submit(fok(1, Buy, 102, 6, STP::CancelResting));  // 2 + (own 3 skipped) + 4 = 6
  EXPECT_EQ(h.last(), (Events{ack(4, 1),
                              trade(1, 100, 2, 1, 4, 2, 1, Buy),
                              cancelled(2, 1, 3, SelfTrade),
                              trade(2, 102, 4, 3, 4, 3, 1, Buy)}));
  EXPECT_EQ(h.book().resting_orders(), 0u);
}

TYPED_TEST(StpTest, FokCancelRestingNotEnoughOtherQtyIsUnfillableAndOwnMakerNotCancelled) {
  BookHarness<TypeParam> h;
  stp_asks(h);
  const Snapshot before = h.snapshot();
  h.submit(fok(1, Buy, 102, 7, STP::CancelResting));  // others hold only 6
  EXPECT_EQ(h.last(), (Events{ack(4, 1), cancelled(4, 1, 7, FOKUnfillable)}));
  EXPECT_EQ(h.snapshot(), before);
  EXPECT_EQ(h.book().order(2)->remaining, 3);
}

TYPED_TEST(StpTest, FokCancelTakingFillsCompletelyWhenEnoughQtyComesBeforeOwnMaker) {
  BookHarness<TypeParam> h;
  stp_asks(h);
  h.submit(fok(1, Buy, 102, 2));
  EXPECT_EQ(h.last(), (Events{ack(4, 1), trade(1, 100, 2, 1, 4, 2, 1, Buy)}));
  EXPECT_EQ(h.book().resting_orders(), 2u);
}

TYPED_TEST(StpTest, FokCancelTakingReachingOwnMakerBeforeEnoughQtyIsUnfillableNothingChanges) {
  BookHarness<TypeParam> h;
  stp_asks(h);
  const Snapshot before = h.snapshot();
  h.submit(fok(1, Buy, 102, 3));  // 2 before the own order at 101; the walk ends there
  EXPECT_EQ(h.last(), (Events{ack(4, 1), cancelled(4, 1, 3, FOKUnfillable)}));
  EXPECT_EQ(h.snapshot(), before);
}

TYPED_TEST(StpTest, LimitIocCancelTakingFillsOthersThenStopsAtOwnOrderWithSelfTradeNotIocRemainder) {
  BookHarness<TypeParam> h;
  h.submit(limit(2, Sell, 100, 2));  // id1, other owner
  h.submit(limit(1, Sell, 101, 3));  // id2, own
  h.submit(ioc(1, Buy, 102, 5));     // id3
  EXPECT_EQ(h.last(), (Events{ack(3, 1),
                              trade(1, 100, 2, 1, 3, 2, 1, Buy),
                              cancelled(3, 1, 3, SelfTrade)}));
  EXPECT_EQ(h.book().order(2)->remaining, 3);  // own maker untouched
  EXPECT_EQ(h.book().order(3), std::nullopt);
}

TYPED_TEST(StpTest, LimitIocCancelTakingZeroFillAtOwnOrderIsSelfTradeNotIocRemainder) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));  // id1, own
  const Snapshot before = h.snapshot();
  h.submit(ioc(1, Buy, 100, 2));     // id2
  EXPECT_EQ(h.last(), (Events{ack(2, 1), cancelled(2, 1, 2, SelfTrade)}));
  EXPECT_EQ(h.snapshot(), before);
}
