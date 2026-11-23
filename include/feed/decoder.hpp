#pragma once

// The feed parser (upgrade 6): walks any ITCH 5.0 file framed with 2-byte big-endian lengths.
// The handler is a template parameter, so every call inlines (no virtual call, no std::function,
// no std::variant). Types this feed does not use are skipped by their length and counted, so the
// parser also walks a full Nasdaq file. It never reads past the buffer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "feed/itch.hpp"

namespace feed {

enum class ParseError : std::uint8_t {
  None,
  TruncatedLength,  // fewer than 2 bytes left for a length prefix
  TruncatedBody,    // the length prefix promises more bytes than are left
  EmptyBody,        // a length of 0: no type byte
  MalformedLength,  // a type of this feed whose length differs from kBodyLen
  Stopped,          // the handler returned false
};

inline const char* to_string(ParseError e) noexcept {
  switch (e) {
    case ParseError::None: return "none";
    case ParseError::TruncatedLength: return "truncated length prefix";
    case ParseError::TruncatedBody: return "truncated message body";
    case ParseError::EmptyBody: return "message of length 0";
    case ParseError::MalformedLength: return "length differs from the ITCH layout of its type";
    case ParseError::Stopped: return "stopped by the handler";
  }
  return "?";
}

struct ParseResult {
  ParseError error{ParseError::None};
  std::size_t offset{0};      // byte offset of the failing message, or the buffer size
  std::uint64_t messages{0};  // messages passed to the handler
  std::uint64_t unknown{0};   // of which: types this feed does not use
  std::array<std::uint64_t, 256> by_type{};
  bool ok() const noexcept { return error == ParseError::None; }
};

// Handler: bool on(const MsgS&), on(const MsgA&), on(const MsgE&), on(const MsgX&), on(const MsgD&),
// and bool on_unknown(std::uint8_t type, std::span<const std::byte> body). Returning false stops
// the parse with ParseError::Stopped at that message.
template <class Handler>
ParseResult parse(std::span<const std::byte> buf, Handler& h) {
  ParseResult r;
  const std::byte* const p = buf.data();
  const std::size_t n = buf.size();
  std::size_t at = 0;
  while (at < n) {
    r.offset = at;
    if (n - at < kPrefix) {
      r.error = ParseError::TruncatedLength;
      return r;
    }
    const std::size_t len = be::load<std::uint16_t>(p + at);
    if (len == 0) {
      r.error = ParseError::EmptyBody;
      return r;
    }
    if (n - at - kPrefix < len) {
      r.error = ParseError::TruncatedBody;
      return r;
    }
    const std::byte* const b = p + at + kPrefix;
    const auto type = static_cast<std::uint8_t>(b[0]);
    const std::size_t want = kBodyLen[type];
    if (want != 0 && want != len) {
      r.error = ParseError::MalformedLength;
      return r;
    }
    bool go;
    switch (type) {
      case 'S': go = h.on(read_S(b)); break;
      case 'A': go = h.on(read_A(b)); break;
      case 'E': go = h.on(read_E(b)); break;
      case 'X': go = h.on(read_X(b)); break;
      case 'D': go = h.on(read_D(b)); break;
      default:
        go = h.on_unknown(type, std::span<const std::byte>(b, len));
        ++r.unknown;
        break;
    }
    ++r.messages;
    ++r.by_type[type];
    if (!go) {
      r.error = ParseError::Stopped;
      return r;
    }
    at += kPrefix + len;
  }
  r.offset = at;
  return r;
}

// A handler that writes every parsed message back out: used to check that parse then encode is
// byte-identical (the round-trip check of `lobsim_feed check` and of the fuzz target).
struct Reencoder {
  std::vector<std::byte> out;

  template <class Msg>
  bool on(const Msg& m) {
    std::byte tmp[kMaxFramed];
    const std::size_t k = write(tmp, m);
    out.insert(out.end(), tmp, tmp + k);
    return true;
  }
  bool on_unknown(std::uint8_t, std::span<const std::byte> body) {
    std::byte len[kPrefix];
    be::store(len, static_cast<std::uint16_t>(body.size()));
    out.insert(out.end(), len, len + kPrefix);
    out.insert(out.end(), body.begin(), body.end());
    return true;
  }
};

}  // namespace feed
