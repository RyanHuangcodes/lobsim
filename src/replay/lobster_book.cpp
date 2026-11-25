#include <charconv>
#include <limits>
#include <utility>

#include "parse_util.hpp"
#include "replay/lobster.hpp"

namespace replay {

using detail::fail;
using detail::strip_cr;
using detail::to_int;

Parsed<BookRow> parse_book_line(std::string_view line, int levels) {
  Parsed<BookRow> r;
  line = strip_cr(line, r.cr_stripped);
  if (levels < 1) return fail(std::move(r), "levels: must be at least 1");
  const std::size_t want = 4 * static_cast<std::size_t>(levels);
  const std::size_t have = count_fields(line);
  if (have != want) {
    return fail(std::move(r), "expected " + std::to_string(want) + " fields, found " + std::to_string(have));
  }
  // The fields in line order (exactly `want` of them, counted above): level by level, the ask
  // price, ask size, bid price and bid size, the same order the errors below are checked in.
  std::size_t start = 0;
  const auto next_field = [&]() {
    const std::size_t comma = line.find(',', start);
    const std::string_view f = line.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    start = comma == std::string_view::npos ? line.size() : comma + 1;
    return f;
  };
  static constexpr const char* kName[4] = {"ask price", "ask size", "bid price", "bid size"};
  BookRow row;
  row.asks.reserve(static_cast<std::size_t>(levels));
  row.bids.reserve(static_cast<std::size_t>(levels));
  for (std::size_t lvl = 0; lvl < static_cast<std::size_t>(levels); ++lvl) {
    std::int64_t v[4];
    for (std::size_t k = 0; k < 4; ++k) {
      const auto x = to_int(next_field());
      if (!x) {
        return fail(std::move(r), std::string(kName[k]) + " of level " + std::to_string(lvl + 1) + ": not an integer");
      }
      v[k] = *x;
    }
    if (v[1] < 0) return fail(std::move(r), "ask size of level " + std::to_string(lvl + 1) + ": negative");
    if (v[3] < 0) return fail(std::move(r), "bid size of level " + std::to_string(lvl + 1) + ": negative");
    row.asks.push_back({v[0], v[1]});
    row.bids.push_back({v[2], v[3]});
  }
  const auto ordered = [](const std::vector<BookLevel>& side, bool ascending) {
    bool seen_empty = false;
    const BookLevel* prev = nullptr;
    for (const BookLevel& l : side) {
      if (l.empty()) {
        seen_empty = true;
        continue;
      }
      if (seen_empty) return false;
      if (prev != nullptr && (ascending ? l.price <= prev->price : l.price >= prev->price)) return false;
      prev = &l;
    }
    return true;
  };
  r.oracle_malformed = !ordered(row.asks, true) || !ordered(row.bids, false);
  r.value = std::move(row);
  return r;
}

std::optional<int> levels_from_filename(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  constexpr std::string_view ext = ".csv";
  if (name.size() <= ext.size() || name.substr(name.size() - ext.size()) != ext) return std::nullopt;
  name.remove_suffix(ext.size());
  std::size_t d = name.size();
  while (d > 0 && name[d - 1] >= '0' && name[d - 1] <= '9') --d;
  const std::string_view digits = name.substr(d);
  if (digits.empty()) return std::nullopt;
  const std::string_view stem = name.substr(0, d);
  bool kind_ok = false;
  for (std::string_view kind : {std::string_view("_orderbook_"), std::string_view("_message_")}) {
    if (stem.size() >= kind.size() && stem.substr(stem.size() - kind.size()) == kind) kind_ok = true;
  }
  if (!kind_ok) return std::nullopt;
  int levels = 0;
  const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), levels);
  if (ec != std::errc{} || p != digits.data() + digits.size() || levels < 1) return std::nullopt;
  return levels;
}

std::optional<LobsterFileName> parse_lobster_filename(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  constexpr std::string_view ext = ".csv";
  if (name.size() <= ext.size() || name.substr(name.size() - ext.size()) != ext) return std::nullopt;
  name.remove_suffix(ext.size());
  // From the right: L, KIND, END, START, DATE; what is left is the ticker.
  std::string_view parts[5];
  for (std::string_view& part : parts) {
    const std::size_t us = name.rfind('_');
    if (us == std::string_view::npos) return std::nullopt;
    part = name.substr(us + 1);
    name = name.substr(0, us);
  }
  if (name.empty()) return std::nullopt;
  const auto digits = [](std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
      if (c < '0' || c > '9') return false;
    }
    return true;
  };
  const std::string_view l = parts[0], kind = parts[1], end = parts[2], start = parts[3], date = parts[4];
  if (kind != "message" && kind != "orderbook") return std::nullopt;
  if (!digits(l) || !digits(end) || !digits(start)) return std::nullopt;
  if (date.size() != 10 || date[4] != '-' || date[7] != '-' || !digits(date.substr(0, 4)) ||
      !digits(date.substr(5, 2)) || !digits(date.substr(8, 2))) {
    return std::nullopt;
  }
  LobsterFileName f;
  f.ticker = std::string(name);
  f.date = std::string(date);
  f.kind = std::string(kind);
  const auto [p1, e1] = std::from_chars(start.data(), start.data() + start.size(), f.start_ms);
  const auto [p2, e2] = std::from_chars(end.data(), end.data() + end.size(), f.end_ms);
  const auto [p3, e3] = std::from_chars(l.data(), l.data() + l.size(), f.levels);
  if (e1 != std::errc{} || e2 != std::errc{} || e3 != std::errc{} || f.levels < 1) return std::nullopt;
  return f;
}

std::size_t count_fields(std::string_view line) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  std::size_t n = 1;
  for (char c : line) n += (c == ',');
  return n;
}

std::optional<int> levels_from_columns(std::size_t fields) {
  if (fields == 0 || fields % 4 != 0 || fields / 4 > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  return static_cast<int>(fields / 4);
}

}  // namespace replay
