#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob::test {

// SPEC 3.8 quantity conservation. Keeps its own model of every order's remaining
// quantity, driven only by requests and events, and compares it with the book after
// every message:
//   Ack          remaining = the request's qty
//   Trade        remaining -= qty for maker and taker
//   Cancelled    qty_cancelled must equal remaining; remaining = 0
//   Modified     remaining = new_qty
// After each message, for every known id: remaining >= 0; remaining == 0 means the book
// has no such resting order; remaining > 0 means the book reports exactly that.
class ConservationChecker {
 public:
  using Lookup = std::function<std::optional<OrderView>(OrderId)>;

  // Returns one string per violation found (empty = conserved).
  std::vector<std::string> check(const Request& req, const EventBuffer& events,
                                 const Lookup& book_order) {
    std::vector<std::string> bad;
    touched_.clear();
    for (const Event& ev : events) apply_event(req, ev, bad);

    for (const auto& [id, rem] : remaining_) check_one(id, rem, book_order, bad);
    return bad;
  }

  // The same model and the same pass/fail verdict as check(), in O(open + touched) per
  // message instead of O(every id ever seen), for streams of millions of orders.
  // `book_resting` is the book's resting_orders(), which must equal the number of ids for
  // which `book_order` returns a value (validate() guarantees this for both books).
  //   - ids touched by this message get exactly check()'s per-id checks;
  //   - untouched ids whose modelled remaining is non-zero are checked against the book;
  //   - untouched finished ids are covered by a count: every modelled-live id was found
  //     resting, so if the book also held a finished (or unknown) id its resting count
  //     would exceed the number of modelled-live ids.
  std::vector<std::string> check_fast(const Request& req, const EventBuffer& events,
                                      const Lookup& book_order, std::size_t book_resting) {
    std::vector<std::string> bad;
    touched_.clear();
    for (const Event& ev : events) apply_event(req, ev, bad);

    for (OrderId id : touched_)
      if (remaining_[id] == 0) check_one(id, 0, book_order, bad);
    std::size_t live = 0;
    for (const auto& [id, rem] : open_) {
      if (rem > 0) ++live;
      check_one(id, rem, book_order, bad);
    }
    if (book_resting != live)
      bad.push_back("book reports " + std::to_string(book_resting) + " resting orders but the model has " +
                    std::to_string(live) + " live");
    return bad;
  }

 private:
  static void check_one(OrderId id, Qty rem, const Lookup& book_order, std::vector<std::string>& bad) {
    auto who = [id] { return "order " + std::to_string(id) + ": "; };  // built only on failure
    std::optional<OrderView> view = book_order(id);
    if (rem < 0) {
      bad.push_back(who() + "modelled remaining is negative (" + std::to_string(rem) + ")");
    } else if (rem == 0) {
      if (view) bad.push_back(who() + "model says finished but the book still holds it");
    } else if (!view) {
      bad.push_back(who() + "model says " + std::to_string(rem) + " remaining but the book has no such order");
    } else if (view->remaining != rem) {
      bad.push_back(who() + "model says " + std::to_string(rem) + " remaining, book says " +
                    std::to_string(view->remaining));
    }
  }

  // Records a change of `id`'s modelled remaining for check_fast().
  void changed(OrderId id, Qty rem) {
    touched_.push_back(id);
    if (rem != 0) open_[id] = rem;
    else open_.erase(id);
  }

  void apply_event(const Request& req, const Event& ev, std::vector<std::string>& bad) {
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, Ack>) {
            if (const auto* n = std::get_if<NewOrder>(&req)) {
              remaining_[e.order_id] = n->qty;
              changed(e.order_id, n->qty);
            } else {
              bad.push_back("Ack for a request that is not a NewOrder");
            }
          } else if constexpr (std::is_same_v<T, Trade>) {
            take(e.maker_order_id, e.qty, bad);
            take(e.taker_order_id, e.qty, bad);
          } else if constexpr (std::is_same_v<T, Cancelled>) {
            auto it = remaining_.find(e.order_id);
            if (it == remaining_.end()) {
              bad.push_back("Cancelled for unknown order " + std::to_string(e.order_id));
            } else {
              if (e.qty_cancelled != it->second)
                bad.push_back("order " + std::to_string(e.order_id) + ": Cancelled qty " +
                              std::to_string(e.qty_cancelled) + " but modelled remaining is " +
                              std::to_string(it->second));
              it->second = 0;
              changed(e.order_id, 0);
            }
          } else if constexpr (std::is_same_v<T, Modified>) {
            auto it = remaining_.find(e.order_id);
            if (it == remaining_.end()) bad.push_back("Modified for unknown order " + std::to_string(e.order_id));
            else {
              it->second = e.new_qty;
              changed(e.order_id, e.new_qty);
            }
          }
        },
        ev);
  }

  void take(OrderId id, Qty qty, std::vector<std::string>& bad) {
    auto it = remaining_.find(id);
    if (it == remaining_.end()) bad.push_back("Trade against unknown order " + std::to_string(id));
    else {
      it->second -= qty;
      changed(id, it->second);
    }
  }

  std::map<OrderId, Qty> remaining_;
  std::map<OrderId, Qty> open_;    // the entries of remaining_ that are not 0
  std::vector<OrderId> touched_;   // ids changed by the current message
};

}  // namespace lob::test
