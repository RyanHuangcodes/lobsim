// The ITCH 5.0 wire layer (upgrade 6, F1): hand-checked bytes for every message type, and the
// byte-order helpers.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <vector>

#include "feed/itch.hpp"

namespace {

using feed::MsgA;
using feed::MsgD;
using feed::MsgE;
using feed::MsgS;
using feed::MsgX;

std::vector<std::byte> bytes(std::initializer_list<int> v) {
  std::vector<std::byte> out;
  for (const int b : v) out.push_back(static_cast<std::byte>(b));
  return out;
}

template <class Msg>
std::vector<std::byte> encode(const Msg& m) {
  std::array<std::byte, feed::kMaxFramed> buf{};
  const std::size_t n = feed::write(buf.data(), m);
  return std::vector<std::byte>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
}

feed::Prefix at(std::uint64_t simulated_ns) { return feed::Prefix{1, 0, feed::kOpenNs + simulated_ns}; }

// SPEC upgrade 6, section 3: order 7, buy 5 lots at 10,000 ticks, at simulated time 1.5 s.
// 34,200 s + 1.5 s = 34,201,500,000,000 ns = 0x1F1B28421F00; 10,000 x 100 = 1,000,000 = 0x000F4240.
TEST(FeedItch, GoldenAddOrder) {
  const MsgA m{at(1'500'000'000), 7, 'B', 5, feed::kStock, 10'000 * 100};
  const std::vector<std::byte> want = bytes({
      0x00, 0x24,                                      // length 36
      0x41,                                            // 'A'
      0x00, 0x01,                                      // stock locate 1
      0x00, 0x00,                                      // tracking number 0
      0x1f, 0x1b, 0x28, 0x42, 0x1f, 0x00,              // 34,201,500,000,000 ns
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,  // order ref 7
      0x42,                                            // 'B'
      0x00, 0x00, 0x00, 0x05,                          // 5 shares
      0x4c, 0x4f, 0x42, 0x53, 0x49, 0x4d, 0x20, 0x20,  // "LOBSIM  "
      0x00, 0x0f, 0x42, 0x40,                          // 1,000,000 = $100.0000
  });
  ASSERT_EQ(want.size(), 38u);
  EXPECT_EQ(encode(m), want);
  EXPECT_EQ(feed::read_A(want.data() + 2), m);
}

// Start of messages at simulated time 0: 34,200,000,000,000 ns = 0x1F1ACED9F000.
TEST(FeedItch, GoldenSystemEvent) {
  const MsgS m{at(0), 'O'};
  const std::vector<std::byte> want = bytes({
      0x00, 0x0c,                          // length 12
      0x53,                                // 'S'
      0x00, 0x01, 0x00, 0x00,              // locate 1, tracking 0
      0x1f, 0x1a, 0xce, 0xd9, 0xf0, 0x00,  // 34,200,000,000,000 ns
      0x4f,                                // 'O'
  });
  EXPECT_EQ(encode(m), want);
  EXPECT_EQ(feed::read_S(want.data() + 2), m);
}

// Order 7 executed for 3 shares, match number 42, at 2 s: 34,202,000,000,000 ns = 0x1F1B460F8400.
TEST(FeedItch, GoldenOrderExecuted) {
  const MsgE m{at(2'000'000'000), 7, 3, 42};
  const std::vector<std::byte> want = bytes({
      0x00, 0x1f,                                      // length 31
      0x45,                                            // 'E'
      0x00, 0x01, 0x00, 0x00,                          // locate 1, tracking 0
      0x1f, 0x1b, 0x46, 0x0f, 0x84, 0x00,              // 34,202,000,000,000 ns
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,  // order ref 7
      0x00, 0x00, 0x00, 0x03,                          // 3 shares
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2a,  // match number 42
  });
  EXPECT_EQ(encode(m), want);
  EXPECT_EQ(feed::read_E(want.data() + 2), m);
}

// Order 7 reduced by 2 shares at 2.5 s: 34,202,500,000,000 ns = 0x1F1B63DCE900.
TEST(FeedItch, GoldenOrderCancel) {
  const MsgX m{at(2'500'000'000), 7, 2};
  const std::vector<std::byte> want = bytes({
      0x00, 0x17,                                      // length 23
      0x58,                                            // 'X'
      0x00, 0x01, 0x00, 0x00,                          // locate 1, tracking 0
      0x1f, 0x1b, 0x63, 0xdc, 0xe9, 0x00,              // 34,202,500,000,000 ns
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,  // order ref 7
      0x00, 0x00, 0x00, 0x02,                          // 2 shares
  });
  EXPECT_EQ(encode(m), want);
  EXPECT_EQ(feed::read_X(want.data() + 2), m);
}

// Order 7 deleted at 3 s: 34,203,000,000,000 ns = 0x1F1B81AA4E00.
TEST(FeedItch, GoldenOrderDelete) {
  const MsgD m{at(3'000'000'000), 7};
  const std::vector<std::byte> want = bytes({
      0x00, 0x13,                                      // length 19
      0x44,                                            // 'D'
      0x00, 0x01, 0x00, 0x00,                          // locate 1, tracking 0
      0x1f, 0x1b, 0x81, 0xaa, 0x4e, 0x00,              // 34,203,000,000,000 ns
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07,  // order ref 7
  });
  EXPECT_EQ(encode(m), want);
  EXPECT_EQ(feed::read_D(want.data() + 2), m);
}

TEST(FeedItch, BodyLengthTable) {
  EXPECT_EQ(feed::kBodyLen['S'], 12);
  EXPECT_EQ(feed::kBodyLen['A'], 36);
  EXPECT_EQ(feed::kBodyLen['E'], 31);
  EXPECT_EQ(feed::kBodyLen['X'], 23);
  EXPECT_EQ(feed::kBodyLen['D'], 19);
  int known = 0;
  for (const std::uint8_t n : feed::kBodyLen) known += n != 0 ? 1 : 0;
  EXPECT_EQ(known, 5);  // every other type is unknown to this feed and skipped by its length
}

template <class T>
void round_trip(T v) {
  std::byte buf[sizeof(T)];
  feed::be::store(buf, v);
  EXPECT_EQ(feed::be::load<T>(buf), v);
  // Big-endian: the most significant byte comes first.
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    EXPECT_EQ(static_cast<unsigned>(buf[i]), static_cast<unsigned>((v >> (8 * (sizeof(T) - 1 - i))) & 0xff));
  }
}

TEST(FeedBigEndian, StoreThenLoad16) {
  for (const std::uint16_t v : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0xffff}, std::uint16_t{0x1234}}) {
    round_trip(v);
  }
}

TEST(FeedBigEndian, StoreThenLoad32) {
  for (const std::uint32_t v : {0u, 1u, 0xffffffffu, 0x12345678u}) round_trip(v);
}

TEST(FeedBigEndian, StoreThenLoad64) {
  for (const std::uint64_t v : {std::uint64_t{0}, std::uint64_t{1}, std::numeric_limits<std::uint64_t>::max(),
                                std::uint64_t{0x0123456789abcdefULL}}) {
    round_trip(v);
  }
}

TEST(FeedBigEndian, StoreThenLoad48) {
  for (const std::uint64_t v : {std::uint64_t{0}, std::uint64_t{1}, feed::kTimestampLimit - 1,
                                std::uint64_t{0x123456789abcULL}}) {
    std::byte buf[8]{std::byte{0xee}, std::byte{0xee}, std::byte{0xee}, std::byte{0xee},
                     std::byte{0xee}, std::byte{0xee}, std::byte{0xee}, std::byte{0xee}};
    feed::be::store48(buf, v);
    EXPECT_EQ(feed::be::load48(buf), v);
    for (int i = 0; i < 6; ++i) {
      EXPECT_EQ(static_cast<unsigned>(buf[i]), static_cast<unsigned>((v >> (8 * (5 - i))) & 0xff));
    }
    EXPECT_EQ(buf[6], std::byte{0xee});  // exactly 6 bytes written
    EXPECT_EQ(buf[7], std::byte{0xee});
  }
}

}  // namespace
