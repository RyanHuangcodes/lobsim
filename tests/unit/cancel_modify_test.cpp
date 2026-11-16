// SPEC 3.3.6 (modify), 3.3.7 (cancel) and the cancel/modify validation of 3.2.
#include "support/book_harness.hpp"
#include "unit/book_types.hpp"

using namespace lob;
using namespace lob::test;
using enum CancelReason;
using enum RejectReason;

template <class B>
class CancelTest : public ::testing::Test {};
TYPED_TEST_SUITE(CancelTest, BookTypes);

template <class B>
class ModifyTest : public ::testing::Test {};
TYPED_TEST_SUITE(ModifyTest, BookTypes);

constexpr Price kMax = 1000;

// ------------------------------------------------------------------ cancel

TYPED_TEST(CancelTest, CancelsRemainingQtyAfterPartialFillAndOrderIsGone) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));  // id1
  h.submit(ioc(2, Sell, 100, 2));   // id2 takes 2
  h.cancel(1, 1);
  EXPECT_EQ(h.last(), (Events{cancelled(1, 1, 3, User)}));
  EXPECT_EQ(h.book().order(1), std::nullopt);
  EXPECT_EQ(h.book().resting_orders(), 0u);
  EXPECT_EQ(h.book().best_bid(), std::nullopt);
}

TYPED_TEST(CancelTest, UnknownIdNeverAssignedIsUnknownOrder) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));
  h.cancel(1, 99);
  EXPECT_EQ(h.last(), (Events{reject_cancel(1, 99, UnknownOrder)}));
  h.cancel(1, 0);
  EXPECT_EQ(h.last(), (Events{reject_cancel(1, 0, UnknownOrder)}));
}

TYPED_TEST(CancelTest, FullyFilledOrderIsUnknownOrder) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 2));  // id1
  h.submit(limit(2, Buy, 100, 2));   // id2 fills it completely
  h.cancel(1, 1);
  EXPECT_EQ(h.last(), (Events{reject_cancel(1, 1, UnknownOrder)}));
  h.cancel(2, 2);  // the taker never rested
  EXPECT_EQ(h.last(), (Events{reject_cancel(2, 2, UnknownOrder)}));
}

TYPED_TEST(CancelTest, AlreadyCancelledOrderIsUnknownOrder) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));
  h.cancel(1, 1);
  h.cancel(1, 1);
  EXPECT_EQ(h.last(), (Events{reject_cancel(1, 1, UnknownOrder)}));
}

TYPED_TEST(CancelTest, WrongOwnerIsNotOwnerAndLeavesOrderUntouched) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));
  const Snapshot before = h.snapshot();
  h.cancel(2, 1);
  EXPECT_EQ(h.last(), (Events{reject_cancel(2, 1, NotOwner)}));
  EXPECT_EQ(h.snapshot(), before);
  EXPECT_EQ(h.book().order(1)->remaining, 5);
}

// ------------------------------------------------------------------ modify

TYPED_TEST(ModifyTest, SamePriceSmallerQtyKeepsPriority) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));  // id1
  h.submit(limit(2, Buy, 100, 5));  // id2
  h.modify(1, 1, 100, 3);
  EXPECT_EQ(h.last(), (Events{modified(1, 1, 100, 3, true)}));
  h.submit(ioc(3, Sell, 100, 4));  // id3: id1 still fills first
  EXPECT_EQ(h.last(), (Events{ack(3, 3),
                              trade(1, 100, 3, 1, 3, 1, 3, Sell),
                              trade(2, 100, 1, 2, 3, 2, 3, Sell)}));
}

TYPED_TEST(ModifyTest, SamePriceLargerQtyLosesPriority) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));  // id1
  h.submit(limit(2, Buy, 100, 5));  // id2
  h.modify(1, 1, 100, 8);
  EXPECT_EQ(h.last(), (Events{modified(1, 1, 100, 8, false)}));
  EXPECT_EQ(h.book().best_level(Buy), (LevelView{100, 13}));
  h.submit(ioc(3, Sell, 100, 6));  // id3: id2 (now ahead) fills first
  EXPECT_EQ(h.last(), (Events{ack(3, 3),
                              trade(1, 100, 5, 2, 3, 2, 3, Sell),
                              trade(2, 100, 1, 1, 3, 1, 3, Sell)}));
  EXPECT_EQ(h.book().order(1)->remaining, 7);
}

