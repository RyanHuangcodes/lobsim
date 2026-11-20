#pragma once

// Small helpers shared by the LOBSTER message and orderbook parsers (not part of the public API).

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "replay/lobster.hpp"

namespace replay::detail {

// Whole field, optional '-', digits. No '+', no spaces, no other characters.
inline std::optional<std::int64_t> to_int(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::int64_t v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

inline std::string_view strip_cr(std::string_view line, bool& stripped) {
  stripped = !line.empty() && line.back() == '\r';
  if (stripped) line.remove_suffix(1);
  return line;
}

// Stores up to `max` fields in `out`; returns the total number of fields found.
inline std::size_t split(std::string_view line, std::string_view* out, std::size_t max) {
  std::size_t n = 0;
  std::size_t start = 0;
  for (;;) {
    const std::size_t comma = line.find(',', start);
    const std::size_t len = comma == std::string_view::npos ? std::string_view::npos : comma - start;
    if (n < max) out[n] = line.substr(start, len);
    ++n;
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return n;
}

template <class T>
Parsed<T> fail(Parsed<T> r, std::string why) {
  r.value.reset();
  r.error = std::move(why);
  return r;
}

}  // namespace replay::detail
