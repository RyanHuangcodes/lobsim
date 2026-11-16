// OrderBook::validate() is not vacuous: corrupt a healthy book through a white-box peer
// and expect the invariant of SPEC 3.8 that was broken to be named.
#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lob/apply.hpp"
#include "lob/order_book.hpp"
#include "lob/reference_book.hpp"
#include "support/splitmix64.hpp"

namespace lob {

struct OrderBookTestPeer {
  using Order = OrderBook::Order;
  using Level = OrderBook::Level;

  static Order* order(OrderBook& b, OrderId id) { return b.index_[id]; }
  static void clear_index_slot(OrderBook& b, OrderId id) { b.index_[id] = nullptr; }
  // A removal that forgets to clear the id-index slot: unlinked and back in the pool.
  static void remove_but_keep_index_slot(OrderBook& b, OrderId id) {
    Order* o = b.index_[id];
    b.unlink(o);
    b.pool_.destroy(o);
  }
  // A scan of the whole id index, independent of the book's own `indexed_` counter:
  // (number of non-null slots, every non-null slot holds the order with that id).
  static std::pair<std::size_t, bool> scan_index(const OrderBook& b) {
    std::size_t non_null = 0;
    bool ids_match = true;
    for (std::size_t id = 0; id < b.index_.size(); ++id) {
      const Order* o = b.index_[id];
      if (o == nullptr) continue;
      ++non_null;
      if (o->id != id) ids_match = false;
    }
    return {non_null, ids_match};
  }
  static std::size_t pool_chunks(const OrderBook& b) { return b.pool_.chunks(); }
  // Rests an order without matching, bypassing every check, as a buggy book might.
  static void force_rest(OrderBook& b, OrderId id, OwnerId owner, Side side, Price price, Qty qty) {
    b.rest(OrderBook::Taker{id, owner, side, false, price, qty, STP::CancelTaking}, false);
  }
};

}  // namespace lob

using namespace lob;
using Peer = OrderBookTestPeer;

namespace {

NewOrder gtc(OwnerId owner, Side side, Price price, Qty qty) {
  return NewOrder{owner, 0, side, OrderType::Limit, TIF::GTC, price, qty, false, STP::CancelTaking};
}

// id1, id2 bid 100 (3 and 4 lots), id3 bid 99, id4 ask 101.
void healthy(OrderBook& b) {
  EventBuffer out;
  b.submit(gtc(1, Side::Buy, 100, 3), out);
  b.submit(gtc(2, Side::Buy, 100, 4), out);
  b.submit(gtc(3, Side::Buy, 99, 5), out);
  b.submit(gtc(4, Side::Sell, 101, 6), out);
  ASSERT_EQ(b.resting_orders(), 4u);
  ASSERT_NO_THROW(b.validate());
}

// validate() must throw std::logic_error whose message contains `needle`.
void expect_invalid(const OrderBook& b, const std::string& needle) {
  try {
    b.validate();
    ADD_FAILURE() << "validate() did not throw; expected: " << needle;
  } catch (const std::logic_error& e) {
    EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << "message was: " << e.what();
  }
}

}  // namespace

TEST(OrderBookValidate, HealthyBookPasses) {
  OrderBook b;
  healthy(b);
}

TEST(OrderBookValidate, WrongLevelTotalQty) {
  OrderBook b;
  healthy(b);
  Peer::order(b, 1)->level->total_qty += 1;
  expect_invalid(b, "total_qty");
}

TEST(OrderBookValidate, LevelCountTooHigh) {
  OrderBook b;
  healthy(b);
  Peer::order(b, 1)->level->count += 1;
  expect_invalid(b, "count");
}

TEST(OrderBookValidate, LevelCountTooLow) {
  OrderBook b;
  healthy(b);
  Peer::order(b, 1)->level->count -= 1;
  expect_invalid(b, "count");
}

TEST(OrderBookValidate, IdIndexEntryClearedForRestingOrder) {
  OrderBook b;
  healthy(b);
  Peer::clear_index_slot(b, 2);
  expect_invalid(b, "id index does not point at it");
}

TEST(OrderBookValidate, IdIndexEntryLeftBehindForARemovedOrder) {
  OrderBook b;
  healthy(b);
  Peer::remove_but_keep_index_slot(b, 1);  // id2 still rests at 100, so the level stays
  expect_invalid(b, "non-null entries");
}

TEST(OrderBookValidate, OrderWithZeroRemainingLeftInALevel) {
  OrderBook b;
  healthy(b);
  Peer::Order* o = Peer::order(b, 1);
  o->level->total_qty -= o->remaining;  // keep the level total consistent
  o->remaining = 0;
  expect_invalid(b, "remaining <= 0");
}

TEST(OrderBookValidate, CrossedBook) {
  OrderBook b;
  healthy(b);
  Peer::force_rest(b, 5, 5, Side::Sell, 99, 2);  // an ask below the best bid of 100
  expect_invalid(b, "crossed");
}

