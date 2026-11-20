#include <gtest/gtest.h>

#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "sim/order_log.hpp"

using namespace sim;

TEST(OrderLog, HeaderRoundTrip) {
  EXPECT_EQ(format_header(1'000'000), "# lobsim orders.log v1 max_price=1000000");
  EXPECT_EQ(parse_header(format_header(77)), 77);
  EXPECT_FALSE(parse_header("# lobsim orders.log v2 max_price=5"));
  EXPECT_FALSE(parse_header("# lobsim orders.log v1 max_price="));
  EXPECT_FALSE(parse_header("# lobsim orders.log v1 max_price=5x"));
  EXPECT_FALSE(parse_header(""));
}

TEST(OrderLog, EveryNewOrderEnumCombinationRoundTrips) {
  int n = 0;
  for (auto side : {lob::Side::Buy, lob::Side::Sell})
    for (auto type : {lob::OrderType::Limit, lob::OrderType::Market})
      for (auto tif : {lob::TIF::GTC, lob::TIF::IOC, lob::TIF::FOK})
        for (bool po : {false, true})
          for (auto stp : {lob::STP::CancelTaking, lob::STP::CancelResting}) {
            lob::NewOrder o{7, 123456789012ULL, side, type, tif, -5, -9, po, stp};
            const std::string line = format_request(987654321, o);
            const auto parsed = parse_request(line);
            ASSERT_TRUE(parsed) << line;
            EXPECT_EQ(parsed->first, 987654321);
            EXPECT_EQ(std::get<lob::NewOrder>(parsed->second), o) << line;
            ++n;
          }
  EXPECT_EQ(n, 48);
}

TEST(OrderLog, ExtremeValuesRoundTrip) {
  constexpr auto imax = std::numeric_limits<std::int64_t>::max();
  constexpr auto imin = std::numeric_limits<std::int64_t>::min();
  lob::NewOrder o{0xFFFFFFFFu, ~0ULL, lob::Side::Sell, lob::OrderType::Limit, lob::TIF::FOK, imin, imax, true,
                  lob::STP::CancelResting};
  const auto p = parse_request(format_request(imax, o));
  ASSERT_TRUE(p);
  EXPECT_EQ(p->first, imax);
  EXPECT_EQ(std::get<lob::NewOrder>(p->second), o);
}