TYPED_TEST(ModifyTest, PriceChangeToNonCrossingPriceLosesPriorityAndKeepsOrderId) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));  // id1, alone at 100
  h.submit(limit(2, Buy, 99, 3));   // id2
  h.submit(limit(3, Buy, 99, 2));   // id3
  h.submit(limit(4, Sell, 105, 5)); // id4
  h.modify(1, 1, 99, 5);
  EXPECT_EQ(h.last(), (Events{modified(1, 1, 99, 5, false)}));
  EXPECT_EQ(h.book().depth(Buy, 5), (std::vector<LevelView>{{99, 10}}));  // level 100 is gone
  EXPECT_EQ(h.book().best_bid(), 99);
  EXPECT_EQ(h.book().order(1)->price, 99);

  h.submit(ioc(5, Sell, 99, 6));  // id5: id2, id3, then id1 at the tail
  EXPECT_EQ(h.last(), (Events{ack(5, 5),
                              trade(1, 99, 3, 2, 5, 2, 5, Sell),
                              trade(2, 99, 2, 3, 5, 3, 5, Sell),
                              trade(3, 99, 1, 1, 5, 1, 5, Sell)}));
}

TYPED_TEST(ModifyTest, ToCrossingPriceEmitsModifiedFirstThenTradesAtMakerPricesAndRests) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 2));  // id1
  h.submit(limit(2, Sell, 102, 3));  // id2
  h.submit(limit(3, Buy, 100, 6));   // id3
  h.modify(3, 3, 102, 6);
  EXPECT_EQ(h.last(), (Events{modified(3, 3, 102, 6, false),
                              trade(1, 101, 2, 1, 3, 1, 3, Buy),
                              trade(2, 102, 3, 2, 3, 2, 3, Buy)}));
  EXPECT_EQ(h.book().order(3), (OrderView{3, 3, Buy, 102, 1, STP::CancelTaking, false}));
  EXPECT_EQ(h.book().best_ask(), std::nullopt);
  EXPECT_EQ(h.book().best_bid(), 102);
}

TYPED_TEST(ModifyTest, SellSideToCrossingPriceTradesWithAggressorSell) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 2));   // id1
  h.submit(limit(2, Sell, 105, 5));  // id2
  h.modify(2, 2, 99, 5);
  EXPECT_EQ(h.last(), (Events{modified(2, 2, 99, 5, false),
                              trade(1, 100, 2, 1, 2, 1, 2, Sell)}));
  EXPECT_EQ(h.book().order(2), (OrderView{2, 2, Sell, 99, 3, STP::CancelTaking, false}));
}

TYPED_TEST(ModifyTest, PostOnlyToCrossingPriceIsRejectedAndOrderUnchanged) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 3));      // id1
  h.submit(post_only(2, Buy, 100, 4));   // id2
  h.submit(limit(3, Buy, 100, 4));       // id3 behind id2
  const Snapshot before = h.snapshot();
  h.modify(2, 2, 101, 4);
  EXPECT_EQ(h.last(), (Events{reject_modify(2, 2, PostOnlyWouldCross)}));
  EXPECT_EQ(h.snapshot(), before);
  h.modify(2, 2, 102, 2);  // different qty as well: still rejected
  EXPECT_EQ(h.last(), (Events{reject_modify(2, 2, PostOnlyWouldCross)}));
  EXPECT_EQ(h.snapshot(), before);
  // Queue position kept: id2 still fills before id3.
  h.submit(ioc(4, Sell, 100, 4));
  EXPECT_EQ(h.last(), (Events{ack(4, 4), trade(1, 100, 4, 2, 4, 2, 4, Sell)}));
}

