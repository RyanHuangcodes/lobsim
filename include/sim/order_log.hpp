#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "lob/types.hpp"
#include "sim/messages.hpp"

namespace sim {

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

}  // namespace sim
