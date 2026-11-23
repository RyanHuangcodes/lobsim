// The feed encoder (upgrade 6, F2): one hand-checkable scenario per mapping rule of FEED.md,
// each on a fresh OrderBook, and the overflow checks.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "feed/encoder.hpp"
#include "feed/itch.hpp"
#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"

namespace {

using lob::Cancel;
using lob::Modify;
using lob::NewOrder;
using lob::OrderType;
using lob::Side;
using lob::STP;
using lob::TIF;

// One decoded message: enough fields to state every scenario in one line.
struct Msg {
  char type{0};
  std::uint64_t ref{0};
  std::uint32_t shares{0};
  std::uint32_t price{0};  // A only, Price(4)
  char side{0};            // A only
  std::uint64_t match{0};  // E only
  bool operator==(const Msg&) const = default;
};

std::ostream& operator<<(std::ostream& os, const Msg& m) {
  os << m.type << "{ref " << m.ref;
  if (m.type == 'A') os << ", " << m.side << " " << m.shares << " @ " << m.price;
  if (m.type == 'E') os << ", " << m.shares << " match " << m.match;
  if (m.type == 'X') os << ", " << m.shares;
  return os << "}";
}

Msg A(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t ticks) {
  return Msg{'A', ref, shares, ticks * 100, side, 0};
}
Msg E(std::uint64_t ref, std::uint32_t shares, std::uint64_t match) { return Msg{'E', ref, shares, 0, 0, match}; }
Msg X(std::uint64_t ref, std::uint32_t shares) { return Msg{'X', ref, shares, 0, 0, 0}; }
Msg D(std::uint64_t ref) { return Msg{'D', ref, 0, 0, 0, 0}; }

// Walks framed messages written by the encoder (the parser is tested separately).
std::vector<Msg> decode(std::span<const std::byte> buf) {
  std::vector<Msg> out;
  std::size_t at = 0;
  while (at < buf.size()) {
    const auto len = feed::be::load<std::uint16_t>(buf.data() + at);
    const std::byte* b = buf.data() + at + feed::kPrefix;
    const char type = static_cast<char>(b[0]);
    EXPECT_EQ(len, feed::kBodyLen[static_cast<unsigned char>(type)]) << "type " << type;
    switch (type) {
      case 'A': {
        const feed::MsgA m = feed::read_A(b);
        out.push_back(Msg{'A', m.ref, m.shares, m.price, m.side, 0});
        break;
      }
      case 'E': {
        const feed::MsgE m = feed::read_E(b);
        out.push_back(Msg{'E', m.ref, m.shares, 0, 0, m.match});
        break;
      }
      case 'X': {
        const feed::MsgX m = feed::read_X(b);
        out.push_back(Msg{'X', m.ref, m.shares, 0, 0, 0});
        break;
      }
      case 'D': out.push_back(Msg{'D', feed::read_D(b).ref, 0, 0, 0, 0}); break;
      case 'S': out.push_back(Msg{'S', 0, 0, 0, feed::read_S(b).event_code, 0}); break;
      default: ADD_FAILURE() << "unexpected type " << type;
    }
    at += feed::kPrefix + len;
  }
  return out;
}

NewOrder limit(lob::OwnerId owner, Side side, lob::Price price, lob::Qty qty, TIF tif = TIF::GTC,
               STP stp = STP::CancelTaking) {
  return NewOrder{owner, 0, side, OrderType::Limit, tif, price, qty, false, stp};
}
NewOrder market(lob::OwnerId owner, Side side, lob::Qty qty) {
  return NewOrder{owner, 0, side, OrderType::Market, TIF::IOC, 0, qty, false, STP::CancelTaking};
}

class FeedEncoder : public ::testing::Test {
 protected:
  // The messages of one request.
  std::vector<Msg> send(const lob::Request& req, std::int64_t t = 0) {
    events_.clear();
    lob::apply(book_, req, events_);
    enc_.clear();
    enc_.on_request(t, req, events_);
    return decode(enc_.buffered());
  }

