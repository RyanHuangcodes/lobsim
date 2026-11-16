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
    for (const Event& ev : events) apply_event(req, ev, bad);

    for (const auto& [id, rem] : remaining_) {
      const std::string who = "order " + std::to_string(id) + ": ";
      std::optional<OrderView> view = book_order(id);
      if (rem < 0) {
        bad.push_back(who + "modelled remaining is negative (" + std::to_string(rem) + ")");
      } else if (rem == 0) {
        if (view) bad.push_back(who + "model says finished but the book still holds it");
      } else if (!view) {
        bad.push_back(who + "model says " + std::to_string(rem) + " remaining but the book has no such order");
      } else if (view->remaining != rem) {
        bad.push_back(who + "model says " + std::to_string(rem) + " remaining, book says " +
                      std::to_string(view->remaining));
      }
    }
    return bad;
  }

 private:
  void apply_event(const Request& req, const Event& ev, std::vector<std::string>& bad) {
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, Ack>) {
            if (const auto* n = std::get_if<NewOrder>(&req)) remaining_[e.order_id] = n->qty;
            else bad.push_back("Ack for a request that is not a NewOrder");
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
            }
          } else if constexpr (std::is_same_v<T, Modified>) {
            auto it = remaining_.find(e.order_id);
            if (it == remaining_.end()) bad.push_back("Modified for unknown order " + std::to_string(e.order_id));
            else it->second = e.new_qty;
          }
        },
        ev);
  }

  void take(OrderId id, Qty qty, std::vector<std::string>& bad) {
    auto it = remaining_.find(id);
    if (it == remaining_.end()) bad.push_back("Trade against unknown order " + std::to_string(id));
    else it->second -= qty;
  }

  std::map<OrderId, Qty> remaining_;
};

}  // namespace lob::test
