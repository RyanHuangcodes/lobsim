#pragma once

// The wire layer of the binary market-data feed (upgrade 6): a subset of Nasdaq TotalView-ITCH
// 5.0. Five message types (S, A, E, X, D) with ITCH's own fixed body layouts, every integer
// big-endian, each body framed by a 2-byte big-endian length. Header-only: the encoder, the
// parser and the fuzz target all use these functions. See FEED.md for the byte tables.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace feed {

// Body lengths (without the 2-byte length prefix).
inline constexpr std::size_t kLenS = 12;
inline constexpr std::size_t kLenA = 36;
inline constexpr std::size_t kLenE = 31;
inline constexpr std::size_t kLenX = 23;
inline constexpr std::size_t kLenD = 19;
inline constexpr std::size_t kPrefix = 2;  // the length prefix of every message
inline constexpr std::size_t kMaxFramed = kPrefix + kLenA;  // the longest message this encoder writes

// The body length of each type this feed uses, indexed by the type byte; 0 for every other type.
inline constexpr std::array<std::uint8_t, 256> kBodyLen = [] {
  std::array<std::uint8_t, 256> t{};
  t['S'] = kLenS;
  t['A'] = kLenA;
  t['E'] = kLenE;
  t['X'] = kLenX;
  t['D'] = kLenD;
  return t;
}();

inline constexpr std::uint64_t kOpenNs = 34'200'000'000'000;   // 9:30:00 after midnight (D-603)
inline constexpr std::uint64_t kTimestampLimit = 1ULL << 48;   // 6-byte timestamps are below this
inline constexpr std::uint16_t kLocate = 1;
inline constexpr std::array<char, 8> kStock = {'L', 'O', 'B', 'S', 'I', 'M', ' ', ' '};

// Byte order. The project is C++20, so there is no std::byteswap: the builtins are what libstdc++
// and libc++ use for it. Every access is a memcpy, never a cast of a byte pointer to a wider
// integer pointer, which would be undefined on an unaligned address.
namespace be {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "mixed-endian platforms are not supported");

template <class T>
constexpr T bswap(T v) noexcept {
  static_assert(std::is_unsigned_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8));
  if constexpr (sizeof(T) == 1) return v;
  else if constexpr (sizeof(T) == 2) return __builtin_bswap16(v);
  else if constexpr (sizeof(T) == 4) return __builtin_bswap32(v);
  else return __builtin_bswap64(v);
}

template <class T>
inline void store(std::byte* p, T v) noexcept {
  if constexpr (std::endian::native == std::endian::little) v = bswap(v);
  std::memcpy(p, &v, sizeof v);
}

template <class T>
inline T load(const std::byte* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof v);
  if constexpr (std::endian::native == std::endian::little) v = bswap(v);
  return v;
}

// The low 48 bits of v, as 6 big-endian bytes.
inline void store48(std::byte* p, std::uint64_t v) noexcept {
  std::byte tmp[8];
  store(tmp, v << 16);
  std::memcpy(p, tmp, 6);
}

inline std::uint64_t load48(const std::byte* p) noexcept {
  std::byte tmp[8]{};
  std::memcpy(tmp, p, 6);
  return load<std::uint64_t>(tmp) >> 16;
}

}  // namespace be

// The fields every body starts with, after the type byte (offsets 1 to 10).
struct Prefix {
  std::uint16_t locate{kLocate};
  std::uint16_t tracking{0};
  std::uint64_t timestamp{0};  // ns after midnight, below 2^48
  bool operator==(const Prefix&) const = default;
};

struct MsgS {  // System Event
  Prefix p;
  char event_code{'O'};  // O start of messages, C end of messages
  bool operator==(const MsgS&) const = default;
};

struct MsgA {  // Add Order, no MPID attribution
  Prefix p;
  std::uint64_t ref{0};
  char side{'B'};  // B buy, S sell
  std::uint32_t shares{0};
  std::array<char, 8> stock{kStock};
  std::uint32_t price{0};  // 4 implied decimals
  bool operator==(const MsgA&) const = default;
};

