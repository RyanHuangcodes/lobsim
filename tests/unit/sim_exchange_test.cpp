#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <variant>

#include "sim/exchange.hpp"
#include "sim/order_log.hpp"
#include "support/sim_support.hpp"

using namespace sim;
using simtest::limit;
using simtest::market;

namespace {

std::size_t count_bbo(const ExchangeOutput& o) {
  std::size_t n = 0;
  for (const MarketData& m : o.public_data) n += std::holds_alternative<BBO>(m) ? 1 : 0;
  return n;
}

}  // namespace

TEST(Exchange, OwnerIsForcedToTheSender) {
  std::ostringstream log;
  Exchange ex(1000, &log);
  const ExchangeOutput& out = ex.process(10, /*sender=*/3, limit(lob::Side::Buy, 100, 5));  // owner field 0
  ASSERT_FALSE(out.events.empty());
  const auto& ack = std::get<lob::Ack>(out.events[0]);
  EXPECT_EQ(ack.owner, 3u);
  EXPECT_EQ(ex.book().order(ack.order_id)->owner, 3u);

  lob::NewOrder lie = limit(lob::Side::Buy, 99, 1, 2);
  lie.owner = 77;
  const ExchangeOutput& out2 = ex.process(20, 4, lie);
  EXPECT_EQ(std::get<lob::Ack>(out2.events[0]).owner, 4u);
  // The log shows the forced owner.
  EXPECT_NE(log.str().find("20 N 4 2 "), std::string::npos) << log.str();
  EXPECT_EQ(log.str().find(" 77 "), std::string::npos);
  // A cancel by another sender is refused, whatever its owner field says.
  const ExchangeOutput& out3 = ex.process(30, 5, lob::Cancel{3, std::get<lob::Ack>(out.events[0]).order_id});
  const auto& rej = std::get<lob::Reject>(out3.events[0]);
  EXPECT_EQ(rej.reason, lob::RejectReason::NotOwner);
  EXPECT_EQ(rej.owner, 5u);
}

TEST(Exchange, EveryRequestIsLoggedIncludingRejects) {
  std::ostringstream log;
  Exchange ex(1000, &log);
  ex.process(1, 1, limit(lob::Side::Sell, 101, 5));
  ex.process(2, 2, limit(lob::Side::Buy, 100, 0));  // InvalidQty reject
  ex.process(3, 2, lob::Cancel{2, 999});            // UnknownOrder reject
  ex.process(4, 2, lob::Modify{2, 999, 5, 5});
  EXPECT_EQ(ex.requests(), 4u);

  std::istringstream in(log.str());
  std::string line;
  std::getline(in, line);
  EXPECT_EQ(parse_header(line), 1000);
  int n = 0;
  while (std::getline(in, line)) {
    const auto p = parse_request(line);
    ASSERT_TRUE(p) << line;
    EXPECT_EQ(p->first, ++n);
  }
  EXPECT_EQ(n, 4);
}

TEST(Exchange, NullLogIsAllowed) {
  Exchange ex(1000, nullptr);
  EXPECT_NO_THROW(ex.process(1, 1, limit(lob::Side::Buy, 10, 1)));
}

TEST(Exchange, OneTradePrintPerTradeThenAtMostOneBbo) {
  Exchange ex(1000, nullptr);
  ex.process(1, 1, limit(lob::Side::Sell, 101, 3, 1));
  ex.process(2, 1, limit(lob::Side::Sell, 101, 4, 2));
  ex.process(3, 1, limit(lob::Side::Sell, 102, 5, 3));
  ex.process(4, 2, limit(lob::Side::Buy, 99, 1, 1));

  // A buy of 9 @ 102 trades 3 + 4 at 101 and 2 at 102: three trades.
  const ExchangeOutput& out = ex.process(5, 2, limit(lob::Side::Buy, 102, 9, 2));
  ASSERT_EQ(out.public_data.size(), 4u);
  const TradePrint expect[3] = {{101, 3, lob::Side::Buy}, {101, 4, lob::Side::Buy}, {102, 2, lob::Side::Buy}};
  for (int i = 0; i < 3; ++i) EXPECT_EQ(std::get<TradePrint>(out.public_data[static_cast<std::size_t>(i)]), expect[i]);
  ASSERT_TRUE(std::holds_alternative<BBO>(out.public_data[3]));
  EXPECT_EQ(std::get<BBO>(out.public_data[3]), (BBO{99, 1, 102, 3}));
  EXPECT_EQ(out.top, (BBO{99, 1, 102, 3}));
  EXPECT_TRUE(out.top_changed);
  EXPECT_EQ(ex.trades(), 3u);
}

