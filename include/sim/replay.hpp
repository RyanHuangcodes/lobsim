#pragma once

#include <cstdint>
#include <iosfwd>

#include "lob/types.hpp"
#include "sim/order_log.hpp"

namespace sim {

struct ReplayResult {
  lob::Price max_price{0};
  std::uint64_t requests{0};
  std::uint64_t events{0};
  std::uint64_t trades{0};
  std::uint64_t event_hash{0};
};

// Reads an orders.log (SPEC 5.7) with the one codec of order_log.hpp and feeds the requests,
// in order, into a fresh lob::OrderBook built with the logged max_price. Throws ReplayError
// on a missing or malformed header or request line, or a request whose time is earlier than
// the previous one (equal times are fine). The tool and the tests share this.
ReplayResult replay_log(std::istream& in);

}  // namespace sim
