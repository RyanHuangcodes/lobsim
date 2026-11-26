// BookConfig::expected_order_ids (D-260, D-261): pre-sizing OrderBook's order-id index changes
// nothing a caller can see, and ids beyond the estimate still work.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "support/splitmix64.hpp"

using namespace lob;
using lob::test::SplitMix64;

namespace {

constexpr Price kMaxPrice = 200;

// A seeded stream of valid and invalid requests: limit orders of every time in force, market and
// post-only orders, cancels and modifies of ids seen so far (some unknown), few owners so self-trade
// prevention triggers. The stream depends only on the seed and the ids it has handed out.
class Stream {
 public:
  explicit Stream(std::uint64_t seed) : rng_(seed) {}

  Request next() {
    const std::uint64_t kind = rng_.below(10);
    const auto owner = static_cast<OwnerId>(rng_.range(1, 3));
    if (kind < 6 || issued_ == 0) {
      const Side side = rng_.chance(50) ? Side::Buy : Side::Sell;
      const auto r = rng_.below(10);
      const OrderType type = r == 0 ? OrderType::Market : OrderType::Limit;
      const TIF tif = type == OrderType::Market ? TIF::IOC : (r == 1 ? TIF::IOC : (r == 2 ? TIF::FOK : TIF::GTC));
      const bool post_only = type == OrderType::Limit && tif == TIF::GTC && r == 3;
      const Price price = rng_.range(90, 110);
      const Qty qty = rng_.range(0, 10);  // 0 is an InvalidQty reject
      const STP stp = rng_.chance(50) ? STP::CancelTaking : STP::CancelResting;
      return NewOrder{owner, ++client_id_, side, type, tif, price, qty, post_only, stp};
    }
    const OrderId id = static_cast<OrderId>(rng_.range(1, static_cast<std::int64_t>(issued_) + 2));
    if (kind < 8) return Cancel{owner, id};
    return Modify{owner, id, rng_.range(90, 110), rng_.range(1, 10)};
  }

  // The ids an Ack handed out, so later cancels and modifies target real orders.
  void observe(const EventBuffer& events) {
    for (const Event& e : events) {
      if (const auto* a = std::get_if<Ack>(&e); a != nullptr && a->order_id > issued_) issued_ = a->order_id;
    }
  }

 private:
  SplitMix64 rng_;
  std::uint64_t client_id_{0};
  OrderId issued_{0};
};

TEST(PresizedIdIndex, SameEventsAndHashAsADefaultBook) {
  OrderBook plain(BookConfig{kMaxPrice});
  OrderBook sized(BookConfig{kMaxPrice, 1000});
  Stream stream(20261124);
  EventHasher h_plain, h_sized;
  EventBuffer ev_plain, ev_sized;
  std::uint64_t acks = 0;
  for (int i = 0; i < 10'000; ++i) {
    const Request req = stream.next();
    ev_plain.clear();
    ev_sized.clear();
    apply(plain, req, ev_plain);
    apply(sized, req, ev_sized);
    ASSERT_EQ(ev_plain, ev_sized) << "request " << i;
    h_plain.add(ev_plain);
    h_sized.add(ev_sized);
    for (const Event& e : ev_plain) acks += std::holds_alternative<Ack>(e) ? 1 : 0;
    stream.observe(ev_plain);
  }
  EXPECT_EQ(h_plain.value(), h_sized.value());
  EXPECT_EQ(plain.resting_orders(), sized.resting_orders());
  EXPECT_EQ(plain.depth(Side::Buy, 1000), sized.depth(Side::Buy, 1000));
  EXPECT_EQ(plain.depth(Side::Sell, 1000), sized.depth(Side::Sell, 1000));
  EXPECT_GT(acks, 1000u);  // the stream goes past the estimate, so the fallback growth ran too
}

TEST(PresizedIdIndex, IdsBeyondTheEstimateStillWork) {
  OrderBook book(BookConfig{1000, 1000});
  EventBuffer ev;
  // 2,000 resting orders, none crossing: bids 1..499, asks 501..999, cycled.
  for (OrderId k = 1; k <= 2000; ++k) {
    const bool buy = k % 2 == 1;
    const Price price = buy ? static_cast<Price>(1 + (k / 2) % 499) : static_cast<Price>(501 + (k / 2) % 499);
    ev.clear();
    book.submit(NewOrder{1, k, buy ? Side::Buy : Side::Sell, OrderType::Limit, TIF::GTC, price, 1, false,
                         STP::CancelTaking},
                ev);
    ASSERT_EQ(ev.size(), 1u);
    ASSERT_EQ(std::get<Ack>(ev[0]).order_id, k);
    ASSERT_NO_THROW(book.validate()) << "after order " << k;
  }
  EXPECT_EQ(book.resting_orders(), 2000u);
  // Every order, inside and beyond the estimate, can be found and cancelled.
  for (const OrderId id : {OrderId{1}, OrderId{1000}, OrderId{1001}, OrderId{2000}}) {
    ASSERT_TRUE(book.order(id).has_value()) << id;
    ev.clear();
    book.cancel(Cancel{1, id}, ev);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<Cancelled>(ev[0])) << id;
    ASSERT_NO_THROW(book.validate());
  }
  EXPECT_EQ(book.resting_orders(), 1996u);
}

TEST(PresizedIdIndex, ConfigDefaultIsGrowOnDemand) {
  EXPECT_EQ(BookConfig{}.expected_order_ids, 0u);
  EXPECT_EQ(BookConfig{100}, (BookConfig{100, 0}));
  EXPECT_NE(BookConfig{100}, (BookConfig{100, 5}));
}

}  // namespace
