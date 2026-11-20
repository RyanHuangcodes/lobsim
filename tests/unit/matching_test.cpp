// SPEC 3.3.1 to 3.3.3 and the worked example of SPEC section 1.
#include "support/book_harness.hpp"
#include "unit/book_types.hpp"

using namespace lob;
using namespace lob::test;
using enum CancelReason;

template <class B>
class MatchingTest : public ::testing::Test {};
TYPED_TEST_SUITE(MatchingTest, BookTypes);

// Asks: A=id1 5@101 (owner 1), B=id2 3@101 (owner 2), C=id3 4@102 (owner 3). Buyer is owner 9.
template <class H>
void worked_example_asks(H& h) {
  h.submit(limit(1, Sell, 101, 5));
  h.submit(limit(2, Sell, 101, 3));
  h.submit(limit(3, Sell, 102, 4));
}

TYPED_TEST(MatchingTest, WorkedExampleLimit101_FillsAThenBThenRestsRemainder) {
  BookHarness<TypeParam> h;
  worked_example_asks(h);
  h.submit(limit(9, Buy, 101, 10));
  EXPECT_EQ(h.last(), (Events{ack(4, 9),
                              trade(1, 101, 5, 1, 4, 1, 9, Buy),
                              trade(2, 101, 3, 2, 4, 2, 9, Buy)}));
  EXPECT_EQ(h.book().best_bid(), 101);
  EXPECT_EQ(h.book().best_level(Buy), (LevelView{101, 2}));
  EXPECT_EQ(h.book().best_ask(), 102);
  EXPECT_EQ(h.book().best_level(Sell), (LevelView{102, 4}));
  EXPECT_EQ(h.book().order(4), (OrderView{4, 9, Buy, 101, 2, STP::CancelTaking, false}));
  EXPECT_EQ(h.book().order(1), std::nullopt);
  EXPECT_EQ(h.book().order(2), std::nullopt);
  EXPECT_EQ(h.book().order(3), (OrderView{3, 3, Sell, 102, 4, STP::CancelTaking, false}));
  EXPECT_EQ(h.book().resting_orders(), 2u);
}

TYPED_TEST(MatchingTest, WorkedExampleLimit105_FirstTwoFillsStillPrintAt101ThenSweepsC) {
  BookHarness<TypeParam> h;
  worked_example_asks(h);
  h.submit(limit(9, Buy, 105, 10));
  EXPECT_EQ(h.last(), (Events{ack(4, 9),
                              trade(1, 101, 5, 1, 4, 1, 9, Buy),
                              trade(2, 101, 3, 2, 4, 2, 9, Buy),
                              trade(3, 102, 2, 3, 4, 3, 9, Buy)}));
  EXPECT_EQ(h.book().best_bid(), std::nullopt);
  EXPECT_EQ(h.book().best_level(Sell), (LevelView{102, 2}));
  EXPECT_EQ(h.book().order(3), (OrderView{3, 3, Sell, 102, 2, STP::CancelTaking, false}));
  EXPECT_EQ(h.book().order(4), std::nullopt);
  EXPECT_EQ(h.book().resting_orders(), 1u);
}

TYPED_TEST(MatchingTest, TradePrintsAtMakerPrice_BuyTakerWithBetterLimit) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 5));
  h.submit(limit(2, Buy, 105, 3));
  EXPECT_EQ(h.last(), (Events{ack(2, 2), trade(1, 100, 3, 1, 2, 1, 2, Buy)}));
  EXPECT_EQ(h.book().order(1)->remaining, 2);
}

TYPED_TEST(MatchingTest, TradePrintsAtMakerPrice_SellTakerAggressorIsSell) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 5));
  h.submit(limit(2, Sell, 95, 3));
  EXPECT_EQ(h.last(), (Events{ack(2, 2), trade(1, 100, 3, 1, 2, 1, 2, Sell)}));
  EXPECT_EQ(h.book().order(1)->remaining, 2);
}