  lob::OrderBook book_{lob::BookConfig{50'000'000}};
  feed::Encoder enc_;
  lob::EventBuffer events_;
};

TEST_F(FeedEncoder, RestingLimitGivesAdd) {
  EXPECT_EQ(send(limit(1, Side::Buy, 100, 5)), (std::vector<Msg>{A(1, 'B', 5, 100)}));
  EXPECT_EQ(send(limit(1, Side::Sell, 102, 4)), (std::vector<Msg>{A(2, 'S', 4, 102)}));
}

TEST_F(FeedEncoder, PartialFillThenRestGivesExecutionsThenAdd) {
  send(limit(1, Side::Sell, 100, 3));  // order 1
  send(limit(1, Side::Sell, 101, 2));  // order 2
  // Buy 7 @ 101 from another owner: takes 3 @ 100 and 2 @ 101, then 2 rest at 101.
  EXPECT_EQ(send(limit(2, Side::Buy, 101, 7)), (std::vector<Msg>{E(1, 3, 1), E(2, 2, 2), A(3, 'B', 2, 101)}));
}

TEST_F(FeedEncoder, MarketOrderGivesOnlyTheMakersExecutions) {
  send(limit(1, Side::Sell, 100, 3));
  send(limit(1, Side::Sell, 101, 3));
  EXPECT_EQ(send(market(2, Side::Buy, 4)), (std::vector<Msg>{E(1, 3, 1), E(2, 1, 2)}));
  EXPECT_EQ(send(market(2, Side::Buy, 9)), (std::vector<Msg>{E(2, 2, 3)}));  // the remainder is cancelled: nothing
}

TEST_F(FeedEncoder, FullFillOfAMakerGivesExecutionAndNoDelete) {
  send(limit(1, Side::Buy, 100, 5));
  EXPECT_EQ(send(limit(2, Side::Sell, 100, 5)), (std::vector<Msg>{E(1, 5, 1)}));
  EXPECT_FALSE(book_.best_bid().has_value());
  // The maker is gone from the mirror too: a later cancel of it is a reject, which emits nothing.
  EXPECT_TRUE(send(Cancel{1, 1}).empty());
}

TEST_F(FeedEncoder, SamePriceReduceGivesCancel) {
  send(limit(1, Side::Buy, 100, 5));
  EXPECT_EQ(send(Modify{1, 1, 100, 3}), (std::vector<Msg>{X(1, 2)}));
  EXPECT_EQ(send(Modify{1, 1, 100, 1}), (std::vector<Msg>{X(1, 2)}));
}

TEST_F(FeedEncoder, PriceChangeGivesDeleteThenAdd) {
  send(limit(1, Side::Buy, 100, 5));
  EXPECT_EQ(send(Modify{1, 1, 99, 5}), (std::vector<Msg>{D(1), A(1, 'B', 5, 99)}));
  // A quantity increase also loses priority.
  EXPECT_EQ(send(Modify{1, 1, 99, 8}), (std::vector<Msg>{D(1), A(1, 'B', 8, 99)}));
}

TEST_F(FeedEncoder, CrossingModifyGivesDeleteExecutionThenAdd) {
  send(limit(1, Side::Sell, 101, 2));  // order 1
  send(limit(2, Side::Buy, 99, 5));    // order 2
  // Order 2 moves to 101: it takes the 2 lots of order 1 as a taker and rests 3 at 101.
  EXPECT_EQ(send(Modify{2, 2, 101, 5}), (std::vector<Msg>{D(2), E(1, 2, 1), A(2, 'B', 3, 101)}));
}

TEST_F(FeedEncoder, UserCancelGivesDelete) {
  send(limit(1, Side::Buy, 100, 5));
  EXPECT_EQ(send(Cancel{1, 1}), (std::vector<Msg>{D(1)}));
}

TEST_F(FeedEncoder, SelfTradeCancelRestingGivesDelete) {
  send(limit(1, Side::Sell, 100, 3));  // order 1
  // Same owner, CancelResting: the maker is cancelled and the taker rests in full.
  EXPECT_EQ(send(limit(1, Side::Buy, 100, 4, TIF::GTC, STP::CancelResting)),
            (std::vector<Msg>{D(1), A(2, 'B', 4, 100)}));
}

TEST_F(FeedEncoder, OrdersThatNeverRestGiveNothingForTheTaker) {
  send(limit(1, Side::Sell, 100, 3));  // order 1
  // IOC: takes 3, the remainder is cancelled.
  EXPECT_EQ(send(limit(2, Side::Buy, 100, 5, TIF::IOC)), (std::vector<Msg>{E(1, 3, 1)}));
  send(limit(1, Side::Sell, 100, 3));  // order 3
  // FOK one lot short: nothing at all.
  EXPECT_TRUE(send(limit(2, Side::Buy, 100, 4, TIF::FOK)).empty());
  // Reject (zero quantity): nothing.
  EXPECT_TRUE(send(limit(2, Side::Buy, 100, 0)).empty());
  // Self-trade CancelTaking: the taker is cancelled before it trades; the maker stays.
  EXPECT_TRUE(send(limit(1, Side::Buy, 100, 2)).empty());
  EXPECT_EQ(book_.best_ask(), lob::Price{100});
}

TEST_F(FeedEncoder, StartAndFinishGiveSystemEvents) {
  enc_.start(0);
  enc_.finish(5);
  const std::vector<Msg> got = decode(enc_.buffered());
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].type, 'S');
  EXPECT_EQ(got[0].side, 'O');
  EXPECT_EQ(got[1].side, 'C');
  EXPECT_EQ(enc_.counts().S, 2u);
  EXPECT_EQ(enc_.counts().bytes, 28u);
}

TEST_F(FeedEncoder, TimeOf2Pow48Throws) {
  const auto limit_sim = static_cast<std::int64_t>(feed::kTimestampLimit - feed::kOpenNs);
  EXPECT_NO_THROW(send(limit(1, Side::Buy, 100, 1), limit_sim - 1));
  EXPECT_THROW(send(limit(1, Side::Buy, 100, 1), limit_sim), feed::EncodeError);
  EXPECT_THROW(send(limit(1, Side::Buy, 100, 1), -1), feed::EncodeError);
}

TEST_F(FeedEncoder, PriceAbove32BitsThrows) {
  EXPECT_NO_THROW(send(limit(1, Side::Sell, 42'949'672, 1)));  // 4,294,967,200 fits
  EXPECT_THROW(send(limit(1, Side::Sell, 42'949'673, 1)), feed::EncodeError);
}

TEST_F(FeedEncoder, SharesAbove32BitsThrow) {
  EXPECT_NO_THROW(send(limit(1, Side::Buy, 100, 4'294'967'295LL)));
  EXPECT_THROW(send(limit(1, Side::Buy, 99, 4'294'967'296LL)), feed::EncodeError);
}

TEST_F(FeedEncoder, OverflowErrorNamesTheRequest) {
  send(limit(1, Side::Buy, 100, 1));
  try {
    send(limit(1, Side::Buy, 99, 4'294'967'296LL));
    FAIL() << "no EncodeError";
  } catch (const feed::EncodeError& e) {
    EXPECT_EQ(e.request(), 2u);
    EXPECT_NE(std::string(e.what()).find("request 2"), std::string::npos);
  }
}

}  // namespace
