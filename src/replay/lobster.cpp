#include "replay/lobster.hpp"

#include <charconv>
#include <limits>
#include <utility>

#include "parse_util.hpp"

// The book-side parsers are in lobster_book.cpp.
namespace replay {
namespace {

constexpr std::int64_t kNsPerSec = 1'000'000'000;

bool all_digits(std::string_view s) {
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// "34200.123456789012" -> 34200123456789 ns, truncated. Returns an error text, empty on success.
std::string parse_time(std::string_view s, std::int64_t& ns, bool& truncated) {
  const std::size_t dot = s.find('.');
  const std::string_view secs = s.substr(0, dot);
  if (secs.empty()) return "time: no digits before the decimal point";
  if (!all_digits(secs)) return "time: not a decimal number";
  std::string_view frac;
  if (dot != std::string_view::npos) {
    frac = s.substr(dot + 1);
    if (frac.empty()) return "time: decimal point without fraction digits";
    if (!all_digits(frac)) return "time: not a decimal number";
  }
  std::uint64_t sec = 0;
  const auto [p, ec] = std::from_chars(secs.data(), secs.data() + secs.size(), sec);
  if (ec != std::errc{} || p != secs.data() + secs.size()) return "time: seconds out of range";
  if (frac.size() > 9) {
    truncated = true;
    frac = frac.substr(0, 9);
  }
  std::int64_t f = 0;
  for (std::size_t i = 0; i < 9; ++i) f = f * 10 + (i < frac.size() ? frac[i] - '0' : 0);
  constexpr std::uint64_t kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (sec > kMax / kNsPerSec || sec * kNsPerSec > kMax - static_cast<std::uint64_t>(f)) {
    return "time: does not fit in 64-bit nanoseconds";
  }
  ns = static_cast<std::int64_t>(sec) * kNsPerSec + f;
  return {};
}

}  // namespace

using detail::fail;
using detail::split;
using detail::strip_cr;
using detail::to_int;

Parsed<LobsterMsg> parse_message_line(std::string_view line) {
  Parsed<LobsterMsg> r;
  line = strip_cr(line, r.cr_stripped);
  std::string_view f[6];
  const std::size_t n = split(line, f, 6);
  if (n != 6) return fail(std::move(r), "expected 6 fields, found " + std::to_string(n));

  LobsterMsg m{};
  if (std::string e = parse_time(f[0], m.time_ns, r.time_truncated); !e.empty()) return fail(std::move(r), e);

  const auto type = to_int(f[1]);
  if (!type) return fail(std::move(r), "type: not an integer");
  if (*type < 1 || *type > 7) return fail(std::move(r), "type: outside 1..7");
  m.type = static_cast<int>(*type);

  const auto id = to_int(f[2]);
  if (!id) return fail(std::move(r), "order id: not an integer");
  m.order_id = *id;
  const auto size = to_int(f[3]);
  if (!size) return fail(std::move(r), "size: not an integer");
  m.size = *size;
  const auto price = to_int(f[4]);
  if (!price) return fail(std::move(r), "price: not an integer");
  m.price = *price;
  const auto dir = to_int(f[5]);
  if (!dir) return fail(std::move(r), "direction: not an integer");

  const bool order_type = m.type <= 5;  // types 6 and 7 are skipped by the replayer: no further checks
  if (*dir < -1 || *dir > 1 || (order_type && *dir == 0)) {
    return fail(std::move(r), order_type ? "direction: must be -1 or 1" : "direction: must be -1, 0 or 1");
  }
  m.direction = static_cast<int>(*dir);
  if (order_type && m.size <= 0) return fail(std::move(r), "size: must be positive");
  if (m.type <= 4 && m.price <= 0) return fail(std::move(r), "price: must be positive");
  r.value = m;
  return r;
}

std::string format_message_line(const LobsterMsg& m) {
  const std::int64_t sec = m.time_ns / kNsPerSec;
  std::string frac = std::to_string(m.time_ns % kNsPerSec);
  frac.insert(0, 9 - frac.size(), '0');
  return std::to_string(sec) + '.' + frac + ',' + std::to_string(m.type) + ',' + std::to_string(m.order_id) + ',' +
         std::to_string(m.size) + ',' + std::to_string(m.price) + ',' + std::to_string(m.direction);
}

}  // namespace replay
