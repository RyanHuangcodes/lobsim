// MoldUDP64 packets around the ITCH feed (upgrade 6, F7): hand-checked bytes, packet size limits,
// the round trip, and gap, duplicate and error detection by the reader.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <vector>

#include "feed/itch.hpp"
#include "feed/moldudp64.hpp"

namespace {

namespace mold = feed::mold;

std::vector<std::byte> bytes(std::initializer_list<int> v) {
  std::vector<std::byte> out;
  for (const int b : v) out.push_back(static_cast<std::byte>(b));
  return out;
}

template <class Msg>
void append(std::vector<std::byte>& out, const Msg& m) {
  std::array<std::byte, feed::kMaxFramed> tmp{};
  const std::size_t n = feed::write(tmp.data(), m);
  out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n));
}

feed::Prefix at(std::uint64_t ns) { return feed::Prefix{feed::kLocate, 0, feed::kOpenNs + ns}; }

// n framed messages: S O, then n - 1 alternating A and D of the same order, so sizes vary (14, 38, 21).
std::vector<std::byte> feed_of(std::size_t n) {
  std::vector<std::byte> f;
  append(f, feed::MsgS{at(0), 'O'});
  for (std::size_t i = 1; i < n; ++i) {
    if (i % 2 == 1) append(f, feed::MsgA{at(i), 1, 'B', 5, feed::kStock, 1'000'000});
    else append(f, feed::MsgD{at(i), 1});
  }
  return f;
}

struct Recorder {
  std::vector<std::uint64_t> seqs;
  std::vector<std::byte> feed;
  bool on_message(std::uint64_t seq, std::span<const std::byte> framed) {
    seqs.push_back(seq);
    feed.insert(feed.end(), framed.begin(), framed.end());
    return true;
  }
};