struct MsgE {  // Order Executed, at the resting order's price
  Prefix p;
  std::uint64_t ref{0};
  std::uint32_t shares{0};
  std::uint64_t match{0};
  bool operator==(const MsgE&) const = default;
};

struct MsgX {  // Order Cancel: part of the order's shares
  Prefix p;
  std::uint64_t ref{0};
  std::uint32_t shares{0};
  bool operator==(const MsgX&) const = default;
};

struct MsgD {  // Order Delete
  Prefix p;
  std::uint64_t ref{0};
  bool operator==(const MsgD&) const = default;
};

namespace detail {

// Length prefix, type byte and the common prefix; returns the body pointer.
inline std::byte* frame(std::byte* out, char type, std::size_t len, const Prefix& p) noexcept {
  be::store(out, static_cast<std::uint16_t>(len));
  std::byte* body = out + kPrefix;
  body[0] = static_cast<std::byte>(type);
  be::store(body + 1, p.locate);
  be::store(body + 3, p.tracking);
  be::store48(body + 5, p.timestamp);
  return body;
}

inline Prefix read_prefix(const std::byte* body) noexcept {
  return Prefix{be::load<std::uint16_t>(body + 1), be::load<std::uint16_t>(body + 3), be::load48(body + 5)};
}

}  // namespace detail

// Each write puts one framed message (length prefix + body) at `out` and returns its size.
// The caller guarantees room for kMaxFramed bytes.
inline std::size_t write(std::byte* out, const MsgS& m) noexcept {
  std::byte* b = detail::frame(out, 'S', kLenS, m.p);
  b[11] = static_cast<std::byte>(m.event_code);
  return kPrefix + kLenS;
}

inline std::size_t write(std::byte* out, const MsgA& m) noexcept {
  std::byte* b = detail::frame(out, 'A', kLenA, m.p);
  be::store(b + 11, m.ref);
  b[19] = static_cast<std::byte>(m.side);
  be::store(b + 20, m.shares);
  std::memcpy(b + 24, m.stock.data(), 8);
  be::store(b + 32, m.price);
  return kPrefix + kLenA;
}

inline std::size_t write(std::byte* out, const MsgE& m) noexcept {
  std::byte* b = detail::frame(out, 'E', kLenE, m.p);
  be::store(b + 11, m.ref);
  be::store(b + 19, m.shares);
  be::store(b + 23, m.match);
  return kPrefix + kLenE;
}

inline std::size_t write(std::byte* out, const MsgX& m) noexcept {
  std::byte* b = detail::frame(out, 'X', kLenX, m.p);
  be::store(b + 11, m.ref);
  be::store(b + 19, m.shares);
  return kPrefix + kLenX;
}

inline std::size_t write(std::byte* out, const MsgD& m) noexcept {
  std::byte* b = detail::frame(out, 'D', kLenD, m.p);
  be::store(b + 11, m.ref);
  return kPrefix + kLenD;
}

// Each read takes a body (starting at the type byte) whose length was already checked.
inline MsgS read_S(const std::byte* b) noexcept {
  return MsgS{detail::read_prefix(b), static_cast<char>(b[11])};
}

inline MsgA read_A(const std::byte* b) noexcept {
  MsgA m;
  m.p = detail::read_prefix(b);
  m.ref = be::load<std::uint64_t>(b + 11);
  m.side = static_cast<char>(b[19]);
  m.shares = be::load<std::uint32_t>(b + 20);
  std::memcpy(m.stock.data(), b + 24, 8);
  m.price = be::load<std::uint32_t>(b + 32);
  return m;
}

inline MsgE read_E(const std::byte* b) noexcept {
  return MsgE{detail::read_prefix(b), be::load<std::uint64_t>(b + 11), be::load<std::uint32_t>(b + 19),
              be::load<std::uint64_t>(b + 23)};
}

inline MsgX read_X(const std::byte* b) noexcept {
  return MsgX{detail::read_prefix(b), be::load<std::uint64_t>(b + 11), be::load<std::uint32_t>(b + 19)};
}

inline MsgD read_D(const std::byte* b) noexcept { return MsgD{detail::read_prefix(b), be::load<std::uint64_t>(b + 11)}; }

}  // namespace feed