TYPED_TEST(ModifyTest, PostOnlyToNonCrossingPriceIsAcceptedAndStaysPostOnly) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 3));      // id1
  h.submit(post_only(2, Buy, 100, 4));   // id2
  h.modify(2, 2, 99, 4);
  EXPECT_EQ(h.last(), (Events{modified(2, 2, 99, 4, false)}));
  EXPECT_EQ(h.book().order(2), (OrderView{2, 2, Buy, 99, 4, STP::CancelTaking, true}));
  h.modify(2, 2, 101, 4);
  EXPECT_EQ(h.last(), (Events{reject_modify(2, 2, PostOnlyWouldCross)}));
  EXPECT_EQ(h.book().order(2)->price, 99);
}

TYPED_TEST(ModifyTest, NormalOrderToCrossingPriceIsNotRejected) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 3));  // id1
  h.submit(limit(2, Buy, 100, 2));   // id2, not post-only
  h.modify(2, 2, 101, 2);
  EXPECT_EQ(h.last(), (Events{modified(2, 2, 101, 2, false),
                              trade(1, 101, 2, 1, 2, 1, 2, Buy)}));
}

TYPED_TEST(ModifyTest, ValidationOrderUnknownThenOwnerThenQtyThenPriceThenNoChange) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  h.submit(limit(1, Buy, 100, 5));   // id1
  h.submit(limit(2, Sell, 105, 5));  // id2
  const Snapshot before = h.snapshot();

  h.modify(2, 99, 0, 0);  // unknown id beats everything, also a wrong owner
  EXPECT_EQ(h.last(), (Events{reject_modify(2, 99, UnknownOrder)}));
  h.modify(2, 1, 0, 0);   // wrong owner beats qty 0 and bad price
  EXPECT_EQ(h.last(), (Events{reject_modify(2, 1, NotOwner)}));
  h.modify(1, 1, 0, 0);   // qty 0 beats a bad price
  EXPECT_EQ(h.last(), (Events{reject_modify(1, 1, InvalidQty)}));
  h.modify(1, 1, kMax + 1, -3);
  EXPECT_EQ(h.last(), (Events{reject_modify(1, 1, InvalidQty)}));
  h.modify(1, 1, 0, 3);
  EXPECT_EQ(h.last(), (Events{reject_modify(1, 1, InvalidPrice)}));
  h.modify(1, 1, kMax + 1, 5);
  EXPECT_EQ(h.last(), (Events{reject_modify(1, 1, InvalidPrice)}));
  h.modify(1, 1, 100, 5);  // same price and same qty
  EXPECT_EQ(h.last(), (Events{reject_modify(1, 1, NoChange)}));
  EXPECT_EQ(h.snapshot(), before);
}

TYPED_TEST(ModifyTest, ReEntryKeepsStp_CancelRestingCancelsOwnMakerAndContinues) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5, STP::CancelResting));  // id1
  h.submit(limit(1, Sell, 105, 3));                      // id2, same owner
  h.submit(limit(2, Sell, 106, 4));                      // id3
  h.modify(1, 1, 106, 5);
  EXPECT_EQ(h.last(), (Events{modified(1, 1, 106, 5, false),
                              cancelled(2, 1, 3, SelfTrade),
                              trade(1, 106, 4, 3, 1, 2, 1, Buy)}));
  EXPECT_EQ(h.book().order(1), (OrderView{1, 1, Buy, 106, 1, STP::CancelResting, false}));
  EXPECT_EQ(h.book().order(2), std::nullopt);
}

TYPED_TEST(ModifyTest, ReEntryKeepsStp_CancelTakingCancelsTheModifiedOrdersRemainder) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));   // id1, CancelTaking
  h.submit(limit(2, Sell, 104, 2));  // id2
  h.submit(limit(1, Sell, 105, 3));  // id3, same owner as id1
  h.submit(limit(2, Sell, 106, 4));  // id4
  h.modify(1, 1, 106, 5);
  EXPECT_EQ(h.last(), (Events{modified(1, 1, 106, 5, false),
                              trade(1, 104, 2, 2, 1, 2, 1, Buy),
                              cancelled(1, 1, 3, SelfTrade)}));
  EXPECT_EQ(h.book().order(1), std::nullopt);
  EXPECT_EQ(h.book().order(3)->remaining, 3);
  EXPECT_EQ(h.book().order(4)->remaining, 4);
}

