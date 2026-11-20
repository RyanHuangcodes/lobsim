#pragma once

// Shared check for the LOBSTER line parsers (upgrade 2, R2): used by the libFuzzer target and the
// corpus regression test.

#include <cstddef>
#include <cstdint>

namespace lob::fuzz {

// Splits the bytes on '\n' and feeds every line to both parsers. Rejection is clean and silent;
// a crash, hang or sanitizer report is a failure.
//  - parse_message_line: a line that parses is formatted and parsed again; the second parse must
//    succeed, be equal to the first and not be truncated.
//  - parse_book_line, with L = columns / 4 when the line has a positive multiple of 4 columns, else
//    10: a row that parses must have L asks and L bids.
// A violated check prints a report to stderr and calls std::abort(). No output on success.
void run_lobster_parse(const std::uint8_t* data, std::size_t size);

}  // namespace lob::fuzz
