// Lab 3: a tour of the C++ pieces lobsim is built from, using lobsim's own types.
//
//   course/labs/build_lab.sh course/labs/lab03_cpp_tour.cpp && build/release/labs/lab03_cpp_tour
//
// Read each numbered part, guess what it prints, then run it. Then do the "break it" exercises
// in course/03-cpp-toolkit.md: each one is a small edit that the compiler refuses.
#include <cstdio>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

int main() {
  // 1. Why prices are whole numbers of ticks: decimals in a computer are not exact.
  const double a = 0.1 + 0.2;
  std::printf("1. 0.1 + 0.2 == 0.3 ? %s (it is %.17f)\n", a == 0.3 ? "yes" : "no", a);
  const lob::Price p = 10 + 20;  // ticks: 10 ticks + 20 ticks is exactly 30 ticks
  std::printf("   10 + 20 ticks == 30 ticks ? %s\n", p == 30 ? "yes" : "no");

  // 2. A struct is a bundle of named fields. {} gives each its default value.
  lob::NewOrder order{};  // every field from types.hpp: owner 0, side Buy, type Limit, tif GTC, ...
  order.owner = 7;
  order.side = lob::Side::Sell;
  order.price = 10'001;  // the ' is only a digit separator, like a comma in 10,001
  order.qty = 5;
  std::printf("2. order: owner %u, side %s, type %s, tif %s, price %lld, qty %lld, post_only %d\n", order.owner,
              lob::to_string(order.side), lob::to_string(order.type), lob::to_string(order.tif),
              static_cast<long long>(order.price), static_cast<long long>(order.qty), order.post_only);

  // 3. enum class: a list of names. Underneath each is a small number (Side is a uint8_t).
  std::printf("3. Buy is stored as %d, Sell as %d; opposite(Sell) is %s\n", static_cast<int>(lob::Side::Buy),
              static_cast<int>(lob::Side::Sell), lob::to_string(lob::opposite(lob::Side::Sell)));

  // 4. operator== = default: the compiler writes "equal if every field is equal".
  lob::NewOrder copy = order;
  std::printf("4. copy == order ? %s;", copy == order ? "yes" : "no");
  copy.qty = 6;
  std::printf(" after changing qty: %s\n", copy == order ? "yes" : "no");

  // 5. std::variant: a box that holds exactly one of a few types. A Request is a NewOrder, a
  //    Cancel or a Modify, never two at once.
  lob::Request req = lob::Cancel{7, 42};
  std::printf("5. the request holds alternative number %zu (0 NewOrder, 1 Cancel, 2 Modify)\n", req.index());
  if (const auto* c = std::get_if<lob::Cancel>(&req)) std::printf("   it is a Cancel of order %llu\n",
                                                                  static_cast<unsigned long long>(c->order_id));

  // 6. std::visit runs the right code for whatever the variant holds.
  for (const lob::Request& r : {lob::Request{order}, lob::Request{lob::Cancel{7, 42}},
                                lob::Request{lob::Modify{7, 42, 10'000, 3}}}) {
    std::visit(
        [](const auto& x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, lob::NewOrder>) std::printf("6. a new order for %lld lots\n", static_cast<long long>(x.qty));
          else if constexpr (std::is_same_v<T, lob::Cancel>) std::printf("6. a cancel of order %llu\n", static_cast<unsigned long long>(x.order_id));
          else std::printf("6. a modify to %lld lots at %lld\n", static_cast<long long>(x.new_qty), static_cast<long long>(x.new_price));
        },
        r);
  }

  // 7. std::optional: "a value, or nothing". An empty book has no best bid, and the type says so.
  lob::OrderBook book;
  const std::optional<lob::Price> best = book.best_bid();
  std::printf("7. best bid of an empty book: %s\n", best ? std::to_string(*best).c_str() : "none (nullopt)");
  std::printf("   value_or(-1) gives %lld\n", static_cast<long long>(best.value_or(-1)));

  // 8. Putting it together: the engine answers into a caller-owned vector of Events.
  lob::EventBuffer out;
  book.submit(order, out);
  for (const lob::Event& e : out) std::printf("8. %s\n", lob::to_string(e).c_str());
  std::printf("   best ask is now %lld\n", static_cast<long long>(*book.best_ask()));
}
