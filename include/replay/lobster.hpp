#pragma once

// Parsers for the two CSV files LOBSTER delivers per stock and day: the message file (one row
// per event) and the orderbook file (row k is the book right after message k). Prices and times
// are integers throughout; nothing here uses a fractional number type.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

// One row of a message file. time_ns is nanoseconds after midnight; price is dollars x 10000;
// direction is -1 (sell), +1 (buy), and for an execution the side of the RESTING order. Types
// 6 (cross) and 7 (halt) may carry direction 0.
struct LobsterMsg {
  std::int64_t time_ns;
  int type;
  std::int64_t order_id;
  std::int64_t size;
  std::int64_t price;
  int direction;
  bool operator==(const LobsterMsg&) const = default;
};

// A parse result: the value, or a short reason that names the offending field. Parsing never
// throws. The flags describe the line even when it is rejected (as far as it was read).
template <class T>
struct Parsed {
  std::optional<T> value;
  std::string error;              // empty when value is set
  bool time_truncated = false;    // message line: the time had more than 9 fractional digits
  bool cr_stripped = false;       // the line ended in '\r', which was removed
  bool oracle_malformed = false;  // book line: levels out of order (see parse_book_line)
  bool ok() const { return value.has_value(); }
};

struct BookLevel {
  std::int64_t price;
  std::int64_t size;
  bool empty() const { return size == 0; }
  bool operator==(const BookLevel&) const = default;
};

// asks[0] and bids[0] are the best levels.
struct BookRow {
  std::vector<BookLevel> asks;
  std::vector<BookLevel> bids;
  bool operator==(const BookRow&) const = default;
};

// Exactly 6 comma-separated fields (one trailing '\r' is stripped first):
// Time,Type,OrderID,Size,Price,Direction. Time is a decimal string, seconds after midnight; it is
// never read through a fractional number type: seconds * 1e9 plus the fraction padded on the right
// to 9 digits. A fraction longer than 9 digits is cut to 9 and time_truncated is set.
// Rejected: wrong field count; an empty or non-numeric field (a leading '+' or a space is
// non-numeric); a time without integer digits, with a '.' and no fraction digits, or that does not
// fit in 64-bit nanoseconds; a type outside 1..7; direction not -1 or +1 for types 1..5 (types 6
// and 7 also accept 0); size <= 0 for types 1..5; price <= 0 for types 1..4.
Parsed<LobsterMsg> parse_message_line(std::string_view line);

// <seconds>.<9 digits>,type,id,size,price,direction, no trailing newline. For a non-negative
// time_ns. If a line parses, parsing the formatted value gives an equal message that is not
// truncated.
std::string format_message_line(const LobsterMsg& m);

// Exactly 4 * levels fields (same '\r' rule), all integers: for level 1..levels the ask price,
// ask size, bid price, bid size. A level with size 0 is empty, whatever its price (files use a
// placeholder price there). Size < 0 is rejected. The row is returned even when its levels are
// out of order, with oracle_malformed set: that is, when the non-empty ask prices do not strictly
// increase, the non-empty bid prices do not strictly decrease, or a side has a non-empty level
// after an empty one.
Parsed<BookRow> parse_book_line(std::string_view line, int levels);

// L from the end of a file name TICKER_DATE_34200000_57600000_orderbook_L.csv or ..._message_L.csv,
// after any directory prefix ('/' separated); nullopt for any other name or L < 1.
std::optional<int> levels_from_filename(std::string_view path);

// Number of comma-separated fields of a line (one trailing '\r' ignored); an empty line has 1.
std::size_t count_fields(std::string_view line);

// L for a row of `fields` columns: fields / 4, or nullopt unless fields is a positive multiple of 4.
// The replayer checks this against levels_from_filename.
std::optional<int> levels_from_columns(std::size_t fields);

}  // namespace replay
