#include "sim/order_log.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <istream>
#include <sstream>
#include <type_traits>
#include <variant>
#include <vector>

namespace sim {

namespace {

constexpr std::string_view kHeaderPrefix = "# lobsim orders.log v1 max_price=";

template <class Int>
void put(std::string& out, Int value) {
  std::array<char, 24> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  out.append(buf.data(), res.ptr);
}

template <class Int>
void put_field(std::string& out, Int value) {
  out.push_back(' ');
  put(out, value);
}

// Parses the whole of `token` as a decimal integer of type Int.
template <class Int>
bool parse(std::string_view token, Int& out) {
  if (token.empty()) return false;
  const auto res = std::from_chars(token.data(), token.data() + token.size(), out);
  return res.ec == std::errc{} && res.ptr == token.data() + token.size();
}

// An enum or bool stored as an integer in [0, max].
template <class T>
bool parse_small(std::string_view token, int max, T& out) {
  int v = 0;
  if (!parse(token, v) || v < 0 || v > max) return false;
  out = static_cast<T>(v);
  return true;
}

// Splits on single spaces. Empty tokens (two spaces, a leading or trailing space) are
// kept, and fail later when parsed as integers. A request line has at most 11 tokens, so the
// first 11 are stored and the rest only counted: size() is the full count, which every check
// compares with 11 or fewer, and no allocation happens per line (D-913).
struct Tokens {
  static constexpr std::size_t kMax = 11;
  std::array<std::string_view, kMax> t;
  std::size_t n = 0;
  std::size_t size() const noexcept { return n; }
  std::string_view operator[](std::size_t i) const noexcept { return t[i]; }
};

Tokens split(std::string_view line) {
  Tokens tokens;
  std::size_t start = 0;
  for (;;) {
    const std::size_t space = line.find(' ', start);
    const std::string_view token =
        space == std::string_view::npos ? line.substr(start) : line.substr(start, space - start);
    if (tokens.n < Tokens::kMax) tokens.t[tokens.n] = token;
    ++tokens.n;
    if (space == std::string_view::npos) return tokens;
    start = space + 1;
  }
}

}  // namespace

std::string format_header(lob::Price max_price) {
  std::string s(kHeaderPrefix);
  put(s, max_price);
  return s;
}

std::optional<lob::Price> parse_header(std::string_view line) {
  if (line.substr(0, kHeaderPrefix.size()) != kHeaderPrefix) return std::nullopt;
  lob::Price max_price = 0;
  if (!parse(line.substr(kHeaderPrefix.size()), max_price)) return std::nullopt;
  return max_price;
}

void append_request(std::string& out, TimeNs time, const lob::Request& request) {
  put(out, time);
  std::visit(
      [&out](const auto& r) {
        using T = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<T, lob::NewOrder>) {
          out.append(" N");
          put_field(out, r.owner);
          put_field(out, r.client_order_id);
          put_field(out, static_cast<int>(r.side));
          put_field(out, static_cast<int>(r.type));
          put_field(out, static_cast<int>(r.tif));
          put_field(out, r.price);
          put_field(out, r.qty);
          put_field(out, r.post_only ? 1 : 0);
          put_field(out, static_cast<int>(r.stp));
        } else if constexpr (std::is_same_v<T, lob::Cancel>) {
          out.append(" C");
          put_field(out, r.owner);
          put_field(out, r.order_id);
        } else {
          out.append(" M");
          put_field(out, r.owner);
          put_field(out, r.order_id);
          put_field(out, r.new_price);
          put_field(out, r.new_qty);
        }
      },
      request);
}

std::string format_request(TimeNs time, const lob::Request& request) {
  std::string s;
  append_request(s, time, request);
  return s;
}

std::optional<std::pair<TimeNs, lob::Request>> parse_request(std::string_view line) {
  const Tokens t = split(line);
  if (t.size() < 2 || t[1].size() != 1) return std::nullopt;
  TimeNs time = 0;
  if (!parse(t[0], time) || time < 0) return std::nullopt;

  switch (t[1][0]) {
    case 'N': {
      lob::NewOrder o;
      if (t.size() != 11) return std::nullopt;
      if (!parse(t[2], o.owner) || !parse(t[3], o.client_order_id) || !parse_small(t[4], 1, o.side) ||
          !parse_small(t[5], 1, o.type) || !parse_small(t[6], 2, o.tif) || !parse(t[7], o.price) ||
          !parse(t[8], o.qty) || !parse_small(t[9], 1, o.post_only) || !parse_small(t[10], 1, o.stp))
        return std::nullopt;
      return std::pair<TimeNs, lob::Request>{time, o};
    }
    case 'C': {
      lob::Cancel c;
      if (t.size() != 4) return std::nullopt;
      if (!parse(t[2], c.owner) || !parse(t[3], c.order_id)) return std::nullopt;
      return std::pair<TimeNs, lob::Request>{time, c};
    }
    case 'M': {
      lob::Modify m;
      if (t.size() != 6) return std::nullopt;
      if (!parse(t[2], m.owner) || !parse(t[3], m.order_id) || !parse(t[4], m.new_price) || !parse(t[5], m.new_qty))
        return std::nullopt;
      return std::pair<TimeNs, lob::Request>{time, m};
    }
    default:
      return std::nullopt;
  }
}

OrderLogReader::OrderLogReader(std::istream& in) : in_(in) {
  if (!std::getline(in_, line_)) throw ReplayError(1, "empty log: the header line is missing");
  const auto max_price = parse_header(line_);
  if (!max_price) throw ReplayError(1, "malformed header: " + line_);
  max_price_ = *max_price;
}

std::optional<LoggedRequest> OrderLogReader::next() {
  if (!std::getline(in_, line_)) return std::nullopt;
  ++line_no_;
  const auto parsed = parse_request(line_);
  if (!parsed) throw ReplayError(line_no_, "malformed request: " + line_);
  if (parsed->first < previous_time_)
    throw ReplayError(line_no_, "time goes backwards: " + std::to_string(parsed->first) + " after " +
                                    std::to_string(previous_time_));
  previous_time_ = parsed->first;
  return LoggedRequest{parsed->first, parsed->second};
}

OrderLog parse_order_log(std::string_view contents) {
  std::istringstream in{std::string(contents)};
  OrderLogReader reader(in);
  OrderLog log{reader.max_price(), {}};
  while (auto r = reader.next()) log.requests.push_back(*r);
  return log;
}

std::string format_order_log(const OrderLog& log) {
  std::string out = format_header(log.max_price);
  out.push_back('\n');
  for (const LoggedRequest& r : log.requests) {
    append_request(out, r.time, r.request);
    out.push_back('\n');
  }
  return out;
}

}  // namespace sim
