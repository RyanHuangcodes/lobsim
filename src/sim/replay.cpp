#include "sim/replay.hpp"

#include <istream>
#include <variant>

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "sim/order_log.hpp"

namespace sim {

ReplayResult replay_log(std::istream& in) {
  OrderLogReader reader(in);
  lob::OrderBook book(lob::BookConfig{reader.max_price()});
  lob::EventHasher hasher;
  lob::EventBuffer events;
  ReplayResult result;
  result.max_price = reader.max_price();
  while (const auto logged = reader.next()) {
    events.clear();
    lob::apply(book, logged->request, events);
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
