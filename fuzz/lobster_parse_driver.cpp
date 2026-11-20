#include "lobster_parse_driver.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "replay/lobster.hpp"

namespace lob::fuzz {
namespace {

[[noreturn]] void die(std::string_view what, std::string_view line) {
  std::fprintf(stderr, "run_lobster_parse: %.*s\n  line: \"%.*s\"\n", static_cast<int>(what.size()), what.data(),
               static_cast<int>(line.size()), line.data());
  std::fflush(stderr);
  std::abort();
}

void check_message(std::string_view line) {
  const replay::Parsed<replay::LobsterMsg> first = replay::parse_message_line(line);
  if (!first.value) return;
  const std::string text = replay::format_message_line(*first.value);
  const replay::Parsed<replay::LobsterMsg> second = replay::parse_message_line(text);
  if (!second.value) die("formatted message line was rejected: " + second.error + " (" + text + ")", line);
  if (!(*second.value == *first.value)) die("message round trip changed the value (" + text + ")", line);
  if (second.time_truncated) die("formatted message line reports a truncated time (" + text + ")", line);
}

void check_book(std::string_view line) {
  const int levels = replay::levels_from_columns(replay::count_fields(line)).value_or(10);
  const replay::Parsed<replay::BookRow> row = replay::parse_book_line(line, levels);
  if (!row.value) return;
  const auto want = static_cast<std::size_t>(levels);
  if (row.value->asks.size() != want || row.value->bids.size() != want) die("book row has the wrong number of levels", line);
}

}  // namespace

void run_lobster_parse(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t nl = text.find('\n', start);
    const std::string_view line =
        text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    check_message(line);
    check_book(line);
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
}

}  // namespace lob::fuzz
