#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob {

// The deliberately naive book of SPEC 3.6: one vector of resting orders, the best
// match found by linear scan on (price, priority_seq). It defines correct behaviour.
class ReferenceBook {
 public:
  explicit ReferenceBook(BookConfig cfg = {});

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
  std::optional<OrderView> order(OrderId id) const;
  std::vector<OrderView> level_orders(Side side, Price price) const;
  std::size_t resting_orders() const;
  void validate() const;  // throws std::logic_error naming the first broken invariant

 private:
  struct Resting {
    OrderId id;
    OwnerId owner;
    Side side;
    Price price;
    Qty remaining;
    STP stp;
    bool post_only;
    std::uint64_t priority_seq;  // lower = earlier arrival
  };

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
  bool crosses(const Taker& t, Price maker_price) const;
  bool has_priority_over(const Resting& a, const Resting& b) const;  // same side only
  // Both return an index into orders_, or orders_.size() when there is none.
  std::size_t find_order(OrderId id) const;
  std::size_t find_best(Side side) const;
  bool fok_fillable(const Taker& t) const;
  MatchResult match(Taker& t, EventBuffer& out);
  void rest(const Taker& t, bool post_only);

  BookConfig cfg_;
  std::vector<Resting> orders_;
  OrderId next_order_id_{1};
  std::uint64_t next_trade_id_{1};
  std::uint64_t next_priority_seq_{1};
};

}  // namespace lob