// ---- new_qty is measured against the REMAINING quantity (SPEC 3.1)

// id1 owner1 buy 100x5, id2 owner2 buy 100x5, id3 owner3 sells 2 into id1 (remaining 3).
template <class H>
void partially_filled_front(H& h) {
  h.submit(limit(1, Buy, 100, 5));
  h.submit(limit(2, Buy, 100, 5));
  h.submit(ioc(3, Sell, 100, 2));
  ASSERT_EQ(h.book().order(1)->remaining, 3);
}

TYPED_TEST(ModifyTest, NewQtyEqualToRemainingAfterPartialFillIsNoChange) {
  BookHarness<TypeParam> h;
  partially_filled_front(h);
  const Snapshot before = h.snapshot();
  h.modify(1, 1, 100, 3);
  EXPECT_EQ(h.last(), (Events{reject_modify(1, 1, NoChange)}));
  EXPECT_EQ(h.snapshot(), before);
}

TYPED_TEST(ModifyTest, NewQtyAboveRemainingButBelowOriginalLosesPriority) {
  BookHarness<TypeParam> h;
  partially_filled_front(h);
  h.modify(1, 1, 100, 4);
  EXPECT_EQ(h.last(), (Events{modified(1, 1, 100, 4, false)}));
  h.submit(ioc(3, Sell, 100, 6));  // id4: id2 is now ahead
  EXPECT_EQ(h.last(), (Events{ack(4, 3),
                              trade(2, 100, 5, 2, 4, 2, 3, Sell),
                              trade(3, 100, 1, 1, 4, 1, 3, Sell)}));
  EXPECT_EQ(h.book().order(1)->remaining, 3);
}

TYPED_TEST(ModifyTest, NewQtyBelowRemainingAfterPartialFillKeepsPriority) {
  BookHarness<TypeParam> h;
  partially_filled_front(h);
  h.modify(1, 1, 100, 2);
  EXPECT_EQ(h.last(), (Events{modified(1, 1, 100, 2, true)}));
  h.submit(ioc(3, Sell, 100, 3));  // id4: id1 still first
  EXPECT_EQ(h.last(), (Events{ack(4, 3),
                              trade(2, 100, 2, 1, 4, 1, 3, Sell),
                              trade(3, 100, 1, 2, 4, 2, 3, Sell)}));
}

// ---- price and quantity changed together (SPEC 3.3.6: any price change loses priority)

TYPED_TEST(ModifyTest, SmallerQtyWithNewNonCrossingPriceLosesPriorityAndRestsWithNewQty) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));  // id1
  h.submit(limit(2, Buy, 99, 3));   // id2
  h.modify(1, 1, 99, 2);
  EXPECT_EQ(h.last(), (Events{modified(1, 1, 99, 2, false)}));
  EXPECT_EQ(h.book().order(1)->remaining, 2);
  EXPECT_EQ(h.book().depth(Buy, 5), (std::vector<LevelView>{{99, 5}}));
  h.submit(ioc(3, Sell, 99, 4));  // id3: id2 first, then id1
  EXPECT_EQ(h.last(), (Events{ack(3, 3),
                              trade(1, 99, 3, 2, 3, 2, 3, Sell),
                              trade(2, 99, 1, 1, 3, 1, 3, Sell)}));
}

TYPED_TEST(ModifyTest, LargerQtyWithCrossingPriceEmitsModifiedThenTradeAndRestsRemainderAtNewPrice) {
  BookHarness<TypeParam> h;
  h.submit(limit(2, Sell, 101, 2));  // id1
  h.submit(limit(1, Buy, 100, 3));   // id2
  h.modify(1, 2, 101, 5);
  EXPECT_EQ(h.last(), (Events{modified(2, 1, 101, 5, false),
                              trade(1, 101, 2, 1, 2, 2, 1, Buy)}));
  EXPECT_EQ(h.book().order(2), (OrderView{2, 1, Buy, 101, 3, STP::CancelTaking, false}));
  EXPECT_EQ(h.book().best_bid(), 101);
  EXPECT_EQ(h.book().best_ask(), std::nullopt);
}