// One packet carrying the golden A of FEED.md, then the end of the session.
TEST(FeedMold, GoldenPacket) {
  std::vector<std::byte> f;
  append(f, feed::MsgA{at(1'500'000'000), 7, 'B', 5, feed::kStock, 1'000'000});
  mold::PacketizeStats st;
  const std::vector<std::byte> got = mold::packetize(f, mold::PacketizeOptions{}, &st);
  std::vector<std::byte> want = bytes({
      0x00, 0x3a,                                                  // packet length 58 = 20 + 38
      0x4c, 0x4f, 0x42, 0x53, 0x49, 0x4d, 0x20, 0x20, 0x20, 0x20,  // session "LOBSIM    "
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // sequence number 1
      0x00, 0x01,                                                  // 1 message
  });
  want.insert(want.end(), f.begin(), f.end());  // the message block: its 2-byte length and the A
  const std::vector<std::byte> end = bytes({
      0x00, 0x14,                                                  // packet length 20
      0x4c, 0x4f, 0x42, 0x53, 0x49, 0x4d, 0x20, 0x20, 0x20, 0x20,  // session
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,              // the next sequence number, 2
      0xff, 0xff,                                                  // end of session
  });
  want.insert(want.end(), end.begin(), end.end());
  EXPECT_EQ(got, want);
  EXPECT_EQ(st.packets, 1u);
  EXPECT_EQ(st.messages, 1u);
  EXPECT_EQ(st.bytes, want.size());
}

TEST(FeedMold, HeaderRoundTrip) {
  const mold::PacketHeader h{*mold::make_session("ABC123"), 0x0102030405060708ULL, 0xabcd};
  std::array<std::byte, mold::kHeader> buf{};
  mold::write_header(buf.data(), h);
  EXPECT_EQ(mold::read_header(buf.data()), h);
  EXPECT_EQ(mold::to_string(h.session), "ABC123");
}

TEST(FeedMold, SessionNames) {
  EXPECT_TRUE(mold::make_session("LOBSIM").has_value());
  EXPECT_TRUE(mold::make_session("0123456789").has_value());
  EXPECT_FALSE(mold::make_session("").has_value());
  EXPECT_FALSE(mold::make_session("01234567890").has_value());
  EXPECT_FALSE(mold::make_session("A B").has_value());
}

TEST(FeedMold, PacketsRespectTheLimitAndCarryEveryMessageInOrder) {
  const std::vector<std::byte> f = feed_of(1000);
  for (const std::size_t limit : {std::size_t{60}, std::size_t{100}, std::size_t{1400}, std::size_t{65535}}) {
    mold::PacketizeOptions opt;
    opt.max_packet = limit;
    mold::PacketizeStats st;
    const std::vector<std::byte> file = mold::packetize(f, opt, &st);
    // Walk the packet file by hand: every packet within the limit, sequence numbers consecutive.
    std::size_t at = 0;
    std::uint64_t next = 1, messages = 0, packets = 0;
    bool ended = false;
    while (at < file.size()) {
      const std::size_t len = feed::be::load<std::uint16_t>(file.data() + at);
      ASSERT_LE(len, limit);
      const mold::PacketHeader h = mold::read_header(file.data() + at + 2);
      EXPECT_EQ(h.sequence, next);
      if (h.count == mold::kEndOfSession) {
        EXPECT_EQ(len, mold::kHeader);
        ended = true;
      } else {
        ASSERT_GT(h.count, 0);
        next += h.count;
        messages += h.count;
        ++packets;
      }
      at += 2 + len;
    }
    EXPECT_TRUE(ended);
    EXPECT_EQ(messages, 1000u);
    EXPECT_EQ(st.messages, 1000u);
    EXPECT_EQ(st.packets, packets);
    EXPECT_EQ(st.bytes, file.size());
    Recorder r;
    const mold::ReadResult res = mold::read_packets(file, r);
    EXPECT_TRUE(res.complete()) << mold::to_string(res.error);
    EXPECT_EQ(r.feed, f) << "limit " << limit;  // the round trip is byte-identical
    EXPECT_EQ(res.messages, 1000u);
    EXPECT_EQ(res.next_sequence, 1001u);
  }
}

TEST(FeedMold, PacketsAreFilledGreedily) {
  // 1400 - 20 = 1380 bytes of blocks: 36 A blocks of 38 bytes (1368) fit, a 37th does not.
  std::vector<std::byte> f;
  for (int i = 0; i < 40; ++i) append(f, feed::MsgA{at(0), static_cast<std::uint64_t>(i + 1), 'B', 1, feed::kStock, 100});
  const std::vector<std::byte> file = mold::packetize(f, mold::PacketizeOptions{});
  EXPECT_EQ(feed::be::load<std::uint16_t>(file.data()), 20 + 36 * 38);
  EXPECT_EQ(mold::read_header(file.data() + 2).count, 36);
  const std::size_t second = 2 + 20 + 36 * 38;
  const mold::PacketHeader h2 = mold::read_header(file.data() + second + 2);
  EXPECT_EQ(h2.sequence, 37u);
  EXPECT_EQ(h2.count, 4);
}

TEST(FeedMold, EmptyFeedIsOnlyTheEndOfSession) {
  mold::PacketizeStats st;
  const std::vector<std::byte> file = mold::packetize(std::span<const std::byte>{}, mold::PacketizeOptions{}, &st);
  EXPECT_EQ(file.size(), 22u);
  EXPECT_EQ(st.packets, 0u);
  Recorder r;
  const mold::ReadResult res = mold::read_packets(file, r);
  EXPECT_TRUE(res.complete());
  EXPECT_EQ(res.packets, 1u);
}

TEST(FeedMold, FirstSequenceAndSession) {
  mold::PacketizeOptions opt;
  opt.first_sequence = 1000;
  opt.session = *mold::make_session("DAY2");
  const std::vector<std::byte> file = mold::packetize(feed_of(5), opt);
  Recorder r;
  const mold::ReadResult res = mold::read_packets(file, r, 1000);
  EXPECT_TRUE(res.complete());
  EXPECT_EQ(mold::to_string(res.session), "DAY2");
  EXPECT_EQ(r.seqs, (std::vector<std::uint64_t>{1000, 1001, 1002, 1003, 1004}));
  // A reader that expects sequence 1 sees messages 1 to 999 as missing.
  Recorder r1;
  const mold::ReadResult from1 = mold::read_packets(file, r1);
  ASSERT_EQ(from1.gaps.size(), 1u);
  EXPECT_EQ(from1.gaps[0], (mold::Gap{1, 999}));
}

TEST(FeedMold, LostPacketIsAGap) {
  mold::PacketizeOptions opt;
  opt.max_packet = 100;  // a few messages per packet
  opt.drop_packet = 3;
  mold::PacketizeStats st;
  const std::vector<std::byte> f = feed_of(60);
  const std::vector<std::byte> file = mold::packetize(f, opt, &st);
  ASSERT_GT(st.dropped_messages, 0u);
  Recorder r;
  const mold::ReadResult res = mold::read_packets(file, r);
  EXPECT_TRUE(res.ok());
  EXPECT_TRUE(res.ended);
  EXPECT_FALSE(res.complete());
  ASSERT_EQ(res.gaps.size(), 1u);
  EXPECT_EQ(res.missing(), st.dropped_messages);
  EXPECT_EQ(res.messages + res.missing(), 60u);
  // The messages after the gap are still delivered, with their own sequence numbers.
  EXPECT_EQ(r.seqs.back(), 60u);
  EXPECT_EQ(r.seqs[res.gaps[0].first - 1], res.gaps[0].last + 1);
}

TEST(FeedMold, LossBeforeTheEndOfSessionIsAGap) {
  mold::PacketizeOptions opt;
  opt.max_packet = 100;
  mold::PacketizeStats st;
  const std::vector<std::byte> full = mold::packetize(feed_of(30), opt, &st);
  // Remove the last message packet: only the end-of-session packet shows that messages were sent.
  std::size_t at = 0, last_start = 0;
  while (true) {
    const std::size_t len = feed::be::load<std::uint16_t>(full.data() + at);
    if (mold::read_header(full.data() + at + 2).count == mold::kEndOfSession) break;
    last_start = at;
    at += 2 + len;
  }
  std::vector<std::byte> file(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(last_start));
  file.insert(file.end(), full.begin() + static_cast<std::ptrdiff_t>(at), full.end());
  Recorder r;
  const mold::ReadResult res = mold::read_packets(file, r);
  ASSERT_EQ(res.gaps.size(), 1u);
  EXPECT_EQ(res.gaps[0].last, 30u);
}

TEST(FeedMold, RetransmittedPacketIsSkippedAsADuplicate) {
  mold::PacketizeOptions opt;
  opt.max_packet = 100;
  const std::vector<std::byte> f = feed_of(20);
  const std::vector<std::byte> full = mold::packetize(f, opt);
  // Repeat the first packet right after the second.
  const std::size_t p1 = 2 + feed::be::load<std::uint16_t>(full.data());
  const std::size_t p2 = p1 + 2 + feed::be::load<std::uint16_t>(full.data() + p1);
  std::vector<std::byte> file(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(p2));
  file.insert(file.end(), full.begin(), full.begin() + static_cast<std::ptrdiff_t>(p1));
  file.insert(file.end(), full.begin() + static_cast<std::ptrdiff_t>(p2), full.end());
  Recorder r;
  const mold::ReadResult res = mold::read_packets(file, r);
  EXPECT_TRUE(res.complete());
  EXPECT_EQ(res.duplicates, mold::read_header(full.data() + 2).count);
  EXPECT_EQ(r.feed, f);
}

TEST(FeedMold, HeartbeatIsAcceptedAndCanShowAGap) {
  const mold::Session s = *mold::make_session("LOBSIM");
  std::vector<std::byte> file(2 + mold::kHeader);
  feed::be::store(file.data(), static_cast<std::uint16_t>(mold::kHeader));
  mold::write_header(file.data() + 2, mold::PacketHeader{s, 1, mold::kHeartbeat});
  Recorder r;
  mold::ReadResult res = mold::read_packets(file, r);
  EXPECT_TRUE(res.ok());
  EXPECT_EQ(res.heartbeats, 1u);
  EXPECT_TRUE(res.gaps.empty());
  EXPECT_FALSE(res.ended);
  // A heartbeat announcing sequence 5 when 1 was expected: 1 to 4 were lost.
  mold::write_header(file.data() + 2, mold::PacketHeader{s, 5, mold::kHeartbeat});
  res = mold::read_packets(file, r);
  ASSERT_EQ(res.gaps.size(), 1u);
  EXPECT_EQ(res.gaps[0], (mold::Gap{1, 4}));
}

TEST(FeedMold, MalformedPacketFilesAreRejectedAtTheirOffset) {
  const std::vector<std::byte> good = mold::packetize(feed_of(3), mold::PacketizeOptions{});
  const auto error_of = [](const std::vector<std::byte>& file) {
    Recorder r;
    return mold::read_packets(file, r);
  };
  std::vector<std::byte> f = good;
  f.push_back(std::byte{0x00});
  EXPECT_EQ(error_of(f).error, mold::ReadError::TruncatedPacketLength);

  f = std::vector<std::byte>(good.begin(), good.end() - 1);
  EXPECT_EQ(error_of(f).error, mold::ReadError::TruncatedPacket);

  f = bytes({0x00, 0x05, 1, 2, 3, 4, 5});
  EXPECT_EQ(error_of(f).error, mold::ReadError::ShortPacket);

  f = good;
  f[2 + 19] = std::byte{9};  // count 9 in a packet that holds 3 messages
  EXPECT_EQ(error_of(f).error, mold::ReadError::MessageOverrun);
  EXPECT_EQ(error_of(f).offset, 0u);

  f = good;
  f[2 + 19] = std::byte{2};  // count 2: one message block left over
  EXPECT_EQ(error_of(f).error, mold::ReadError::CountMismatch);

  f = good;
  const std::size_t end_at = good.size() - 22;
  f[end_at + 2] = std::byte{'X'};  // the end-of-session packet of another session
  EXPECT_EQ(error_of(f).error, mold::ReadError::SessionChanged);
  EXPECT_EQ(error_of(f).offset, end_at);

  f = good;
  f.insert(f.end(), good.begin(), good.end());  // a whole second session after the end
  EXPECT_EQ(error_of(f).error, mold::ReadError::AfterEndOfSession);
}

TEST(FeedMold, PacketizeRefusesBadInput) {
  mold::PacketizeOptions opt;
  opt.max_packet = 22;
  EXPECT_THROW(mold::packetize(feed_of(2), opt), std::invalid_argument);
  opt.max_packet = 65536;
  EXPECT_THROW(mold::packetize(feed_of(2), opt), std::invalid_argument);
  opt.max_packet = 50;  // an A block is 38 bytes: 20 + 38 > 50
  EXPECT_THROW(mold::packetize(feed_of(2), opt), mold::PacketizeError);
  std::vector<std::byte> broken = feed_of(2);
  broken.pop_back();
  EXPECT_THROW(mold::packetize(broken, mold::PacketizeOptions{}), mold::PacketizeError);
  opt = mold::PacketizeOptions{};
  opt.first_sequence = 0;
  EXPECT_THROW(mold::packetize(feed_of(2), opt), std::invalid_argument);
}

}  // namespace
