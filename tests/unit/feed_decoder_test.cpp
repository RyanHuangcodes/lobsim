// The feed parser, the book rebuilt from the feed, and the round trip (upgrade 6, F3).
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "feed/decoder.hpp"
#include "feed/encoder.hpp"
#include "feed/feed_book.hpp"
#include "feed/itch.hpp"
#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"

namespace {

using feed::MsgA;
using feed::MsgD;
using feed::MsgE;
using feed::MsgS;
using feed::MsgX;
using feed::ParseError;

feed::Prefix at(std::uint64_t ns) { return feed::Prefix{feed::kLocate, 0, feed::kOpenNs + ns}; }

// A feed built message by message.
struct Feed {
  std::vector<std::byte> bytes;
  template <class Msg>
  Feed& add(const Msg& m) {
    std::array<std::byte, feed::kMaxFramed> tmp{};
    const std::size_t n = feed::write(tmp.data(), m);
    bytes.insert(bytes.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n));
    return *this;
  }
  Feed& raw(std::initializer_list<int> v) {
    for (const int b : v) bytes.push_back(static_cast<std::byte>(b));
    return *this;
  }
  std::span<const std::byte> span() const { return bytes; }
};

MsgA add(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t ticks) {
  return MsgA{at(0), ref, side, shares, feed::kStock, ticks * 100};
}

// Counts what it is given.
struct Counter {
  int s = 0, a = 0, e = 0, x = 0, d = 0, unknown = 0;
  bool on(const MsgS&) { return ++s, true; }
  bool on(const MsgA&) { return ++a, true; }
  bool on(const MsgE&) { return ++e, true; }
  bool on(const MsgX&) { return ++x, true; }
  bool on(const MsgD&) { return ++d, true; }
  bool on_unknown(std::uint8_t, std::span<const std::byte>) { return ++unknown, true; }
};

// ---------------------------------------------------------------- parser

TEST(FeedParse, EveryTypeIsDispatched) {
  Feed f;
  f.add(MsgS{at(0), 'O'}).add(add(1, 'B', 5, 100)).add(MsgE{at(1), 1, 2, 1}).add(MsgX{at(2), 1, 1}).add(MsgD{at(3), 1});
  f.add(MsgS{at(4), 'C'});
  Counter c;
  const feed::ParseResult r = feed::parse(f.span(), c);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.messages, 6u);
  EXPECT_EQ(r.offset, f.bytes.size());
  EXPECT_EQ(c.s, 2);
  EXPECT_EQ(c.a, 1);
  EXPECT_EQ(c.e, 1);
  EXPECT_EQ(c.x, 1);
  EXPECT_EQ(c.d, 1);
  EXPECT_EQ(r.by_type['S'], 2u);
}

TEST(FeedParse, EmptyInputParses) {
  Counter c;
  const feed::ParseResult r = feed::parse(std::span<const std::byte>{}, c);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.messages, 0u);
}

TEST(FeedParse, TruncatedLengthIsAnErrorAtItsOffset) {
  Feed f;
  f.add(MsgS{at(0), 'O'}).raw({0x00});
  Counter c;
  const feed::ParseResult r = feed::parse(f.span(), c);
  EXPECT_EQ(r.error, ParseError::TruncatedLength);
  EXPECT_EQ(r.offset, 14u);
  EXPECT_EQ(r.messages, 1u);
}

TEST(FeedParse, TruncatedBodyIsAnError) {
  Feed f;
  f.add(add(1, 'B', 5, 100));
  f.bytes.pop_back();  // 37 of 38 bytes
  Counter c;
  const feed::ParseResult r = feed::parse(f.span(), c);
  EXPECT_EQ(r.error, ParseError::TruncatedBody);
  EXPECT_EQ(r.offset, 0u);
  EXPECT_EQ(c.a, 0);  // nothing is read past the buffer
}

