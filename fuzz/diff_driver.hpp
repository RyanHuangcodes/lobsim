#pragma once

// Shared differential driver (SPEC upgrade 1, section 3): decodes a byte string into
// requests and feeds them to a fresh OrderBook and a fresh ReferenceBook. Used by the
// libFuzzer target, the regression test and lobsim_fuzz_decode.

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
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

// The book-builder fuzz target (fuzz_diff_builder): the same 8-byte records, with
// kind = b0 % 7. Kinds 0-2 decode exactly as above; 3-6 are the builder operations.
struct InsertPassive {
  Side side{Side::Buy};
  Price price{};
  Qty qty{};
  OwnerId owner{};
  bool operator==(const InsertPassive&) const = default;
};
struct Reduce {
  OrderId order_id{};
  Qty qty{};
  bool operator==(const Reduce&) const = default;
};
struct Remove {
  OrderId order_id{};
  bool operator==(const Remove&) const = default;
};
struct PreviewMatch {
  Side taker_side{Side::Buy};
  Price limit{};
  Qty qty{};
  bool operator==(const PreviewMatch&) const = default;
};
using BuilderOp = std::variant<NewOrder, Cancel, Modify, InsertPassive, Reduce, Remove, PreviewMatch>;

// `known_ids` are the OrderIds handed out so far: every Ack and every insert_passive result.
BuilderOp decode_builder_record(const std::uint8_t* rec, std::uint64_t index,
                                const std::vector<OrderId>& known_ids);

// Like decode(), for builder inputs. If `reference_outcomes` is non-null it receives, per
// operation, the text of what the private ReferenceBook returned (events, status, fills).
std::vector<BuilderOp> decode_builder(const std::uint8_t* data, std::size_t size,
                                      std::vector<std::vector<std::string>>* reference_outcomes = nullptr);

// Runs builder data through a fresh OrderBook and a fresh ReferenceBook. Returns values, event
// buffers, depth, resting_orders, order(target) and level_orders of the touched level must all
// agree; otherwise prints a report to stderr and calls std::abort(). Never throws.
void run_diff_builder(const std::uint8_t* data, std::size_t size);

std::string op_to_string(const BuilderOp& op);

// One line, e.g. "Modify owner=2 id=1 price=97 qty=4" (callers prefix "#<index> ").
std::string to_string(const Request& r);

}  // namespace lob::fuzz
