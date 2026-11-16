// SPEC 3.5 query API and 3.8 invariants as seen from outside.
#include "support/book_harness.hpp"
#include "unit/book_types.hpp"

using namespace lob;
using namespace lob::test;

template <class B>
class QueryTest : public ::testing::Test {};
TYPED_TEST_SUITE(QueryTest, BookTypes);

TYPED_TEST(QueryTest, DepthListsLevelsBestFirstWithSummedQtyAndRespectsN) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 2));  // id1
  h.submit(limit(2, Sell, 101, 3));  // id2
  h.submit(limit(3, Sell, 102, 4));  // id3
  h.submit(limit(4, Sell, 104, 1));  // id4
  h.submit(limit(5, Buy, 100, 5));   // id5
  h.submit(limit(6, Buy, 99, 2));    // id6
  h.submit(limit(7, Buy, 99, 1));    // id7
  h.submit(limit(8, Buy, 98, 7));    // id8

  EXPECT_EQ(h.book().depth(Sell, 10), (std::vector<LevelView>{{101, 5}, {102, 4}, {104, 1}}));
  EXPECT_EQ(h.book().depth(Sell, 2), (std::vector<LevelView>{{101, 5}, {102, 4}}));
  EXPECT_EQ(h.book().depth(Sell, 1), (std::vector<LevelView>{{101, 5}}));
  EXPECT_EQ(h.book().depth(Buy, 10), (std::vector<LevelView>{{100, 5}, {99, 3}, {98, 7}}));
  EXPECT_EQ(h.book().depth(Buy, 2), (std::vector<LevelView>{{100, 5}, {99, 3}}));
  EXPECT_TRUE(h.book().depth(Buy, 0).empty());

  EXPECT_EQ(h.book().best_level(Sell), h.book().depth(Sell, 1).front());
  EXPECT_EQ(h.book().best_level(Buy), h.book().depth(Buy, 1).front());
  EXPECT_EQ(h.book().best_bid(), 100);
  EXPECT_EQ(h.book().best_ask(), 101);
}

TYPED_TEST(QueryTest, OrderReturnsFieldsOfRestingOrderAndNulloptOnceGone) {
  BookHarness<TypeParam> h;
  h.submit(limit(6, Buy, 99, 4, STP::CancelResting));  // id1
  h.submit(post_only(7, Sell, 105, 2));                // id2
  EXPECT_EQ(h.book().order(1), (OrderView{1, 6, Buy, 99, 4, STP::CancelResting, false}));
  EXPECT_EQ(h.book().order(2), (OrderView{2, 7, Sell, 105, 2, STP::CancelTaking, true}));
  EXPECT_EQ(h.book().order(3), std::nullopt);
  EXPECT_EQ(h.book().order(0), std::nullopt);
  EXPECT_EQ(h.book().resting_orders(), 2u);

  h.cancel(6, 1);
  EXPECT_EQ(h.book().order(1), std::nullopt);
  EXPECT_EQ(h.book().resting_orders(), 1u);
  h.submit(ioc(9, Buy, 105, 2));  // id3 takes id2
  EXPECT_EQ(h.book().order(2), std::nullopt);
  EXPECT_EQ(h.book().resting_orders(), 0u);
}

TYPED_TEST(QueryTest, EmptyBookHasNoBestPricesAndNoDepth) {
  BookHarness<TypeParam> h;
  EXPECT_EQ(h.book().best_bid(), std::nullopt);
  EXPECT_EQ(h.book().best_ask(), std::nullopt);
  EXPECT_EQ(h.book().best_level(Buy), std::nullopt);
  EXPECT_EQ(h.book().best_level(Sell), std::nullopt);
  EXPECT_TRUE(h.book().depth(Buy, 5).empty());
  EXPECT_TRUE(h.book().depth(Sell, 5).empty());
  EXPECT_EQ(h.book().resting_orders(), 0u);
}

TYPED_TEST(QueryTest, LevelEmptiedByAFillDisappearsAndBestAskMovesToNextLevel) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 2));  // id1
  h.submit(limit(2, Sell, 102, 3));  // id2
  h.submit(limit(3, Buy, 100, 1));   // id3
  h.submit(ioc(9, Buy, 101, 2));     // id4 empties level 101
  EXPECT_EQ(h.book().best_ask(), 102);
  EXPECT_EQ(h.book().depth(Sell, 5), (std::vector<LevelView>{{102, 3}}));
}

TYPED_TEST(QueryTest, LevelEmptiedByACancelDisappearsAndBestBidMovesToNextLevel) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 2));  // id1
  h.submit(limit(2, Buy, 99, 3));   // id2
  h.cancel(1, 1);
  EXPECT_EQ(h.book().best_bid(), 99);
  EXPECT_EQ(h.book().depth(Buy, 5), (std::vector<LevelView>{{99, 3}}));
  h.cancel(2, 2);
  EXPECT_EQ(h.book().best_bid(), std::nullopt);
  EXPECT_TRUE(h.book().depth(Buy, 5).empty());
}

TYPED_TEST(QueryTest, OneSideEmptyGivesNulloptForThatSideOnly) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 2));
  EXPECT_EQ(h.book().best_bid(), std::nullopt);
  EXPECT_EQ(h.book().best_ask(), 101);
  h.cancel(1, 1);
  EXPECT_EQ(h.book().best_ask(), std::nullopt);
}

TYPED_TEST(QueryTest, CrossingGtcLimitTradesInsteadOfRestingCrossed) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));  // id1
  h.submit(limit(2, Buy, 105, 5));   // id2: trades 3 at 100, rests 2 at 105
  EXPECT_EQ(h.last(), (Events{ack(2, 2), trade(1, 100, 3, 1, 2, 1, 2, Buy)}));
  EXPECT_EQ(h.book().best_bid(), 105);
  EXPECT_EQ(h.book().best_ask(), std::nullopt);
  EXPECT_NO_THROW(h.book().validate());

  h.submit(limit(3, Sell, 104, 1));  // id3 crosses the resting bid at 105: trades at 105
  EXPECT_EQ(h.last(), (Events{ack(3, 3), trade(2, 105, 1, 2, 3, 2, 3, Sell)}));
  EXPECT_EQ(h.book().best_level(Buy), (LevelView{105, 1}));
  EXPECT_EQ(h.book().best_ask(), std::nullopt);
  EXPECT_NO_THROW(h.book().validate());
}