TEST(FeedParse, AddWithLength35IsMalformed) {
  Feed f;
  f.add(MsgS{at(0), 'O'});
  std::vector<std::byte> a(feed::kMaxFramed);
  feed::write(a.data(), add(1, 'B', 5, 100));
  a[1] = std::byte{35};  // the length prefix says 35; the body is the first 35 bytes
  f.bytes.insert(f.bytes.end(), a.begin(), a.begin() + 2 + 35);
  Counter c;
  const feed::ParseResult r = feed::parse(f.span(), c);
  EXPECT_EQ(r.error, ParseError::MalformedLength);
  EXPECT_EQ(r.offset, 14u);
  EXPECT_EQ(c.a, 0);
}

TEST(FeedParse, LengthZeroIsAnError) {
  Feed f;
  f.raw({0x00, 0x00});
  Counter c;
  EXPECT_EQ(feed::parse(f.span(), c).error, ParseError::EmptyBody);
}

TEST(FeedParse, UnknownTypeIsSkippedAndCounted) {
  Feed f;
  f.add(MsgS{at(0), 'O'});
  // A Replace (U): 35 bytes in ITCH 5.0. This feed does not use it: skipped by its length.
  f.raw({0x00, 0x23, 'U'});
  for (int i = 0; i < 34; ++i) f.raw({i});
  f.add(MsgD{at(1), 9});
  Counter c;
  const feed::ParseResult r = feed::parse(f.span(), c);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.unknown, 1u);
  EXPECT_EQ(r.by_type['U'], 1u);
  EXPECT_EQ(c.unknown, 1);
  EXPECT_EQ(c.d, 1);
}

TEST(FeedParse, HandlerCanStop) {
  struct StopAtA : Counter {
    using Counter::on;
    bool on(const MsgA&) { return false; }
  } h;
  Feed f;
  f.add(MsgS{at(0), 'O'}).add(add(1, 'B', 5, 100)).add(MsgD{at(1), 1});
  const feed::ParseResult r = feed::parse(f.span(), h);
  EXPECT_EQ(r.error, ParseError::Stopped);
  EXPECT_EQ(r.offset, 14u);
  EXPECT_EQ(h.d, 0);
}

// ---------------------------------------------------------------- FeedBook

TEST(FeedBook, AppliesEveryMessageType) {
  Feed f;
  f.add(MsgS{at(0), 'O'}).add(add(1, 'B', 5, 100)).add(add(2, 'S', 4, 102)).add(add(3, 'B', 2, 100));
  f.add(MsgE{at(1), 1, 2, 1}).add(MsgX{at(2), 2, 1}).add(MsgE{at(3), 3, 2, 2}).add(MsgS{at(4), 'C'});
  feed::FeedBook fb;
  EXPECT_TRUE(feed::parse(f.span(), fb).ok()) << fb.error();
  EXPECT_TRUE(fb.finish()) << fb.error();
  EXPECT_EQ(fb.book().best_bid(), lob::Price{100});
  EXPECT_EQ(fb.book().depth(lob::Side::Buy, 5), (std::vector<lob::LevelView>{{100, 3}}));
  EXPECT_EQ(fb.book().depth(lob::Side::Sell, 5), (std::vector<lob::LevelView>{{102, 3}}));
  EXPECT_EQ(fb.book().resting_orders(), 2u);  // order 3 was executed in full and is gone
  EXPECT_EQ(fb.executed_shares(), 4u);
}

TEST(FeedBook, ExecutionOnAnUnknownRefFails) {
  Feed f;
  f.add(MsgS{at(0), 'O'}).add(add(1, 'B', 5, 100)).add(MsgE{at(1), 2, 1, 1});
  feed::FeedBook fb;
  const feed::ParseResult r = feed::parse(f.span(), fb);
  EXPECT_EQ(r.error, ParseError::Stopped);
  EXPECT_EQ(fb.messages(), 3u);
  EXPECT_NE(fb.error().find("message 3 (type E)"), std::string::npos) << fb.error();
  EXPECT_NE(fb.error().find("order ref 2 is not resting"), std::string::npos) << fb.error();
}

