#pragma once

#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <string>

#include "lob/types.hpp"

namespace sim {

// A malformed orders.log: `line` is the 1-based line number.
class ReplayError : public std::runtime_error {
 public:
  ReplayError(std::size_t line, const std::string& what)
      : std::runtime_error("line " + std::to_string(line) + ": " + what), line_(line) {}
  std::size_t line() const noexcept { return line_; }

 private:
  std::size_t line_;
};

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
