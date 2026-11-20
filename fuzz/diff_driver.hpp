#pragma once

// Shared differential driver (SPEC upgrade 1, section 3): decodes a byte string into
// requests and feeds them to a fresh OrderBook and a fresh ReferenceBook. Used by the
// libFuzzer target, the regression test and lobsim_fuzz_decode.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob::fuzz {

inline constexpr Price kFuzzMaxPrice = 1000;  // the books' max_price while fuzzing (3.1)
inline constexpr std::size_t kRecordSize = 8;

// Decodes one 8-byte record (3.1). `index` is the record index (= client_order_id);
// `known_ids` are the OrderIds of every Ack so far, in order.
Request decode_record(const std::uint8_t* rec, std::uint64_t index,
                      const std::vector<OrderId>& known_ids);

// Every whole record of data; trailing bytes (< 8) are ignored. Cancel/Modify targets are
// resolved against the Acks of a private, fresh ReferenceBook. Deterministic, no shared
// state. If `reference_events` is non-null it receives that book's events, one buffer per
// request.
std::vector<Request> decode(const std::uint8_t* data, std::size_t size,
                            std::vector<EventBuffer>* reference_events = nullptr);

// Runs data through a fresh OrderBook and a fresh ReferenceBook. Returns if they agree;
// otherwise prints a report to stderr and calls std::abort(). Never throws.
void run_diff(const std::uint8_t* data, std::size_t size);

// One line, e.g. "Modify owner=2 id=1 price=97 qty=4" (callers prefix "#<index> ").
std::string to_string(const Request& r);

}  // namespace lob::fuzz
