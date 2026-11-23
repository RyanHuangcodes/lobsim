#pragma once

// Shared check for the ITCH feed parser (upgrade 6): used by the libFuzzer target fuzz_feed_parse
// and by the corpus regression test.

#include <cstddef>
#include <cstdint>

namespace lob::fuzz {

// Treats the bytes as a feed file and checks:
//  - feed::parse either accepts the whole input, and then re-encoding every parsed message gives
//    exactly the input, or it stops at an error offset, and then the messages before that offset
//    re-encode to exactly the bytes before it;
//  - the messages that parsed, put into MoldUDP64 packets and read back, give exactly the same
//    bytes with no gap; the input read as a MoldUDP64 packet file is accepted or refused cleanly;
//  - the same bytes parsed into a FeedBook are accepted or refused message by message without a
//    crash, and the rebuilt book passes OrderBook::validate().
// A violated check prints a report to stderr and calls std::abort(). No output on success.
void run_feed_parse(const std::uint8_t* data, std::size_t size);

}  // namespace lob::fuzz
