#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lob/types.hpp"
#include "sim/messages.hpp"

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

// The one codec for orders.log: the exchange writes through it and the replay tool reads
// through it. Text, one request per line, decimal integers only (enums and bools as
// their integer values, never as characters):
//
//   # lobsim orders.log v1 max_price=<max_price>
//   <time_ns> N <owner> <client_order_id> <side> <type> <tif> <price> <qty> <post_only> <stp>
//   <time_ns> C <owner> <order_id>
//   <time_ns> M <owner> <order_id> <new_price> <new_qty>
std::string format_header(lob::Price max_price);
std::optional<lob::Price> parse_header(std::string_view line);

void append_request(std::string& out, TimeNs time, const lob::Request& request);  // no newline
std::string format_request(TimeNs time, const lob::Request& request);
std::optional<std::pair<TimeNs, lob::Request>> parse_request(std::string_view line);

struct LoggedRequest {
  TimeNs time;
  lob::Request request;
  bool operator==(const LoggedRequest&) const = default;
};

struct OrderLog {
  lob::Price max_price;
  std::vector<LoggedRequest> requests;
  bool operator==(const OrderLog&) const = default;
};

// Reads a whole log line by line. The constructor reads and checks the header; next() returns
// the next request, or nullopt at the end of the input. Both throw ReplayError on a missing or
// malformed header or request line, and next() also on a time earlier than the previous one
// (equal times are fine). The replay tool and parse_order_log read through this one class.
class OrderLogReader {
 public:
  explicit OrderLogReader(std::istream& in);
  lob::Price max_price() const { return max_price_; }
  std::optional<LoggedRequest> next();

 private:
  std::istream& in_;
  lob::Price max_price_{0};
  std::size_t line_no_{1};
  TimeNs previous_time_{0};
  std::string line_;
};

// Parses the full text of an orders.log; any byte sequence either parses or throws ReplayError.
OrderLog parse_order_log(std::string_view contents);
// The inverse: byte-identical to what the exchange writes (header, then one request per line,
// each ended by a newline).
std::string format_order_log(const OrderLog& log);

}  // namespace sim
