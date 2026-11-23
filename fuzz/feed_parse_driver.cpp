#include "feed_parse_driver.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <string_view>

#include "feed/decoder.hpp"
#include "feed/feed_book.hpp"

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
