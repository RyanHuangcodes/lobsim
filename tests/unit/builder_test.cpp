// The book-builder operations (D-151..D-155): insert_passive, reduce, remove, preview_match and
// level_orders, on both books. They change the book without matching and emit no events.
#include <vector>

#include "support/book_harness.hpp"
#include "unit/book_types.hpp"

using namespace lob;
using namespace lob::test;

namespace {

constexpr Price kMax = 1000;

template <class B>
class BuilderTest : public ::testing::Test {
 protected:
  // A normal order through submit(); the harness is not used because the builder operations
  // do not produce the events its conservation checker follows.
  void submit(const NewOrder& req) {
    EventBuffer out;
    book.submit(req, out);
  }
  static std::vector<OrderId> ids(const std::vector<OrderView>& orders) {
    std::vector<OrderId> out;
    for (const OrderView& o : orders) out.push_back(o.id);
    return out;
  }

  B book{BookConfig{kMax}};
};
TYPED_TEST_SUITE(BuilderTest, BookTypes);

}  // namespace

TYPED_TEST(BuilderTest, InsertPassiveRestsAtTheRequestedLevelAndReturnsAnId) {
  const Result<OrderId> r = this->book.insert_passive(Buy, 100, 7, 3);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.value, 1u);
  EXPECT_EQ(this->book.order(1), (OrderView{1, 3, Buy, 100, 7, STP::CancelTaking, false}));
  EXPECT_EQ(this->book.depth(Buy, 10), (std::vector<LevelView>{{100, 7}}));
  const Result<OrderId> s = this->book.insert_passive(Sell, 101, 2);
  EXPECT_EQ(s, (Result<OrderId>{Status::Ok, 2}));
  EXPECT_EQ(this->book.order(2)->owner, 0u);  // the default owner
  EXPECT_EQ(this->book.resting_orders(), 2u);
}

TYPED_TEST(BuilderTest, InsertPassiveWouldCrossOnBothSidesIncludingEqualPrice) {
  ASSERT_TRUE(this->book.insert_passive(Sell, 100, 5).ok());  // id1
  ASSERT_TRUE(this->book.insert_passive(Buy, 98, 5).ok());    // id2

  EXPECT_EQ(this->book.insert_passive(Buy, 100, 1), (Result<OrderId>{Status::WouldCross, 0}));  // equal
  EXPECT_EQ(this->book.insert_passive(Buy, 105, 1).status, Status::WouldCross);
  EXPECT_EQ(this->book.insert_passive(Sell, 98, 1), (Result<OrderId>{Status::WouldCross, 0}));  // equal
  EXPECT_EQ(this->book.insert_passive(Sell, 90, 1).status, Status::WouldCross);
  EXPECT_EQ(this->book.resting_orders(), 2u);
  EXPECT_EQ(this->book.depth(Buy, 10), (std::vector<LevelView>{{98, 5}}));
  EXPECT_EQ(this->book.depth(Sell, 10), (std::vector<LevelView>{{100, 5}}));

  EXPECT_TRUE(this->book.insert_passive(Buy, 99, 1).ok());    // one tick inside the spread: fine
  EXPECT_EQ(this->book.insert_passive(Sell, 99, 1).status, Status::WouldCross);  // equals the new bid at 99
}

TYPED_TEST(BuilderTest, FailedInsertPassiveConsumesNoId) {
  ASSERT_TRUE(this->book.insert_passive(Sell, 100, 5).ok());  // id1
  EXPECT_EQ(this->book.insert_passive(Buy, 100, 1).status, Status::WouldCross);
  EXPECT_EQ(this->book.insert_passive(Buy, 99, 0).status, Status::InvalidQty);
  EXPECT_EQ(this->book.insert_passive(Buy, 0, 1).status, Status::InvalidPrice);
  const Result<OrderId> next = this->book.insert_passive(Buy, 99, 1);
  EXPECT_EQ(next, (Result<OrderId>{Status::Ok, 2}));
}

