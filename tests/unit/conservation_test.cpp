// The conservation checker must itself detect a violation (SPEC 3.8).
#include <gtest/gtest.h>

#include <map>

#include "support/conservation.hpp"

using namespace lob;
using namespace lob::test;

namespace {

NewOrder sell(OwnerId owner, Qty qty) {
  NewOrder o;
  o.owner = owner;
  o.side = Side::Sell;
  o.price = 100;
  o.qty = qty;
  return o;
}

// A fake book: id -> remaining.
ConservationChecker::Lookup lookup_in(const std::map<OrderId, Qty>& book) {
  return [&book](OrderId id) -> std::optional<OrderView> {
    auto it = book.find(id);
    if (it == book.end()) return std::nullopt;
    return OrderView{id, 0, Side::Sell, 100, it->second, STP::CancelTaking, false};
  };
}

}  // namespace

TEST(ConservationChecker, AcceptsAHonestStream) {
  ConservationChecker c;
  std::map<OrderId, Qty> book;
  book[1] = 10;
  EXPECT_TRUE(c.check(sell(1, 10), {Ack{1, 1, 0}}, lookup_in(book)).empty());

  NewOrder buy = sell(2, 4);
  buy.side = Side::Buy;
  book[1] = 6;
  EXPECT_TRUE(c.check(buy,
                      {Ack{2, 2, 0}, Trade{1, 100, 4, 1, 2, 1, 2, Side::Buy}},
                      lookup_in(book)).empty());
  book.erase(1);
  EXPECT_TRUE(c.check(Cancel{1, 1}, {Cancelled{1, 1, 6, CancelReason::User}}, lookup_in(book)).empty());
}

TEST(ConservationChecker, DetectsABookThatLosesALotAfterATrade) {
  ConservationChecker c;
  std::map<OrderId, Qty> book;
  book[1] = 10;
  ASSERT_TRUE(c.check(sell(1, 10), {Ack{1, 1, 0}}, lookup_in(book)).empty());

  NewOrder buy = sell(2, 4);
  buy.side = Side::Buy;
  // The events say 4 lots traded, but the book still reports all 10 resting.
  auto bad = c.check(buy, {Ack{2, 2, 0}, Trade{1, 100, 4, 1, 2, 1, 2, Side::Buy}}, lookup_in(book));
  ASSERT_EQ(bad.size(), 1u);
  EXPECT_NE(bad[0].find("order 1"), std::string::npos);
}

TEST(ConservationChecker, DetectsACancelWithTheWrongQuantity) {
  ConservationChecker c;
  std::map<OrderId, Qty> book;
  book[1] = 10;
  ASSERT_TRUE(c.check(sell(1, 10), {Ack{1, 1, 0}}, lookup_in(book)).empty());
  book.erase(1);
  auto bad = c.check(Cancel{1, 1}, {Cancelled{1, 1, 7, CancelReason::User}}, lookup_in(book));
  ASSERT_EQ(bad.size(), 1u);
  EXPECT_NE(bad[0].find("Cancelled qty 7"), std::string::npos);
}

TEST(ConservationChecker, DetectsOverfillAndAFinishedOrderStillInTheBook) {
  ConservationChecker c;
  std::map<OrderId, Qty> book;
  book[1] = 3;
  ASSERT_TRUE(c.check(sell(1, 3), {Ack{1, 1, 0}}, lookup_in(book)).empty());
  NewOrder buy = sell(2, 5);
  buy.side = Side::Buy;
  // 5 traded against a maker holding 3: the model goes negative.
  auto bad = c.check(buy, {Ack{2, 2, 0}, Trade{1, 100, 5, 1, 2, 1, 2, Side::Buy}}, lookup_in(book));
  EXPECT_FALSE(bad.empty());
}

TEST(ConservationChecker, DetectsATradeAgainstAnUnknownOrder) {
  ConservationChecker c;
  std::map<OrderId, Qty> book;
  NewOrder buy = sell(2, 5);
  buy.side = Side::Buy;
  auto bad = c.check(buy, {Ack{2, 2, 0}, Trade{1, 100, 5, 77, 2, 1, 2, Side::Buy}}, lookup_in(book));
  EXPECT_FALSE(bad.empty());
}
