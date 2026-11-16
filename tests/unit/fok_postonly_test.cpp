// SPEC 3.3.4 (FOK) and 3.3.5 (post-only).
#include "support/book_harness.hpp"
#include "unit/book_types.hpp"

using namespace lob;
using namespace lob::test;
using enum CancelReason;
using enum RejectReason;

template <class B>
class FokTest : public ::testing::Test {};
TYPED_TEST_SUITE(FokTest, BookTypes);

template <class B>
class PostOnlyTest : public ::testing::Test {};
TYPED_TEST_SUITE(PostOnlyTest, BookTypes);

TYPED_TEST(FokTest, ExactFillAcrossTwoLevelsTradesWithNoCancelAndNothingRests) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));
  h.submit(limit(2, Sell, 101, 2));
  h.submit(fok(9, Buy, 101, 5));
  EXPECT_EQ(h.last(), (Events{ack(3, 9),
                              trade(1, 100, 3, 1, 3, 1, 9, Buy),
                              trade(2, 101, 2, 2, 3, 2, 9, Buy)}));
  EXPECT_EQ(h.book().resting_orders(), 0u);
}

TYPED_TEST(FokTest, OneLotShortIsAckThenFokUnfillableAndBookUnchanged) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));  // id1
  h.submit(limit(2, Sell, 101, 2));  // id2
  h.submit(limit(3, Buy, 99, 4));    // id3
  const Snapshot before = h.snapshot();
  ASSERT_EQ(before.orders.size(), 3u);

  h.submit(fok(9, Buy, 101, 6));  // 5 available, 6 wanted
  EXPECT_EQ(h.last(), (Events{ack(4, 9), cancelled(4, 9, 6, FOKUnfillable)}));
  EXPECT_EQ(h.snapshot(), before);

  // It still consumed an OrderId.
  h.submit(limit(5, Buy, 90, 1));
  EXPECT_EQ(h.last(), (Events{ack(5, 5)}));
}

TYPED_TEST(FokTest, SellSideOneLotShortIsAlsoUnfillable) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 3));
  h.submit(limit(2, Buy, 99, 2));
  const Snapshot before = h.snapshot();
  h.submit(fok(9, Sell, 99, 6));
  EXPECT_EQ(h.last(), (Events{ack(3, 9), cancelled(3, 9, 6, FOKUnfillable)}));
  EXPECT_EQ(h.snapshot(), before);
}

TYPED_TEST(FokTest, LimitedByPriceEnoughTotalQuantityButNotWithinLimitIsUnfillable) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));
  h.submit(limit(2, Sell, 102, 5));
  const Snapshot before = h.snapshot();
  h.submit(fok(9, Buy, 101, 5));  // 8 on the book, only 3 at or below 101
  EXPECT_EQ(h.last(), (Events{ack(3, 9), cancelled(3, 9, 5, FOKUnfillable)}));
  EXPECT_EQ(h.snapshot(), before);
}

TYPED_TEST(PostOnlyTest, BuyAtOrAboveBestAskIsRejectedBookUnchangedNoIdConsumed) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 5));
  const Snapshot before = h.snapshot();

  NewOrder at_ask = post_only(2, Buy, 100, 1);
  at_ask.client_order_id = 5;
  h.submit(at_ask);
  EXPECT_EQ(h.last(), (Events{reject_new(2, PostOnlyWouldCross, 5)}));
  h.submit(post_only(2, Buy, 101, 1));
  EXPECT_EQ(h.last(), (Events{reject_new(2, PostOnlyWouldCross)}));
  EXPECT_EQ(h.snapshot(), before);

  h.submit(limit(3, Buy, 99, 1));
  EXPECT_EQ(h.last(), (Events{ack(2, 3)}));  // the rejects consumed no id
}

TYPED_TEST(PostOnlyTest, SellAtOrBelowBestBidIsRejectedBookUnchangedNoIdConsumed) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));
  const Snapshot before = h.snapshot();
  h.submit(post_only(2, Sell, 100, 1));
  EXPECT_EQ(h.last(), (Events{reject_new(2, PostOnlyWouldCross)}));
  h.submit(post_only(2, Sell, 99, 1));
  EXPECT_EQ(h.last(), (Events{reject_new(2, PostOnlyWouldCross)}));
  EXPECT_EQ(h.snapshot(), before);

  h.submit(limit(3, Sell, 101, 1));
  EXPECT_EQ(h.last(), (Events{ack(2, 3)}));
}

TYPED_TEST(PostOnlyTest, OneTickInsideTheSpreadRestsNormally) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 99, 5));    // id1
  h.submit(limit(2, Sell, 101, 5));  // id2
  h.submit(post_only(3, Buy, 100, 2));
  EXPECT_EQ(h.last(), (Events{ack(3, 3)}));
  EXPECT_EQ(h.book().order(3), (OrderView{3, 3, Buy, 100, 2, STP::CancelTaking, true}));
  EXPECT_EQ(h.book().best_bid(), 100);
}

TYPED_TEST(PostOnlyTest, SellOneTickInsideTheSpreadRests) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 99, 5));
  h.submit(limit(2, Sell, 101, 5));
  h.submit(post_only(3, Sell, 100, 2));
  EXPECT_EQ(h.last(), (Events{ack(3, 3)}));
  EXPECT_EQ(h.book().best_ask(), 100);
}

TYPED_TEST(PostOnlyTest, IntoAnEmptyOppositeSideRests) {
  BookHarness<TypeParam> h;
  h.submit(post_only(1, Buy, 500, 2));
  EXPECT_EQ(h.last(), (Events{ack(1, 1)}));
  EXPECT_EQ(h.book().best_bid(), 500);
  h.submit(post_only(2, Sell, 400, 2));  // bid side non-empty here, so it crosses
  EXPECT_EQ(h.last(), (Events{reject_new(2, PostOnlyWouldCross)}));
}

TYPED_TEST(PostOnlyTest, BuyAtOwnersOwnBestAskIsStillRejectedWithoutStpEvent) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 5));  // id1, same owner
  const Snapshot before = h.snapshot();
  h.submit(post_only(1, Buy, 100, 1));
  EXPECT_EQ(h.last(), (Events{reject_new(1, PostOnlyWouldCross)}));
  EXPECT_EQ(h.snapshot(), before);
  h.submit(limit(3, Buy, 99, 1));
  EXPECT_EQ(h.last(), (Events{ack(2, 3)}));  // no id consumed
}

TYPED_TEST(PostOnlyTest, SellAtOwnersOwnBestBidIsStillRejectedWithoutStpEvent) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));  // id1, same owner
  const Snapshot before = h.snapshot();
  h.submit(post_only(1, Sell, 100, 1));
  EXPECT_EQ(h.last(), (Events{reject_new(1, PostOnlyWouldCross)}));
  EXPECT_EQ(h.snapshot(), before);
  h.submit(limit(3, Sell, 101, 1));
  EXPECT_EQ(h.last(), (Events{ack(2, 3)}));  // no id consumed
}
