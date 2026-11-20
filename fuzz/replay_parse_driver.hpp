#pragma once

// Shared check for the orders.log parser (upgrade 1, F5): used by the libFuzzer target and the
// corpus regression test.

#include <cstddef>
#include <cstdint>

namespace lob::fuzz {

// Treats the bytes as the contents of an orders.log.
//  - sim::ReplayError from the first parse is a clean rejection: return. Anything else the
//    parser throws (bad_alloc, length_error, logic_error, ...) is not caught and is a crash.
//  - A log that parses is formatted and parsed again: the second parse must not throw, must
//    equal the first, and formatting it must give the same text. Otherwise print a report to
//    stderr and std::abort().
// No output on success, no global state.
void run_replay_parse(const std::uint8_t* data, std::size_t size);

}  // namespace lob::fuzz