TYPED_TEST(BuilderTest, InsertPassiveValidationPrecedenceIsQtyThenPriceThenCross) {
  ASSERT_TRUE(this->book.insert_passive(Sell, 100, 5).ok());
  // Bad qty wins over bad price; bad price wins over crossing (a buy above max_price also
  // sits above the ask).
  EXPECT_EQ(this->book.insert_passive(Buy, kMax + 1, 0).status, Status::InvalidQty);
  EXPECT_EQ(this->book.insert_passive(Buy, kMax + 1, -4).status, Status::InvalidQty);
  EXPECT_EQ(this->book.insert_passive(Buy, kMax + 1, 1).status, Status::InvalidPrice);
  EXPECT_EQ(this->book.insert_passive(Sell, 0, 1).status, Status::InvalidPrice);
  EXPECT_EQ(this->book.insert_passive(Buy, -5, 1).status, Status::InvalidPrice);
  EXPECT_EQ(this->book.insert_passive(Buy, 100, 1).status, Status::WouldCross);
  EXPECT_EQ(this->book.insert_passive(Buy, kMax - 1, 1).status, Status::WouldCross);  // 999 >= ask 100
  EXPECT_TRUE(this->book.insert_passive(Sell, kMax, 1).ok());                              // max_price is valid
  EXPECT_TRUE(this->book.insert_passive(Buy, 1, 1).ok());                                  // so is 1
}

TYPED_TEST(BuilderTest, InsertPassiveSharesTheIdCounterWithNewOrder) {
  EventBuffer out;
  this->book.submit(limit(1, Buy, 100, 5), out);                // id1
  EXPECT_EQ(this->book.insert_passive(Buy, 99, 3).value, 2u);   // id2
  out.clear();
  this->book.submit(limit(1, Buy, 98, 5), out);                 // id3
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], ack(3, 1));
  EXPECT_EQ(this->book.insert_passive(Sell, 120, 1).value, 4u);
}

TYPED_TEST(BuilderTest, InsertPassiveLandsAtTheTailOfItsLevel) {
  this->submit(limit(1, Buy, 100, 5));                          // id1
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 4, 2).ok());  // id2
  this->submit(limit(3, Buy, 100, 1));                          // id3
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 2, 4).ok());  // id4
  EXPECT_EQ(this->ids(this->book.level_orders(Buy, 100)), (std::vector<OrderId>{1, 2, 3, 4}));
  EXPECT_EQ(this->book.depth(Buy, 1), (std::vector<LevelView>{{100, 12}}));

  EventBuffer out;  // a taker meets them in that order
  this->book.submit(ioc(9, Sell, 100, 9), out);
  EXPECT_EQ(this->ids(this->book.level_orders(Buy, 100)), (std::vector<OrderId>{3, 4}));
  EXPECT_EQ(this->book.order(2), std::nullopt);
}

TYPED_TEST(BuilderTest, ReducePartialKeepsQueuePosition) {
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 5, 1).ok());  // id1
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 5, 2).ok());  // id2
  EXPECT_EQ(this->book.reduce(1, 2), Status::Ok);
  EXPECT_EQ(this->book.order(1)->remaining, 3);
  EXPECT_EQ(this->book.depth(Buy, 1), (std::vector<LevelView>{{100, 8}}));
  EXPECT_EQ(this->ids(this->book.level_orders(Buy, 100)), (std::vector<OrderId>{1, 2}));

  EventBuffer out;  // a later sell hits id1 first
  this->book.submit(ioc(9, Sell, 100, 1), out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(std::get<Trade>(out[1]).maker_order_id, 1u);
}

TYPED_TEST(BuilderTest, ReduceByExactRemainingRemovesTheOrder) {
  ASSERT_TRUE(this->book.insert_passive(Sell, 100, 5).ok());  // id1
  ASSERT_TRUE(this->book.insert_passive(Sell, 101, 5).ok());  // id2
  EXPECT_EQ(this->book.reduce(1, 5), Status::Ok);
  EXPECT_EQ(this->book.order(1), std::nullopt);
  EXPECT_EQ(this->book.depth(Sell, 10), (std::vector<LevelView>{{101, 5}}));  // its level is gone too
  EXPECT_EQ(this->book.resting_orders(), 1u);
}

