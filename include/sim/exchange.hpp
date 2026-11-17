#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "sim/messages.hpp"

namespace sim {

// What one request produced. Valid until the next call to Exchange::process().
struct ExchangeOutput {
  lob::EventBuffer events;              // engine events, in engine order
  std::vector<MarketData> public_data;  // one TradePrint per trade in trade order, then at most one BBO
  BBO top;                              // true top of book after the request
  bool top_changed{false};              // any of the four top-of-book fields differs from before
};

// Returns `request` with its owner replaced: an agent cannot act for another owner.
lob::Request with_owner(lob::Request request, lob::OwnerId owner);

BBO top_of_book(const lob::OrderBook& book);

// Wraps one OrderBook (SPEC 5.4). A request is processed instantly: it is logged, applied
// to the book, every engine event is added to the event hash, and the public data it
// causes is worked out. Delivering events to agents, with latency, is the simulator's job.
class Exchange {
 public:
  // `order_log` may be null. If set, the header line is written now and every request
  // that arrives is appended as one line (the replay tool reads this file).
  Exchange(lob::Price max_price, std::ostream* order_log);

  const ExchangeOutput& process(TimeNs now, lob::OwnerId sender, const lob::Request& request);

  const lob::OrderBook& book() const noexcept { return book_; }
  BBO top() const noexcept { return top_; }
  // The mid (bid + ask) / 2 the last time both sides were non-empty.
  std::optional<double> last_mid() const noexcept { return last_mid_; }
  std::uint64_t event_hash() const noexcept { return hasher_.value(); }
  std::uint64_t requests() const noexcept { return requests_; }
  std::uint64_t events() const noexcept { return events_; }
  std::uint64_t trades() const noexcept { return trades_; }
  lob::Price max_price() const noexcept { return max_price_; }

 private:
  lob::Price max_price_;
  lob::OrderBook book_;
  std::ostream* log_;
  std::string line_;
  lob::EventHasher hasher_;
  ExchangeOutput out_;
  BBO top_{};
  std::optional<double> last_mid_;
  std::uint64_t requests_{0};
  std::uint64_t events_{0};
  std::uint64_t trades_{0};
};

}  // namespace sim
