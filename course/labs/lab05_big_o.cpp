// Lab 5: why the fast book is fast. Big-O you can feel.
//
//   course/labs/build_lab.sh course/labs/lab05_big_o.cpp && build/release/labs/lab05_big_o
//
// Both books get the same job: a book of N resting orders, then 20,000 "add one order, cancel
// one order" steps. ReferenceBook finds things by looking at every order (linear scan, O(N)).
// OrderBook jumps straight to them (id index, O(1)) or walks a sorted tree (std::map, O(log L)).
// Watch how the time per step changes when N grows ten times.
#include <chrono>
#include <cstdio>
#include <vector>

#include "lob/apply.hpp"
#include "lob/order_book.hpp"
#include "lob/reference_book.hpp"

namespace {

using namespace lob;

// Nanoseconds per step for one book with `resting` orders already in it.
template <class Book>
double ns_per_step(int resting, int steps) {
  Book book(BookConfig{1'000'000});
  EventBuffer out;
  out.reserve(16);
  std::vector<OrderId> ids;
  // Bids at 1..500 below 500,000 and asks above it, so nothing ever trades.
  for (int i = 0; i < resting; ++i) {
    const bool buy = i % 2 == 0;
    const Price p = buy ? 500'000 - 1 - (i / 2) % 500 : 500'000 + 1 + (i / 2) % 500;
    out.clear();
    book.submit(NewOrder{1, 0, buy ? Side::Buy : Side::Sell, OrderType::Limit, TIF::GTC, p, 1, false,
                         STP::CancelTaking},
                out);
    ids.push_back(std::get<Ack>(out.front()).order_id);
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (int s = 0; s < steps; ++s) {
    out.clear();
    // Add one bid...
    book.submit(NewOrder{1, 0, Side::Buy, OrderType::Limit, TIF::GTC, 500'000 - 1 - s % 500, 1, false,
                         STP::CancelTaking},
                out);
    const OrderId fresh = std::get<Ack>(out.front()).order_id;
    // ...and cancel the oldest order, so the book keeps its size.
    book.cancel(Cancel{1, ids[static_cast<std::size_t>(s)]}, out);
    ids.push_back(fresh);
  }
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / steps;
}

}  // namespace

int main() {
  constexpr int kSteps = 20'000;
  std::printf("%10s  %22s  %22s\n", "resting N", "ReferenceBook ns/step", "OrderBook ns/step");
  for (int n : {100, 1'000, 10'000}) {
    std::printf("%10d  %22.0f  %22.0f\n", n, ns_per_step<ReferenceBook>(n, kSteps), ns_per_step<OrderBook>(n, kSteps));
  }
  std::printf("\nIf the reference column grows about 10x per row and the fast column hardly moves,\n"
              "you have measured O(N) against O(1) / O(log L).\n");
}