TEST(Exchange, BboOnlyWhenOneOfTheFourFieldsChanged) {
  Exchange ex(1000, nullptr);
  const ExchangeOutput& a = ex.process(1, 1, limit(lob::Side::Sell, 101, 5, 1));
  EXPECT_EQ(count_bbo(a), 1u) << "first ask appears";
  EXPECT_EQ(std::get<BBO>(a.public_data.back()), (BBO{0, 0, 101, 5}));
  ex.process(2, 1, limit(lob::Side::Buy, 99, 4, 2));

  // Only the quantity at the best price changes.
  const ExchangeOutput& q = ex.process(3, 2, limit(lob::Side::Sell, 101, 2, 1));
  EXPECT_TRUE(q.top_changed);
  EXPECT_EQ(count_bbo(q), 1u);
  EXPECT_EQ(q.top, (BBO{99, 4, 101, 7}));

  // Behind the top on both sides, an invalid request and a rejected cancel: nothing changes.
  const ExchangeOutput& deep = ex.process(4, 2, limit(lob::Side::Sell, 105, 5, 2));
  EXPECT_FALSE(deep.top_changed);
  EXPECT_EQ(count_bbo(deep), 0u);
  EXPECT_TRUE(deep.public_data.empty());
  EXPECT_FALSE(ex.process(5, 2, limit(lob::Side::Buy, 90, 1, 3)).top_changed);
  EXPECT_FALSE(ex.process(6, 2, limit(lob::Side::Buy, 90, 0, 4)).top_changed);
  EXPECT_FALSE(ex.process(7, 2, lob::Cancel{2, 424242}).top_changed);

  // A price change at the best bid.
  const ExchangeOutput& better = ex.process(8, 2, limit(lob::Side::Buy, 100, 1, 5));
  EXPECT_EQ(count_bbo(better), 1u);
  EXPECT_EQ(better.top, (BBO{100, 1, 101, 7}));
}

TEST(Exchange, TradePrintsComeBeforeTheBbo) {
  // A trade always consumes quantity at the best level, so the top changes with it; the
  // prints still come first and the BBO last.
  Exchange ex(1000, nullptr);
  ex.process(1, 1, limit(lob::Side::Sell, 101, 5, 1));
  const ExchangeOutput& out = ex.process(2, 2, market(lob::Side::Buy, 2, 1));
  ASSERT_EQ(out.public_data.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<TradePrint>(out.public_data[0]));
  EXPECT_TRUE(std::holds_alternative<BBO>(out.public_data[1]));
  EXPECT_EQ(std::get<BBO>(out.public_data[1]).ask_qty, 3);
}

TEST(Exchange, LastMidTracksTheLastTwoSidedBook) {
  Exchange ex(1000, nullptr);
  EXPECT_FALSE(ex.last_mid());
  ex.process(1, 1, limit(lob::Side::Buy, 100, 1, 1));
  EXPECT_FALSE(ex.last_mid());
  ex.process(2, 1, limit(lob::Side::Sell, 103, 1, 2));
  EXPECT_DOUBLE_EQ(*ex.last_mid(), 101.5);
  ex.process(3, 2, market(lob::Side::Buy, 1, 1));  // ask side empties
  EXPECT_DOUBLE_EQ(*ex.last_mid(), 101.5) << "kept while a side is empty";
}

TEST(Exchange, HashEqualsHashOfTheEngineEvents) {
  Exchange ex(1000, nullptr);
  lob::EventHasher h;
  const lob::Request reqs[] = {limit(lob::Side::Sell, 101, 5, 1), limit(lob::Side::Buy, 101, 8, 1),
                               lob::Cancel{1, 2}};
  TimeNs t = 0;
  for (const lob::Request& r : reqs) {
    ++t;
    const ExchangeOutput& out = ex.process(t, 1 + static_cast<lob::OwnerId>(t % 2), r);
    h.add(out.events);
  }
  EXPECT_EQ(ex.event_hash(), h.value());
}