TEST(FeedBook, AddThatCrossesFails) {
  Feed f;
  f.add(MsgS{at(0), 'O'}).add(add(1, 'S', 5, 100)).add(add(2, 'B', 3, 100));
  feed::FeedBook fb;
  EXPECT_EQ(feed::parse(f.span(), fb).error, ParseError::Stopped);
  EXPECT_NE(fb.error().find("message 3 (type A)"), std::string::npos) << fb.error();
  EXPECT_NE(fb.error().find("would cross"), std::string::npos) << fb.error();
}

TEST(FeedBook, CancelLargerThanRemainingFails) {
  Feed f;
  f.add(MsgS{at(0), 'O'}).add(add(1, 'B', 5, 100)).add(MsgX{at(1), 1, 5});
  feed::FeedBook fb;
  EXPECT_EQ(feed::parse(f.span(), fb).error, ParseError::Stopped);
  EXPECT_NE(fb.error().find("message 3 (type X)"), std::string::npos) << fb.error();
}

TEST(FeedBook, ExecutionLargerThanRemainingFails) {
  Feed f;
  f.add(MsgS{at(0), 'O'}).add(add(1, 'B', 5, 100)).add(MsgE{at(1), 1, 6, 1});
  feed::FeedBook fb;
  EXPECT_EQ(feed::parse(f.span(), fb).error, ParseError::Stopped);
  EXPECT_NE(fb.error().find("message 3 (type E)"), std::string::npos) << fb.error();
}

