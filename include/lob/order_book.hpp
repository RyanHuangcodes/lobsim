#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "lob/events.hpp"
#include "lob/pool.hpp"
#include "lob/types.hpp"

namespace lob {

struct OrderBookTestPeer;  // white-box access for the validate() tests only

// The fast book of SPEC 3.5: one std::map of price levels per side, each level an
// intrusive doubly linked FIFO of pool-allocated orders, and an id-indexed vector of
// Order* for O(1) lookup. Same API and semantics as ReferenceBook (D-010).
class OrderBook {
 public:
  explicit OrderBook(BookConfig cfg = {});
  // Orders point into map nodes and the pool, so a book is never copied or moved.
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  void submit(const NewOrder& req, EventBuffer& out);
  void cancel(const Cancel& req, EventBuffer& out);
  void modify(const Modify& req, EventBuffer& out);

  // Book-builder operations (D-151..D-155): change the book without matching and without
  // events. A failed call changes nothing and consumes no id.
  Result<OrderId> insert_passive(Side side, Price price, Qty qty, OwnerId owner = 0);
  Status reduce(OrderId id, Qty qty);
  Status remove(OrderId id);
  std::vector<Fill> preview_match(Side taker_side, Price limit, Qty qty) const;

  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  std::optional<LevelView> best_level(Side side) const;
  std::vector<LevelView> depth(Side side, std::size_t n) const;
  // The same levels written into `out` (cleared first), reusing its capacity: no allocation once
  // `out` has grown, for callers that read the book after every message (D-906).
  void depth(Side side, std::size_t n, std::vector<LevelView>& out) const;
  std::optional<OrderView> order(OrderId id) const;
  std::vector<OrderView> level_orders(Side side, Price price) const;
  std::size_t resting_orders() const;
  void validate() const;  // throws std::logic_error naming the first broken invariant

 private:
  friend struct OrderBookTestPeer;

  struct Level;
  struct Order {
    OrderId id;
    OwnerId owner;
    Side side;
    Price price;
    Qty remaining;
    STP stp;
    bool post_only;
    Order* prev;
    Order* next;
    Level* level;
  };
  struct Level {
    Price price;
    Qty total_qty;
    std::uint32_t count;
    Order* head;
    Order* tail;
  };
  using Bids = std::map<Price, Level, std::greater<Price>>;
  using Asks = std::map<Price, Level>;

  // The incoming side of a match: a new order or a modified order re-entering.
  struct Taker {
    OrderId id;
    OwnerId owner;
    Side side;
    bool is_market;
    Price limit;  // ignored for market orders
    Qty remaining;
    STP stp;
  };

  struct MatchResult {
    Qty filled{0};
    bool stopped_by_stp{false};  // CancelTaking hit the taker's own order
  };

  void do_submit(const NewOrder& req, EventBuffer& out);
  void do_cancel(const Cancel& req, EventBuffer& out);
  void do_modify(const Modify& req, EventBuffer& out);

  bool price_in_range(Price p) const { return p >= 1 && p <= cfg_.max_price; }
  bool would_cross(Side side, Price price) const;  // pure price test vs the opposite best
  Order* find(OrderId id) const { return id < index_.size() ? index_[id] : nullptr; }

  template <class Makers>
  void preview_in(const Makers& makers, Side taker_side, Price limit, Qty qty,
                  std::vector<Fill>& fills) const;

  bool fok_fillable(const Taker& t) const;
  template <class Makers>
  bool fok_fillable_in(const Makers& makers, const Taker& t) const;
  MatchResult match(Taker& t, EventBuffer& out);
  template <class Makers>
  MatchResult match_in(Makers& makers, Taker& t, EventBuffer& out);

  void rest(const Taker& t, bool post_only);
  template <class SideMap>
  void append(SideMap& side, Order* o);
  void unlink(Order* o) noexcept;   // out of its level's list; the level may become empty
  void release(Order* o) noexcept;  // clears its index slot and returns it to the pool
  void remove_resting(Order* o);    // unlink + release + erase the level if now empty
  void set_index(OrderId id, Order* o) noexcept;

  template <class SideMap>
  void validate_side(const SideMap& side, Side which, std::size_t& orders) const;

  BookConfig cfg_;
  Bids bids_;
  Asks asks_;
  ObjectPool<Order> pool_;
  std::vector<Order*> index_;  // by OrderId; nullptr unless that order is resting
  std::size_t indexed_{0};     // non-null entries of index_; written only by set_index()
  OrderId next_order_id_{1};
  std::uint64_t next_trade_id_{1};
};

}  // namespace lob
