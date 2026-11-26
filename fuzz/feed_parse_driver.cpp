#include "feed_parse_driver.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

#include "feed/decoder.hpp"
#include "feed/feed_book.hpp"
#include "feed/moldudp64.hpp"

namespace lob::fuzz {
namespace {

// References index a vector in FeedBook; a small limit keeps one input from allocating 512 MiB.
constexpr std::uint64_t kFuzzMaxRef = std::uint64_t{1} << 16;

[[noreturn]] void die(std::string_view what, std::size_t offset) {
  std::fprintf(stderr, "run_feed_parse: %.*s (byte offset %zu)\n", static_cast<int>(what.size()), what.data(), offset);
  std::fflush(stderr);
  std::abort();
}

}  // namespace

void run_feed_parse(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> in(reinterpret_cast<const std::byte*>(data), size);

  feed::Reencoder re;
  const feed::ParseResult r = feed::parse(in, re);
  if (r.error == feed::ParseError::Stopped) die("the re-encoder never stops, but the parse says it did", r.offset);
  if (r.offset > size) die("error offset beyond the input", r.offset);
  // Accepted: everything round-trips. Rejected: everything before the failing message does.
  const std::size_t good = r.ok() ? size : r.offset;
  if (r.ok() && r.offset != size) die("accepted input but the offset is not the input size", r.offset);
  if (re.out.size() != good || !std::equal(re.out.begin(), re.out.end(), in.begin())) {
    die(r.ok() ? "parse then re-encode is not byte-identical" : "the messages before the error do not round-trip",
        good);
  }

  // MoldUDP64 (F7): the part that parsed goes into packets and back unchanged, with no gap, at a
  // packet size picked from the input; and the input read as a packet file never crashes.
  const std::span<const std::byte> parsed = in.first(good);
  std::size_t largest = 0;  // the largest message block, so the packet size limit fits it
  for (std::size_t at = 0; at < parsed.size();) {
    const std::size_t block = feed::kPrefix + feed::be::load<std::uint16_t>(parsed.data() + at);
    largest = std::max(largest, block);
    at += block;
  }
  if (feed::mold::kHeader + largest <= 0xFFFF) {  // a longer message cannot fit in any packet
    feed::mold::PacketizeOptions mopt;
    mopt.max_packet = std::max<std::size_t>(60 + (size > 0 ? data[0] : 0) * 8, feed::mold::kHeader + largest);
    const std::vector<std::byte> packets = feed::mold::packetize(parsed, mopt);
    feed::mold::Reassembler back;
    const feed::mold::ReadResult mr = feed::mold::read_packets(packets, back);
    if (!mr.complete() || back.feed.size() != parsed.size() ||
        !std::equal(back.feed.begin(), back.feed.end(), parsed.begin())) {
      die("feed -> MoldUDP64 packets -> feed is not byte-identical", good);
    }
  }
  feed::mold::Reassembler any;
  const feed::mold::ReadResult ar = feed::mold::read_packets(in, any);
  if (ar.offset > size) die("MoldUDP64 error offset beyond the input", ar.offset);

  feed::FeedBook book(0, kFuzzMaxRef);
  const feed::ParseResult b = feed::parse(in, book);
  if (b.error == feed::ParseError::Stopped && !book.failed()) die("the book stopped the parse without an error", b.offset);
  if (b.ok()) static_cast<void>(book.finish());
  try {
    book.book().validate();
  } catch (const std::exception& e) {
    die(std::string_view(e.what()), b.offset);
  }
}

}  // namespace lob::fuzz
