// Lab 2: the order book playground.
//
// Type orders, watch the real lobsim matching engine (lob::OrderBook) answer.
//
//   course/labs/build_lab.sh course/labs/lab02_book_repl.cpp
//   build/release/labs/lab02_book_repl                         (type commands, "help" lists them)
//   build/release/labs/lab02_book_repl < course/labs/scripts/worked_example.txt
//
// Every line you type becomes one lob::Request (NewOrder, Cancel or Modify). The engine
// appends its answer to an EventBuffer, and this program prints each event twice: once as the
// engine names it (lob::to_string) and once in plain words.
#include <unistd.h>  // isatty

#include <cstdint>
#include <cstdio>  // fileno
#include <iostream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "lob/reference_book.hpp"

namespace {

using namespace lob;

// Which engine the playground uses. Lab 4.3 changes this one line to lob::ReferenceBook: both
// books have the same functions, so nothing else needs to change.
using Book = lob::OrderBook;

void help() {
  std::cout << "commands (prices in ticks, sizes in lots):\n"
               "  buy 5 @ 101            limit order, good till cancelled (GTC)\n"
               "  sell 3 @ 102 ioc       limit, immediate-or-cancel\n"
               "  buy 10 @ 101 fok       limit, fill-or-kill\n"
               "  sell 4 @ 103 post      post-only (rejected if it would trade on arrival)\n"
               "  buy 4 market           market order (always IOC)\n"
               "  ... resting            add to any order: self-trade mode CancelResting\n"
               "  cancel 3               cancel order #3\n"
               "  modify 3 101 2         order #3 to price 101, remaining qty 2\n"
               "  as 2                   send the next orders as trader (owner) 2\n"
               "  book                   show the book, every order in queue order\n"
               "  hash                   the event hash of everything so far\n"
               "  # anything             a comment (ignored)\n"
               "  help, quit\n";
}

std::string plain(const Event& ev) {
  struct V {
    std::string operator()(const Ack& e) const {
      return "accepted: it is order #" + std::to_string(e.order_id) + " (trader " + std::to_string(e.owner) + ")";
    }
    std::string operator()(const Reject& e) const {
      return std::string("refused, reason ") + to_string(e.reason) + ": nothing changed";
    }
    std::string operator()(const Trade& e) const {
      return std::to_string(e.qty) + " lots change hands at " + std::to_string(e.price) + " (the resting order #" +
             std::to_string(e.maker_order_id) + "'s price); the " + (e.aggressor_side == Side::Buy ? "buyer" : "seller") +
             " #" + std::to_string(e.taker_order_id) + " was the aggressor";
    }
    std::string operator()(const Cancelled& e) const {
      return "order #" + std::to_string(e.order_id) + " is gone, " + std::to_string(e.qty_cancelled) +
             " lots unfilled, reason " + to_string(e.reason);
    }
    std::string operator()(const Modified& e) const {
      return "order #" + std::to_string(e.order_id) + " is now " + std::to_string(e.new_qty) + " @ " +
             std::to_string(e.new_price) + (e.priority_kept ? ", kept its place in line" : ", went to the back of the line");
    }
  };
  return std::visit(V{}, ev);
}

void show_book(const Book& book) {
  const std::vector<LevelView> asks = book.depth(Side::Sell, 8);
  const std::vector<LevelView> bids = book.depth(Side::Buy, 8);
  auto print_level = [&](Side side, const LevelView& l) {
    std::cout << "  " << l.price << " | " << l.total_qty << (l.total_qty == 1 ? " lot    " : " lots   ");
    for (const OrderView& o : book.level_orders(side, l.price))
      std::cout << "[#" << o.id << " trader " << o.owner << ": " << o.remaining << "] ";
    std::cout << "\n";
  };
  std::cout << "  ASKS (sellers), the lowest ask is next to the line\n";
  for (auto it = asks.rbegin(); it != asks.rend(); ++it) print_level(Side::Sell, *it);
  if (!asks.empty() && !bids.empty()) {
    const Price spread = asks.front().price - bids.front().price;
    std::cout << "  ------ spread " << spread << (spread == 1 ? " tick" : " ticks") << ", mid "
              << static_cast<double>(asks.front().price + bids.front().price) / 2.0 << "\n";
  } else {
    std::cout << "  ------ (one side is empty: no spread, no mid)\n";
  }
  for (const LevelView& l : bids) print_level(Side::Buy, l);
  std::cout << "  BIDS (buyers), the highest bid is next to the line\n";
}

}  // namespace