TYPED_TEST(MatchingTest, MultiLevelSweepBuyTakesBestPriceFirst) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 2));
  h.submit(limit(2, Sell, 102, 3));
  h.submit(limit(3, Sell, 103, 4));
  h.submit(limit(9, Buy, 103, 8));
  EXPECT_EQ(h.last(), (Events{ack(4, 9),
                              trade(1, 101, 2, 1, 4, 1, 9, Buy),
                              trade(2, 102, 3, 2, 4, 2, 9, Buy),
                              trade(3, 103, 3, 3, 4, 3, 9, Buy)}));
  EXPECT_EQ(h.book().depth(Sell, 5), (std::vector<LevelView>{{103, 1}}));
  EXPECT_EQ(h.book().best_bid(), std::nullopt);
}

TYPED_TEST(MatchingTest, MultiLevelSweepSellMirrorsBuy) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 2));
  h.submit(limit(2, Buy, 99, 3));
  h.submit(limit(3, Buy, 98, 4));
  h.submit(limit(9, Sell, 98, 8));
  EXPECT_EQ(h.last(), (Events{ack(4, 9),
                              trade(1, 100, 2, 1, 4, 1, 9, Sell),
                              trade(2, 99, 3, 2, 4, 2, 9, Sell),
                              trade(3, 98, 3, 3, 4, 3, 9, Sell)}));
  EXPECT_EQ(h.book().depth(Buy, 5), (std::vector<LevelView>{{98, 1}}));
  EXPECT_EQ(h.book().best_ask(), std::nullopt);
}

TYPED_TEST(MatchingTest, FifoWithinLevel_FillsInArrivalOrderAndPartialFillKeepsPlace) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 2));  // id1
  h.submit(limit(2, Sell, 100, 3));  // id2
  h.submit(limit(3, Sell, 100, 4));  // id3
  h.submit(limit(9, Buy, 100, 6));   // id4: 2 + 3 + 1
  EXPECT_EQ(h.last(), (Events{ack(4, 9),
                              trade(1, 100, 2, 1, 4, 1, 9, Buy),
                              trade(2, 100, 3, 2, 4, 2, 9, Buy),
                              trade(3, 100, 1, 3, 4, 3, 9, Buy)}));
  EXPECT_EQ(h.book().order(3)->remaining, 3);

  h.submit(limit(4, Sell, 100, 5));  // id5 queues behind the partially filled id3
  h.submit(limit(8, Buy, 100, 5));   // id6
  EXPECT_EQ(h.last(), (Events{ack(6, 8),
                              trade(4, 100, 3, 3, 6, 3, 8, Buy),
                              trade(5, 100, 2, 5, 6, 4, 8, Buy)}));
}

TYPED_TEST(MatchingTest, AckComesFirstIdsAndTradeIdsCountUpFromOne) {
  BookHarness<TypeParam> h;
  NewOrder first = limit(1, Sell, 100, 5);
  first.client_order_id = 7;
  h.submit(first);
  EXPECT_EQ(h.last(), (Events{ack(1, 1, 7)}));

  h.submit(limit(2, Buy, 100, 2));
  EXPECT_EQ(h.last(), (Events{ack(2, 2), trade(1, 100, 2, 1, 2, 1, 2, Buy)}));

  h.submit(limit(3, Buy, 100, 3));
  EXPECT_EQ(h.last(), (Events{ack(3, 3), trade(2, 100, 3, 1, 3, 1, 3, Buy)}));

  for (const Event& ev : h.all()) {
    if (const auto* t = std::get_if<Trade>(&ev)) {
      EXPECT_GE(t->trade_id, 1u);
    }
  }
  // Every request's first event is its Ack.
  h.submit(ioc(4, Buy, 90, 1));
  EXPECT_TRUE(std::holds_alternative<Ack>(h.last().front()));
  EXPECT_EQ(std::get<Ack>(h.last().front()).order_id, 4u);
}

TYPED_TEST(MatchingTest, GtcRemainderRestsAtItsLimitAndLaterOrdersQueueBehindIt) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 2));  // id1
  h.submit(limit(2, Buy, 101, 5));   // id2: trades 2, rests 3 @101
  EXPECT_EQ(h.last(), (Events{ack(2, 2), trade(1, 101, 2, 1, 2, 1, 2, Buy)}));
  EXPECT_EQ(h.book().order(2), (OrderView{2, 2, Buy, 101, 3, STP::CancelTaking, false}));
  h.submit(limit(3, Buy, 101, 4));   // id3 joins the same level behind the remainder
  EXPECT_EQ(h.book().best_level(Buy), (LevelView{101, 7}));
  h.submit(ioc(9, Sell, 101, 4));    // id4: the remainder fills first, then id3
  EXPECT_EQ(h.last(), (Events{ack(4, 9),
                              trade(2, 101, 3, 2, 4, 2, 9, Sell),
                              trade(3, 101, 1, 3, 4, 3, 9, Sell)}));
}