TEST(FeedBook, OtherFailures) {
  const auto fails = [](const Feed& f, const char* what) {
    feed::FeedBook fb;
    EXPECT_EQ(feed::parse(f.span(), fb).error, ParseError::Stopped) << what;
    return fb.error();
  };
  Feed before;
  before.add(add(1, 'B', 5, 100));
  EXPECT_NE(fails(before, "no S O").find("before the start"), std::string::npos);
  Feed after;
  after.add(MsgS{at(0), 'O'}).add(MsgS{at(1), 'C'}).add(add(1, 'B', 5, 100));
  EXPECT_NE(fails(after, "after S C").find("after the end"), std::string::npos);
  Feed dup;
  dup.add(MsgS{at(0), 'O'}).add(add(1, 'B', 5, 100)).add(add(1, 'B', 5, 99));
  EXPECT_NE(fails(dup, "dup ref").find("already resting"), std::string::npos);
  Feed cents;
  cents.add(MsgS{at(0), 'O'}).add(MsgA{at(0), 1, 'B', 5, feed::kStock, 10'050});
  EXPECT_NE(fails(cents, "sub-penny").find("whole number of cents"), std::string::npos);
  Feed del;
  del.add(MsgS{at(0), 'O'}).add(MsgD{at(0), 4});
  EXPECT_NE(fails(del, "unknown D").find("not resting"), std::string::npos);

  feed::FeedBook open_only;
  Feed o;
  o.add(MsgS{at(0), 'O'});
  EXPECT_TRUE(feed::parse(o.span(), open_only).ok());
  EXPECT_FALSE(open_only.finish());
  EXPECT_NE(open_only.error().find("without the end-of-messages"), std::string::npos);
}

TEST(FeedBook, DigestIgnoresOrderIdsButNotFifoOrder) {
  lob::OrderBook a, b, c;
  a.insert_passive(lob::Side::Buy, 100, 3);
  a.insert_passive(lob::Side::Buy, 100, 4);
  b.insert_passive(lob::Side::Sell, 200, 1);  // shifts b's ids by one
  b.remove(1);
  b.insert_passive(lob::Side::Buy, 100, 3);
  b.insert_passive(lob::Side::Buy, 100, 4);
  c.insert_passive(lob::Side::Buy, 100, 4);  // same level totals, other FIFO order
  c.insert_passive(lob::Side::Buy, 100, 3);
  EXPECT_EQ(feed::book_digest(a), feed::book_digest(b));
  EXPECT_NE(feed::book_digest(a), feed::book_digest(c));
  lob::OrderBook empty, bid_only;
  bid_only.insert_passive(lob::Side::Buy, 100, 3);
  EXPECT_NE(feed::book_digest(empty), feed::book_digest(bid_only));
}

// ---------------------------------------------------------------- round trip and engine agreement

TEST(FeedRoundTrip, ParseThenEncodeIsByteIdentical) {
  Feed f;
  f.add(MsgS{at(0), 'O'}).add(add(1, 'B', 5, 100)).add(MsgE{at(1), 1, 2, 1}).add(MsgX{at(2), 1, 1});
  f.raw({0x00, 0x03, 'Z', 0x01, 0x02});  // an unknown type is copied as it is
  f.add(MsgD{feed::Prefix{7, 9, 123}, 1}).add(MsgS{at(4), 'C'});  // any locate, tracking and time
  feed::Reencoder re;
  ASSERT_TRUE(feed::parse(f.span(), re).ok());
  EXPECT_EQ(re.out, f.bytes);
}

TEST(FeedRoundTrip, EngineBookEqualsFeedBook) {
  // A short hand-made flow covering every mapping rule; the rebuilt book must equal the engine's.
  using lob::NewOrder;
  using lob::OrderType;
  using lob::Side;
  using lob::STP;
  using lob::TIF;
  const auto lim = [](lob::OwnerId o, Side s, lob::Price p, lob::Qty q, TIF t = TIF::GTC, STP stp = STP::CancelTaking) {
    return lob::Request{NewOrder{o, 0, s, OrderType::Limit, t, p, q, false, stp}};
  };
  const std::vector<lob::Request> flow = {
      lim(1, Side::Sell, 101, 3), lim(1, Side::Sell, 102, 4), lim(2, Side::Buy, 99, 5), lim(2, Side::Buy, 98, 2),
      lim(3, Side::Buy, 101, 5),                                   // takes 3, rests 2 at 101
      lob::Request{lob::Modify{2, 3, 99, 3}},                      // same-price reduce
      lob::Request{lob::Modify{2, 4, 102, 6}},                     // crossing modify: takes 4, rests 2
      lob::Request{lob::Cancel{3, 5}},                             // user cancel
      lim(2, Side::Sell, 98, 10, TIF::IOC),                        // sweeps the bids
      lim(1, Side::Sell, 105, 1), lim(1, Side::Buy, 105, 2, TIF::GTC, STP::CancelResting),
      lob::Request{NewOrder{4, 0, Side::Sell, OrderType::Market, TIF::IOC, 0, 1, false, STP::CancelTaking}},
  };
  lob::OrderBook engine;
  feed::Encoder enc;
  feed::FeedBook fb;
  lob::EventBuffer ev;
  enc.start(0);
  std::int64_t t = 0;
  for (const lob::Request& r : flow) {
    ev.clear();
    lob::apply(engine, r, ev);
    enc.on_request(++t, r, ev);
  }
  enc.finish(++t);
  ASSERT_TRUE(feed::parse(enc.buffered(), fb).ok()) << fb.error();
  ASSERT_TRUE(fb.finish()) << fb.error();
  EXPECT_EQ(feed::book_digest(fb.book()), feed::book_digest(engine));
  EXPECT_EQ(fb.book().depth(lob::Side::Buy, 10), engine.depth(lob::Side::Buy, 10));
  EXPECT_EQ(fb.book().depth(lob::Side::Sell, 10), engine.depth(lob::Side::Sell, 10));
  EXPECT_GT(enc.counts().E, 0u);
  EXPECT_GT(enc.counts().X, 0u);
  EXPECT_GT(enc.counts().D, 0u);
}

}  // namespace