int main() {
  Book book(BookConfig{1'000'000});
  EventHasher hasher;
  OwnerId owner = 1;
  std::uint64_t client_id = 1;
  std::string line;
  // When the commands come from a file (`< script.txt`), echo them so the output reads like a transcript.
  const bool echo = isatty(fileno(stdin)) == 0;
  std::cout << "lobsim order book playground. Type help for the commands.\n";
  while (std::cout << "trader " << owner << "> " << std::flush, std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    if (echo) std::cout << line << "\n";
    if (!(in >> cmd) || cmd[0] == '#') continue;
    if (cmd == "quit" || cmd == "exit") break;
    if (cmd == "help") {
      help();
      continue;
    }
    if (cmd == "book") {
      show_book(book);
      continue;
    }
    if (cmd == "hash") {
      std::cout << "  event hash so far: " << std::hex << hasher.value() << std::dec << "\n";
      continue;
    }
    if (cmd == "as") {
      if (!(in >> owner) || owner == 0) {
        std::cout << "  usage: as <trader number 1 or more>\n";
        owner = 1;
      }
      continue;
    }

    Request req;
    if (cmd == "buy" || cmd == "sell") {
      NewOrder o;
      o.owner = owner;
      o.client_order_id = client_id++;
      o.side = cmd == "buy" ? Side::Buy : Side::Sell;
      std::string word;
      if (!(in >> o.qty)) {
        std::cout << "  usage: buy <qty> @ <price> [ioc|fok|post] [resting], or buy <qty> market\n";
        continue;
      }
      in >> word;
      if (word == "market") {
        o.type = OrderType::Market;
        o.tif = TIF::IOC;
      } else if (word == "@" && (in >> o.price)) {
        o.type = OrderType::Limit;
        o.tif = TIF::GTC;
      } else {
        std::cout << "  usage: buy <qty> @ <price> [ioc|fok|post] [resting], or buy <qty> market\n";
        continue;
      }
      while (in >> word) {
        if (word == "ioc") o.tif = TIF::IOC;
        else if (word == "fok") o.tif = TIF::FOK;
        else if (word == "gtc") o.tif = TIF::GTC;
        else if (word == "post") o.post_only = true;
        else if (word == "resting") o.stp = STP::CancelResting;
        else std::cout << "  (ignored unknown option " << word << ")\n";
      }
      req = o;
    } else if (cmd == "cancel") {
      Cancel c;
      c.owner = owner;
      if (!(in >> c.order_id)) {
        std::cout << "  usage: cancel <order id>\n";
        continue;
      }
      req = c;
    } else if (cmd == "modify") {
      Modify m;
      m.owner = owner;
      if (!(in >> m.order_id >> m.new_price >> m.new_qty)) {
        std::cout << "  usage: modify <order id> <new price> <new remaining qty>\n";
        continue;
      }
      req = m;
    } else {
      std::cout << "  unknown command, type help\n";
      continue;
    }

    EventBuffer events;
    apply(book, req, events);  // the same function the exchange, the replay and the fuzzer use
    for (const Event& ev : events) {
      hasher.add(ev);
      std::cout << "  " << to_string(ev) << "\n      = " << plain(ev) << "\n";
    }
  }
  std::cout << "\n";
  return 0;
}
