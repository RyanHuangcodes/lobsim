#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <ostream>
#include <stdexcept>
#include <vector>

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/types.hpp"
#include "support/conservation.hpp"

// Readable gtest failure output for events (the variant printer prints each alternative).
namespace lob {
inline void PrintTo(const Ack& e, std::ostream* os) { *os << to_string(Event{e}); }
inline void PrintTo(const Reject& e, std::ostream* os) { *os << to_string(Event{e}); }
inline void PrintTo(const Trade& e, std::ostream* os) { *os << to_string(Event{e}); }
inline void PrintTo(const Cancelled& e, std::ostream* os) { *os << to_string(Event{e}); }
inline void PrintTo(const Modified& e, std::ostream* os) { *os << to_string(Event{e}); }
}  // namespace lob

namespace lob::test {

using Events = std::vector<Event>;
constexpr Side Buy = Side::Buy;
constexpr Side Sell = Side::Sell;

// ---- request builders (client_order_id is 0; set it on the result when it matters)
inline NewOrder limit(OwnerId owner, Side side, Price price, Qty qty, STP stp = STP::CancelTaking) {
  return NewOrder{owner, 0, side, OrderType::Limit, TIF::GTC, price, qty, false, stp};
}
inline NewOrder ioc(OwnerId owner, Side side, Price price, Qty qty, STP stp = STP::CancelTaking) {
  return NewOrder{owner, 0, side, OrderType::Limit, TIF::IOC, price, qty, false, stp};
}
inline NewOrder fok(OwnerId owner, Side side, Price price, Qty qty, STP stp = STP::CancelTaking) {
  return NewOrder{owner, 0, side, OrderType::Limit, TIF::FOK, price, qty, false, stp};
}
inline NewOrder post_only(OwnerId owner, Side side, Price price, Qty qty, STP stp = STP::CancelTaking) {
  return NewOrder{owner, 0, side, OrderType::Limit, TIF::GTC, price, qty, true, stp};
}
inline NewOrder market(OwnerId owner, Side side, Qty qty, STP stp = STP::CancelTaking, Price price = 0) {
  return NewOrder{owner, 0, side, OrderType::Market, TIF::IOC, price, qty, false, stp};
}

// ---- literal event builders
inline Event ack(OrderId id, OwnerId owner, std::uint64_t client_order_id = 0) {
  return Ack{id, owner, client_order_id};
}
inline Event trade(std::uint64_t trade_id, Price price, Qty qty, OrderId maker, OrderId taker,
                   OwnerId maker_owner, OwnerId taker_owner, Side aggressor) {
  return Trade{trade_id, price, qty, maker, taker, maker_owner, taker_owner, aggressor};
}
inline Event cancelled(OrderId id, OwnerId owner, Qty qty, CancelReason reason) {
  return Cancelled{id, owner, qty, reason};
}
inline Event modified(OrderId id, OwnerId owner, Price price, Qty qty, bool priority_kept) {
  return Modified{id, owner, price, qty, priority_kept};
}
inline Event reject_new(OwnerId owner, RejectReason reason, std::uint64_t client_order_id = 0) {
  return Reject{owner, RequestKind::New, client_order_id, 0, reason};
}
inline Event reject_cancel(OwnerId owner, OrderId id, RejectReason reason) {
  return Reject{owner, RequestKind::Cancel, 0, id, reason};
}
inline Event reject_modify(OwnerId owner, OrderId id, RejectReason reason) {
  return Reject{owner, RequestKind::Modify, 0, id, reason};
}

// Everything observable through the query API.
struct Snapshot {
  std::vector<LevelView> bids, asks;
  std::vector<OrderView> orders;  // resting orders, by ascending id
  bool operator==(const Snapshot&) const = default;
};

// Owns a book, sends requests, keeps the event stream, and after every message runs
// validate() and the quantity-conservation checker. Failures are reported through gtest.
template <class Book>
class BookHarness {
 public:
  explicit BookHarness(BookConfig cfg = {}) : book_(cfg), cfg_(cfg) {}

  // Each returns the events of that request (also available as last()).
  const Events& submit(const NewOrder& req) { return send(req); }
  const Events& cancel(OwnerId owner, OrderId id) { return send(Cancel{owner, id}); }
  const Events& modify(OwnerId owner, OrderId id, Price new_price, Qty new_qty) {
    return send(Modify{owner, id, new_price, new_qty});
  }

  const Events& last() const { return last_; }
  const Events& all() const { return all_; }
  Book& book() { return book_; }
  const Book& book() const { return book_; }
  Price max_price() const { return cfg_.max_price; }

  Snapshot snapshot() const {
    Snapshot s;
    s.bids = book_.depth(Side::Buy, 1000);
    s.asks = book_.depth(Side::Sell, 1000);
    for (OrderId id = 1; id <= highest_id_; ++id)
      if (auto o = book_.order(id)) s.orders.push_back(*o);
    return s;
  }

 private:
  const Events& send(const Request& req) {
    EventBuffer out;
    apply(book_, req, out);
    last_ = out;
    all_.insert(all_.end(), out.begin(), out.end());
    for (const Event& ev : out)
      if (const auto* a = std::get_if<Ack>(&ev)) highest_id_ = std::max(highest_id_, a->order_id);

    try {
      book_.validate();
    } catch (const std::logic_error& e) {
      ADD_FAILURE() << "validate() threw: " << e.what();
    }
    auto violations = checker_.check(req, out, [this](OrderId id) { return book_.order(id); });
    for (const std::string& v : violations) ADD_FAILURE() << "conservation: " << v;
    return last_;
  }

  Book book_;
  BookConfig cfg_;
  ConservationChecker checker_;
  Events last_;
  Events all_;
  OrderId highest_id_{0};
};

}  // namespace lob::test