TYPED_TEST(BuilderTest, ReduceBeyondRemainingRemovesAndReportsOversized) {
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 5).ok());  // id1
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 2).ok());  // id2
  EXPECT_EQ(this->book.reduce(1, 6), Status::Oversized);
  EXPECT_EQ(this->book.order(1), std::nullopt);
  EXPECT_EQ(this->book.depth(Buy, 10), (std::vector<LevelView>{{100, 2}}));
  EXPECT_EQ(this->ids(this->book.level_orders(Buy, 100)), (std::vector<OrderId>{2}));
}

TYPED_TEST(BuilderTest, ReduceRejectsNonPositiveQtyAndUnknownIds) {
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 5).ok());  // id1
  EXPECT_EQ(this->book.reduce(1, 0), Status::InvalidQty);
  EXPECT_EQ(this->book.reduce(1, -3), Status::InvalidQty);
  EXPECT_EQ(this->book.order(1)->remaining, 5);  // untouched
  EXPECT_EQ(this->book.reduce(99, 1), Status::UnknownOrder);  // never existed
  EXPECT_EQ(this->book.reduce(0, 1), Status::UnknownOrder);
  EXPECT_EQ(this->book.reduce(99, 0), Status::InvalidQty);  // qty is checked first
  EXPECT_EQ(this->book.reduce(1, 5), Status::Ok);
  EXPECT_EQ(this->book.reduce(1, 1), Status::UnknownOrder);  // already removed
  EXPECT_EQ(this->book.resting_orders(), 0u);
}

TYPED_TEST(BuilderTest, RemoveTakesTheOrderOutAndThenReportsUnknownOrder) {
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 5).ok());  // id1
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 3).ok());  // id2
  EXPECT_EQ(this->book.remove(1), Status::Ok);
  EXPECT_EQ(this->book.order(1), std::nullopt);
  EXPECT_EQ(this->book.depth(Buy, 1), (std::vector<LevelView>{{100, 3}}));
  EXPECT_EQ(this->book.remove(1), Status::UnknownOrder);
  EXPECT_EQ(this->book.remove(42), Status::UnknownOrder);
  EXPECT_EQ(this->book.remove(2), Status::Ok);
  EXPECT_EQ(this->book.resting_orders(), 0u);
  EXPECT_TRUE(this->book.depth(Buy, 1).empty());
}

TYPED_TEST(BuilderTest, PreviewMatchWalksLevelsBestFirstWithFifoInsideALevel) {
  ASSERT_TRUE(this->book.insert_passive(Sell, 100, 2, 1).ok());  // id1
  ASSERT_TRUE(this->book.insert_passive(Sell, 100, 3, 2).ok());  // id2
  ASSERT_TRUE(this->book.insert_passive(Sell, 101, 4, 3).ok());  // id3
  ASSERT_TRUE(this->book.insert_passive(Sell, 103, 5, 4).ok());  // id4
  ASSERT_TRUE(this->book.insert_passive(Sell, 102, 1, 5).ok());  // id5 (inserted late, better than 103)

  EXPECT_EQ(this->book.preview_match(Buy, 103, 100),
            (std::vector<Fill>{{1, 100, 2}, {2, 100, 3}, {3, 101, 4}, {5, 102, 1}, {4, 103, 5}}));
  EXPECT_TRUE(this->book.preview_match(Buy, 99, 10).empty());  // limit below the best ask
}

TYPED_TEST(BuilderTest, PreviewMatchStopsAtTheLimitAndAtQty) {
  ASSERT_TRUE(this->book.insert_passive(Sell, 100, 2).ok());  // id1
  ASSERT_TRUE(this->book.insert_passive(Sell, 101, 4).ok());  // id2
  ASSERT_TRUE(this->book.insert_passive(Sell, 102, 6).ok());  // id3

  EXPECT_EQ(this->book.preview_match(Buy, 101, 100), (std::vector<Fill>{{1, 100, 2}, {2, 101, 4}}));  // limit
  EXPECT_EQ(this->book.preview_match(Buy, 102, 5), (std::vector<Fill>{{1, 100, 2}, {2, 101, 3}}));  // partial last fill
  EXPECT_EQ(this->book.preview_match(Buy, 102, 2), (std::vector<Fill>{{1, 100, 2}}));               // exact
  EXPECT_TRUE(this->book.preview_match(Buy, 102, 0).empty());
  EXPECT_TRUE(this->book.preview_match(Buy, 102, -1).empty());
}