TEST(OrderBookValidate, BrokenListLinks) {
  OrderBook b;
  healthy(b);
  Peer::Order* second = Peer::order(b, 2);
  second->prev = nullptr;  // id1 -> id2 but id2 no longer points back
  expect_invalid(b, "links disagree");
}

TEST(OrderBookValidate, OrderOnTheWrongSide) {
  OrderBook b;
  healthy(b);
  Peer::order(b, 3)->side = Side::Sell;
  expect_invalid(b, "side differs");
}

TEST(OrderBookValidate, OrderPriceDiffersFromItsLevel) {
  OrderBook b;
  healthy(b);
  Peer::order(b, 2)->price = 98;
  expect_invalid(b, "price differs");
}

// The fuzz test keeps the book below about 500 orders, inside one pool chunk. This test
// grows OrderBook past two chunks (4096 orders each), frees slots all over them, reuses
// those slots, and drains the book, comparing every event with ReferenceBook.
TEST(OrderBookDeepBook, GrowsAcrossPoolChunksReusesSlotsAndDrainsLikeReferenceBook) {
  const BookConfig cfg{2000};
  OrderBook fast(cfg);
  ReferenceBook ref(cfg);
  lob::test::SplitMix64 rng(20261116);
  EventHasher hash_fast, hash_ref;
  EventBuffer ev_fast, ev_ref;
  std::size_t message = 0;

  auto send = [&](const Request& req) {
    ev_fast.clear();
    ev_ref.clear();
    apply(fast, req, ev_fast);
    apply(ref, req, ev_ref);
    ASSERT_EQ(ev_fast, ev_ref) << "message " << message;
    hash_fast.add(ev_fast);
    hash_ref.add(ev_ref);
    ++message;
  };
  // Bids in [1, 900] and asks in [1100, 2000]: nothing crosses while the book is built.
  auto passive = [&]() {
    const Side side = rng.chance(50) ? Side::Buy : Side::Sell;
    const Price price = side == Side::Buy ? rng.range(1, 900) : rng.range(1100, 2000);
    const Qty qty = rng.range(1, 9);
    const auto owner = static_cast<OwnerId>(rng.range(1, 4));
    return NewOrder{owner, message, side, OrderType::Limit, TIF::GTC, price, qty, false, STP::CancelTaking};
  };
  auto check_index = [&]() {
    const auto [non_null, ids_match] = Peer::scan_index(fast);
    EXPECT_EQ(non_null, fast.resting_orders());
    EXPECT_TRUE(ids_match);
    EXPECT_EQ(fast.resting_orders(), ref.resting_orders());
    EXPECT_NO_THROW(fast.validate());
  };

  // Grow: 8,500 resting orders need three chunks.
  for (int i = 0; i < 8500; ++i) {
    send(passive());
    if (HasFatalFailure()) return;
  }
  ASSERT_EQ(fast.resting_orders(), 8500u);
  EXPECT_GE(Peer::pool_chunks(fast), 3u);
  check_index();

  // Free about 40% of the slots, scattered over all chunks.
  std::vector<OrderId> kept;
  for (OrderId id = 1; id <= 8500; ++id) {
    if (!rng.chance(40)) {
      kept.push_back(id);
      continue;
    }
    send(Cancel{fast.order(id)->owner, id});
    if (HasFatalFailure()) return;
  }
  ASSERT_EQ(fast.resting_orders(), kept.size());
  check_index();

  // Reuse the freed slots without growing the pool, and reprice some surviving orders
  // (a modify that loses priority returns its node and takes another).
  const std::size_t chunks_before_reuse = Peer::pool_chunks(fast);
  for (int i = 0; i < 2000; ++i) {
    send(passive());
    if (HasFatalFailure()) return;
  }
  for (std::size_t i = 0; i < kept.size(); i += 7) {
    const auto o = fast.order(kept[i]);
    ASSERT_TRUE(o.has_value());
    const Price moved = o->side == Side::Buy ? rng.range(1, 900) : rng.range(1100, 2000);
    send(Modify{o->owner, o->id, moved, o->remaining + 1});
    if (HasFatalFailure()) return;
  }
  EXPECT_EQ(Peer::pool_chunks(fast), chunks_before_reuse);
  check_index();

  // Drain both sides with market orders from an owner that has nothing resting.
  for (int i = 0; i < 400 && fast.resting_orders() > 0; ++i) {
    const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
    send(NewOrder{9, message, side, OrderType::Market, TIF::IOC, 0, 400, false, STP::CancelTaking});
    if (HasFatalFailure()) return;
  }
  EXPECT_EQ(fast.resting_orders(), 0u);
  EXPECT_EQ(fast.best_bid(), std::nullopt);
  EXPECT_EQ(fast.best_ask(), std::nullopt);
  check_index();
  EXPECT_EQ(hash_fast.value(), hash_ref.value());
}
