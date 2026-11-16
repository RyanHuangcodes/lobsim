#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

using namespace lob;

static_assert(std::is_same_v<Price, std::int64_t>);
static_assert(std::is_same_v<Qty, std::int64_t>);
static_assert(std::is_same_v<OrderId, std::uint64_t>);
static_assert(std::is_same_v<OwnerId, std::uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<Side>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<OrderType>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<TIF>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<STP>, std::uint8_t>);
static_assert(opposite(Side::Buy) == Side::Sell && opposite(Side::Sell) == Side::Buy);

namespace {

std::uint64_t hash_bytes(const std::vector<std::uint8_t>& b) {
  EventHasher h;
  h.add_bytes(b.data(), b.size());
  return h.value();
}

std::uint64_t hash_event(const Event& e) {
  EventHasher h;
  h.add(e);
  return h.value();
}

}  // namespace

TEST(Fnv1a, KnownVectors) {
  EventHasher empty;
  EXPECT_EQ(empty.value(), 0xcbf29ce484222325ULL);
  EventHasher a;
  a.add_bytes("a", 1);
  EXPECT_EQ(a.value(), 0xaf63dc4c8601ec8cULL);
  EventHasher foobar;
  foobar.add_bytes("foobar", 6);
  EXPECT_EQ(foobar.value(), 0x85944171f73967e8ULL);
}

TEST(EventHashGolden, Ack) {
  Ack e{0x0102030405060708ULL, 0x11121314U, 0x2122232425262728ULL};
  EXPECT_EQ(hash_event(e),
            hash_bytes({0, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x14, 0x13, 0x12, 0x11,
                        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21}));
}

TEST(EventHashGolden, Reject) {
  Reject e{0x11121314U, RequestKind::Modify, 0x2122232425262728ULL, 0x3132333435363738ULL,
           RejectReason::PostOnlyWouldCross};
  EXPECT_EQ(hash_event(e),
            hash_bytes({1, 0x14, 0x13, 0x12, 0x11, 2, 0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22,
                        0x21, 0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31, 6}));
}

TEST(EventHashGolden, Trade) {
  Trade e{0x0102030405060708ULL, 0x1112131415161718LL, 0x2122232425262728LL,
          0x3132333435363738ULL, 0x4142434445464748ULL, 0x51525354U, 0x61626364U, Side::Sell};
  EXPECT_EQ(hash_event(e),
            hash_bytes({2, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
                        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
                        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
                        0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
                        0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41,
                        0x54, 0x53, 0x52, 0x51, 0x64, 0x63, 0x62, 0x61, 1}));
}

TEST(EventHashGolden, Cancelled) {
  Cancelled e{0x0102030405060708ULL, 0x11121314U, 0x2122232425262728LL, CancelReason::SelfTrade};
  EXPECT_EQ(hash_event(e),
            hash_bytes({3, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x14, 0x13, 0x12, 0x11,
                        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, 4}));
}

TEST(EventHashGolden, Modified) {
  Modified e{0x0102030405060708ULL, 0x11121314U, 0x2122232425262728LL, 0x3132333435363738LL, true};
  EXPECT_EQ(hash_event(e),
            hash_bytes({4, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x14, 0x13, 0x12, 0x11,
                        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
                        0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31, 1}));
}

TEST(EventHashGolden, BufferEqualsSequentialAdds) {
  EventBuffer buf{Ack{1, 2, 3}, Cancelled{1, 2, 3, CancelReason::User}};
  EventHasher a, b;
  a.add(buf);
  b.add(buf[0]);
  b.add(buf[1]);
  EXPECT_EQ(a.value(), b.value());
}

TEST(Types, NewOrderDefaults) {
  NewOrder o;
  EXPECT_EQ(o.stp, STP::CancelTaking);
  EXPECT_FALSE(o.post_only);
  EXPECT_EQ(o.tif, TIF::GTC);
}

TEST(Types, ToStringReadable) {
  EXPECT_STREQ(to_string(Side::Sell), "Sell");
  EXPECT_NE(to_string(Event{Trade{}}).find("Trade{trade_id=0"), std::string::npos);
}