TYPED_TEST(BuilderTest, PreviewMatchSellTakerWalksTheBidsDownToItsLimit) {
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 2).ok());  // id1
  ASSERT_TRUE(this->book.insert_passive(Buy, 99, 3).ok());   // id2
  ASSERT_TRUE(this->book.insert_passive(Buy, 98, 4).ok());   // id3
  EXPECT_EQ(this->book.preview_match(Sell, 99, 100), (std::vector<Fill>{{1, 100, 2}, {2, 99, 3}}));
  EXPECT_EQ(this->book.preview_match(Sell, 1, 4), (std::vector<Fill>{{1, 100, 2}, {2, 99, 2}}));
  EXPECT_TRUE(this->book.preview_match(Sell, 101, 4).empty());
}

TYPED_TEST(BuilderTest, PreviewMatchIgnoresOwnersAndLeavesTheBookUnchanged) {
  ASSERT_TRUE(this->book.insert_passive(Sell, 100, 3, 7).ok());  // id1, the taker's own owner
  ASSERT_TRUE(this->book.insert_passive(Sell, 100, 3, 8).ok());  // id2
  ASSERT_TRUE(this->book.insert_passive(Sell, 101, 3, 7).ok());  // id3
  ASSERT_TRUE(this->book.insert_passive(Buy, 90, 3, 7).ok());    // id4

  const auto asks_before = this->book.depth(Sell, 10);
  const auto bids_before = this->book.depth(Buy, 10);
  const auto l100 = this->book.level_orders(Sell, 100);
  const auto l101 = this->book.level_orders(Sell, 101);

  // No self-trade prevention: owner 7's orders appear like anyone's.
  EXPECT_EQ(this->book.preview_match(Buy, 101, 8), (std::vector<Fill>{{1, 100, 3}, {2, 100, 3}, {3, 101, 2}}));

  EXPECT_EQ(this->book.depth(Sell, 10), asks_before);
  EXPECT_EQ(this->book.depth(Buy, 10), bids_before);
  EXPECT_EQ(this->book.level_orders(Sell, 100), l100);
  EXPECT_EQ(this->book.level_orders(Sell, 101), l101);
  EXPECT_EQ(this->book.resting_orders(), 4u);
  EXPECT_EQ(this->book.insert_passive(Sell, 150, 1).value, 5u);  // and no id was consumed
}

TYPED_TEST(BuilderTest, PreviewMatchOnAnEmptySideIsEmpty) {
  EXPECT_TRUE(this->book.preview_match(Buy, kMax, 5).empty());
  EXPECT_TRUE(this->book.preview_match(Sell, 1, 5).empty());
}

TYPED_TEST(BuilderTest, LevelOrdersListsOneLevelInFifoOrderAndIsEmptyWhereThereIsNone) {
  EXPECT_TRUE(this->book.level_orders(Buy, 100).empty());
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 5, 1).ok());  // id1
  ASSERT_TRUE(this->book.insert_passive(Buy, 99, 6, 2).ok());   // id2
  ASSERT_TRUE(this->book.insert_passive(Buy, 100, 4, 3).ok());  // id3
  EXPECT_EQ(this->book.level_orders(Buy, 100),
            (std::vector<OrderView>{{1, 1, Buy, 100, 5, STP::CancelTaking, false},
                                    {3, 3, Buy, 100, 4, STP::CancelTaking, false}}));
  EXPECT_EQ(this->ids(this->book.level_orders(Buy, 99)), (std::vector<OrderId>{2}));
  EXPECT_TRUE(this->book.level_orders(Buy, 98).empty());
  EXPECT_TRUE(this->book.level_orders(Sell, 100).empty());  // right price, wrong side
  EXPECT_TRUE(this->book.level_orders(Buy, -1).empty());
  EXPECT_TRUE(this->book.level_orders(Buy, kMax + 5).empty());
}