TEST(OrderLog, CancelAndModifyRoundTrip) {
  const lob::Cancel c{3, 99};
  const auto pc = parse_request(format_request(5, c));
  ASSERT_TRUE(pc);
  EXPECT_EQ(std::get<lob::Cancel>(pc->second), c);

  for (lob::Price price : {lob::Price{-4}, lob::Price{0}, lob::Price{10'000}})
    for (lob::Qty qty : {lob::Qty{-1}, lob::Qty{0}, lob::Qty{12}}) {
      const lob::Modify m{4, 17, price, qty};
      const auto pm = parse_request(format_request(0, m));
      ASSERT_TRUE(pm);
      EXPECT_EQ(std::get<lob::Modify>(pm->second), m);
    }
}

TEST(OrderLog, LineFormatIsPlainIntegers) {
  const lob::NewOrder o{1, 2, lob::Side::Sell, lob::OrderType::Market, lob::TIF::IOC, 0, 5, false,
                        lob::STP::CancelTaking};
  EXPECT_EQ(format_request(100, o), "100 N 1 2 1 1 1 0 5 0 0");
  EXPECT_EQ(format_request(7, lob::Cancel{2, 9}), "7 C 2 9");
  EXPECT_EQ(format_request(8, lob::Modify{2, 9, 10, 3}), "8 M 2 9 10 3");
}

TEST(OrderLog, MalformedLinesAreRejected) {
  const std::vector<std::string> bad = {
      "",
      "N 1 2 0 0 0 5 5 0 0",
      "-5 C 1 2",
      "10 X 1 2",
      "10 NN 1 2",
      "10 N 1 2 0 0 0 5 5 0",        // too few fields
      "10 N 1 2 0 0 0 5 5 0 0 0",    // too many
      "10 N 1 2 2 0 0 5 5 0 0",      // side out of range
      "10 N 1 2 0 2 0 5 5 0 0",      // type out of range
      "10 N 1 2 0 0 3 5 5 0 0",      // tif out of range
      "10 N 1 2 0 0 0 5 5 2 0",      // post_only out of range
      "10 N 1 2 0 0 0 5 5 0 2",      // stp out of range
      "10 N 1 2 0 0 0 5 five 0 0",   // not a number
      "10 N 1 2 0 0 0 5 5 0 0 ",     // trailing space
      "10  C 1 2",                   // double space
      "10 C 1",                      // too few
      "10 C 1 2 3",                  // too many
      "10 C -1 2",                   // negative owner
      "10 M 1 2 3",                  // too few
      "10 M 1 2 3 4 5",              // too many
      "10 C 1 2x",
      "99999999999999999999 C 1 2",  // overflow
  };
  for (const std::string& line : bad) EXPECT_FALSE(parse_request(line)) << "accepted: '" << line << "'";
}

namespace {

// Expects parse_order_log(text) to throw a ReplayError on `line`.
void expect_parse_error(const std::string& text, std::size_t line) {
  try {
    parse_order_log(text);
    FAIL() << "expected a ReplayError";
  } catch (const ReplayError& e) {
    EXPECT_EQ(e.line(), line) << e.what();
  }
}

std::string header_line() { return format_header(1000) + "\n"; }

}  // namespace

TEST(OrderLog, WholeLogParseFormatRoundTrip) {
  const std::string text = header_line() +
                           "10 N 1 1 0 0 0 100 5 0 0\n"
                           "10 N 2 1 1 0 1 100 3 1 1\n"
                           "20 C 1 1\n"
                           "30 M 2 2 101 4\n"
                           "30 N 3 7 1 1 2 0 9 0 0\n";
  const OrderLog log = parse_order_log(text);
  EXPECT_EQ(log.max_price, 1000);
  ASSERT_EQ(log.requests.size(), 5u);
  EXPECT_EQ(log.requests[2].time, 20);
  EXPECT_EQ(std::get<lob::Cancel>(log.requests[2].request), (lob::Cancel{1, 1}));
  EXPECT_EQ(std::get<lob::Modify>(log.requests[3].request), (lob::Modify{2, 2, 101, 4}));
  EXPECT_EQ(format_order_log(log), text);
  EXPECT_EQ(parse_order_log(format_order_log(log)), log);
}

TEST(OrderLog, HeaderOnlyLogHasNoRequests) {
  const OrderLog log = parse_order_log(header_line());
  EXPECT_TRUE(log.requests.empty());
  EXPECT_EQ(format_order_log(log), header_line());
  EXPECT_TRUE(parse_order_log(format_header(1000)).requests.empty());  // no trailing newline
}

TEST(OrderLog, LastLineWithoutNewlineIsAccepted) {
  const OrderLog log = parse_order_log(header_line() + "5 C 1 2");
  ASSERT_EQ(log.requests.size(), 1u);
  EXPECT_EQ(format_order_log(log), header_line() + "5 C 1 2\n");
}

TEST(OrderLog, EmptyInputIsAnErrorOnLine1) { expect_parse_error("", 1); }

TEST(OrderLog, BadHeaderIsAnErrorOnLine1) {
  expect_parse_error("not a header\n", 1);
  expect_parse_error("# lobsim orders.log v1 max_price=5\r\n", 1);
  expect_parse_error(std::string("# lobsim orders.log v1 max_price=5\0", 35) + "\n", 1);
}

TEST(OrderLog, MalformedRequestIsAnErrorNamingItsLine) {
  expect_parse_error(header_line() + "1 C 1 2\n2 C 1\n", 3);
  expect_parse_error(header_line() + "\n", 2);  // an empty request line
}

TEST(OrderLog, TimeGoingBackwardsIsAnErrorNamingItsLine) {
  expect_parse_error(header_line() + "10 C 1 1\n30 C 1 2\n20 C 1 3\n", 4);
  EXPECT_EQ(parse_order_log(header_line() + "10 C 1 1\n10 C 1 2\n").requests.size(), 2u);
}

TEST(OrderLog, CarriageReturnAtTheEndOfARequestLineIsAnError) {
  expect_parse_error(header_line() + "1 C 1 2\r\n", 2);
  expect_parse_error(header_line() + "1 C 1 2\n2 C 1 2\r", 3);
}

TEST(OrderLog, EmbeddedNulIsAnError) {
  expect_parse_error(header_line() + std::string("1 C 1\0 2\n", 9), 2);
  expect_parse_error(header_line() + std::string("1 C 1 2\0\n", 9), 2);
}

TEST(OrderLog, VeryLongLineIsRejectedWithoutTrouble) {
  expect_parse_error(header_line() + "1 C 1 " + std::string(1'000'000, '9') + "\n", 2);
  expect_parse_error(header_line() + std::string(1'000'000, ' ') + "\n", 2);
}

TEST(OrderLog, ReaderReturnsRequestsThenNullopt) {
  std::istringstream in(header_line() + "1 C 1 2\n");
  OrderLogReader reader(in);
  EXPECT_EQ(reader.max_price(), 1000);
  const auto first = reader.next();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->time, 1);
  EXPECT_FALSE(reader.next());
  EXPECT_FALSE(reader.next());
}
