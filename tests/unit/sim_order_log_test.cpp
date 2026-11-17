#include <gtest/gtest.h>

#include <limits>
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