TYPED_TEST(MatchingTest, LimitIocPartialFillThenCancelsIocRemainderAndNothingRests) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));
  h.submit(ioc(2, Buy, 100, 5));
  EXPECT_EQ(h.last(), (Events{ack(2, 2),
                              trade(1, 100, 3, 1, 2, 1, 2, Buy),
                              cancelled(2, 2, 2, IOCRemainder)}));
  EXPECT_EQ(h.book().resting_orders(), 0u);
  EXPECT_EQ(h.book().best_bid(), std::nullopt);
}

TYPED_TEST(MatchingTest, LimitIocZeroFillIsAlsoIocRemainderNotNoLiquidity) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 100, 3));
  h.submit(ioc(2, Buy, 99, 4));
  EXPECT_EQ(h.last(), (Events{ack(2, 2), cancelled(2, 2, 4, IOCRemainder)}));
  EXPECT_EQ(h.book().resting_orders(), 1u);
  h.submit(ioc(3, Sell, 101, 4));  // empty bid side as well
  EXPECT_EQ(h.last(), (Events{ack(3, 3), cancelled(3, 3, 4, IOCRemainder)}));
}

TYPED_TEST(MatchingTest, MarketFillsAcrossLevelsAtAnyPriceAndIgnoresItsPriceField) {
  for (Price nonsense : {Price{0}, Price{-5}, Price{1}}) {
    BookHarness<TypeParam> h;
    h.submit(limit(1, Sell, 101, 2));
    h.submit(limit(2, Sell, 105, 3));
    h.submit(market(9, Buy, 4, STP::CancelTaking, nonsense));
    EXPECT_EQ(h.last(), (Events{ack(3, 9),
                                trade(1, 101, 2, 1, 3, 1, 9, Buy),
                                trade(2, 105, 2, 2, 3, 2, 9, Buy)}))
        << "price field " << nonsense;
    EXPECT_EQ(h.book().best_level(Sell), (LevelView{105, 1}));
    EXPECT_EQ(h.book().resting_orders(), 1u);
  }
}

TYPED_TEST(MatchingTest, MarketSellFillsAgainstBidsAtAnyPrice) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 2));
  h.submit(limit(2, Buy, 90, 3));
  h.submit(market(9, Sell, 5, STP::CancelTaking, -5));
  EXPECT_EQ(h.last(), (Events{ack(3, 9),
                              trade(1, 100, 2, 1, 3, 1, 9, Sell),
                              trade(2, 90, 3, 2, 3, 2, 9, Sell)}));
  EXPECT_EQ(h.book().resting_orders(), 0u);
}

TYPED_TEST(MatchingTest, MarketRemainderAfterSomeFillsIsIocRemainderAndNeverRests) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Sell, 101, 2));
  h.submit(market(9, Buy, 10));
  EXPECT_EQ(h.last(), (Events{ack(2, 9),
                              trade(1, 101, 2, 1, 2, 1, 9, Buy),
                              cancelled(2, 9, 8, IOCRemainder)}));
  EXPECT_EQ(h.book().resting_orders(), 0u);
  EXPECT_EQ(h.book().order(2), std::nullopt);
}

TYPED_TEST(MatchingTest, MarketOnEmptyOppositeSideIsAckThenNoLiquidity) {
  BookHarness<TypeParam> h;
  h.submit(market(9, Sell, 5));
  EXPECT_EQ(h.last(), (Events{ack(1, 9), cancelled(1, 9, 5, NoLiquidity)}));
  h.submit(limit(1, Sell, 100, 1));  // id2: asks exist, but a market sell needs bids
  h.submit(market(9, Sell, 3));
  EXPECT_EQ(h.last(), (Events{ack(3, 9), cancelled(3, 9, 3, NoLiquidity)}));
  EXPECT_EQ(h.book().resting_orders(), 1u);
}
