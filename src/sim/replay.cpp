#include "sim/replay.hpp"

#include <istream>
#include <string>
#include <variant>

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "sim/order_log.hpp"

namespace sim {

ReplayResult replay_log(std::istream& in) {
  std::string line;
  if (!std::getline(in, line)) throw ReplayError(1, "empty log: the header line is missing");
  const auto max_price = parse_header(line);
  if (!max_price) throw ReplayError(1, "malformed header: " + line);

  lob::OrderBook book(lob::BookConfig{*max_price});
  lob::EventHasher hasher;
  lob::EventBuffer events;
  ReplayResult result;
  result.max_price = *max_price;
  TimeNs previous_time = 0;
  for (std::size_t n = 2; std::getline(in, line); ++n) {
    const auto parsed = parse_request(line);
    if (!parsed) throw ReplayError(n, "malformed request: " + line);
    if (parsed->first < previous_time)
      throw ReplayError(n, "time goes backwards: " + std::to_string(parsed->first) + " after " +
                               std::to_string(previous_time));
    previous_time = parsed->first;
    events.clear();
    lob::apply(book, parsed->second, events);
    hasher.add(events);
    result.events += events.size();
    for (const lob::Event& e : events)
      if (std::holds_alternative<lob::Trade>(e)) ++result.trades;
    ++result.requests;
  }
  result.event_hash = hasher.value();
  return result;
}

}  // namespace sim
