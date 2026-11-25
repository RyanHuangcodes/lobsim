// Lab 8.2: replay the start of an orders.log and print what the engine said to each request.
//
//   course/labs/build_lab.sh course/labs/lab08_trace_log.cpp
//   build/release/labs/lab08_trace_log out/run1/orders.log 40
//
// orders.log holds only requests (what arrived at the exchange, and when). Feeding them, in order,
// to a fresh lob::OrderBook recreates every event of the live run: this is what lobsim_replay does
// for the whole file. Here we stop after N requests and print the events, so you can read the
// first milliseconds of a run like a story.
#include <cstdio>
#include <fstream>
#include <string>

#include "lob/apply.hpp"
#include "lob/order_book.hpp"
#include "sim/exchange.hpp"
#include "sim/order_log.hpp"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <orders.log> <number of requests>\n", argv[0]);
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  const long n = std::stol(argv[2]);
  sim::OrderLogReader reader(in);  // reads and checks the header line
  lob::OrderBook book(lob::BookConfig{reader.max_price()});
  lob::EventBuffer events;
  for (long i = 0; i < n; ++i) {
    const auto logged = reader.next();
    if (!logged) break;
    events.clear();
    lob::apply(book, logged->request, events);
    std::printf("%s\n", sim::format_request(logged->time, logged->request).c_str());
    for (const lob::Event& e : events) std::printf("    %s\n", lob::to_string(e).c_str());
  }
  const sim::BBO top = sim::top_of_book(book);  // the same function the exchange uses for its BBO messages
  std::printf("book now: best bid %lld x %lld, best ask %lld x %lld, %zu resting orders\n",
              static_cast<long long>(top.bid), static_cast<long long>(top.bid_qty), static_cast<long long>(top.ask),
              static_cast<long long>(top.ask_qty), book.resting_orders());
}
