#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "replay/lobster.hpp"

namespace {

using replay::BookLevel;
using replay::LobsterMsg;

LobsterMsg must_parse(const std::string& line) {
  const auto r = replay::parse_message_line(line);
  EXPECT_TRUE(r.ok()) << line << ": " << r.error;
  return r.value.value_or(LobsterMsg{});
}

void expect_rejected(const std::string& line, const std::string& reason_part) {
  const auto r = replay::parse_message_line(line);
  EXPECT_FALSE(r.ok()) << line;
  EXPECT_NE(r.error.find(reason_part), std::string::npos) << line << ": reason was \"" << r.error << "\"";
}

TEST(LobsterTime, TwelveDigitFractionIsTruncatedAndCounted) {
  const auto r = replay::parse_message_line("34200.123456789012,1,16113575,18,5853300,1");
  ASSERT_TRUE(r.ok()) << r.error;
  EXPECT_EQ(r.value->time_ns, 34'200'123'456'789);
  EXPECT_TRUE(r.time_truncated);
}

TEST(LobsterTime, WholeSecondsAndShortFractions) {
  const auto a = replay::parse_message_line("34200,1,1,1,1,1");
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a.value->time_ns, 34'200'000'000'000);
  EXPECT_FALSE(a.time_truncated);
  const auto b = replay::parse_message_line("34200.5,1,1,1,1,1");
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.value->time_ns, 34'200'500'000'000);
  EXPECT_FALSE(b.time_truncated);
  EXPECT_EQ(must_parse("34200.000000001,1,1,1,1,1").time_ns, 34'200'000'000'001);
  EXPECT_EQ(must_parse("0.0,1,1,1,1,1").time_ns, 0);
}

TEST(LobsterTime, ExactlyNineDigitsIsNotTruncated) {
  const auto r = replay::parse_message_line("34200.123456789,1,1,1,1,1");
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r.time_truncated);
  EXPECT_EQ(r.value->time_ns, 34'200'123'456'789);
}

TEST(LobsterTime, Rejections) {
  expect_rejected(",1,1,1,1,1", "time");
  expect_rejected(".5,1,1,1,1,1", "time");
  expect_rejected("34200.,1,1,1,1,1", "time");
  expect_rejected("34200.5x,1,1,1,1,1", "time");
  expect_rejected("-34200.5,1,1,1,1,1", "time");
  expect_rejected("+34200.5,1,1,1,1,1", "time");
  expect_rejected(" 34200.5,1,1,1,1,1", "time");
  expect_rejected("34200.5.1,1,1,1,1,1", "time");
  expect_rejected("1e3,1,1,1,1,1", "time");
  expect_rejected("9223372037,1,1,1,1,1", "time");
  expect_rejected("99999999999999999999999,1,1,1,1,1", "time");
  EXPECT_EQ(must_parse("9223372036.854775807,1,1,1,1,1").time_ns, INT64_MAX);
  expect_rejected("9223372036.854775808,1,1,1,1,1", "time");
}

TEST(LobsterMessage, WrongColumnCount) {
  expect_rejected("", "6 fields");
  expect_rejected("34200.5,1,1,1,1", "6 fields, found 5");
  expect_rejected("34200.5,1,1,1,1,1,", "6 fields, found 7");
}

TEST(LobsterMessage, EmptyAndNonNumericFields) {
  expect_rejected("34200.5,,1,1,1,1", "type");
  expect_rejected("34200.5,1,,1,1,1", "order id");
  expect_rejected("34200.5,1,1,,1,1", "size");
  expect_rejected("34200.5,1,1,1,,1", "price");
  expect_rejected("34200.5,1,1,1,1,", "direction");
  expect_rejected("34200.5,a,1,1,1,1", "type");
  expect_rejected("34200.5,1,x,1,1,1", "order id");
  expect_rejected("34200.5,1,1,+5,1,1", "size");
  expect_rejected("34200.5,1,1,1, 7,1", "price");
  expect_rejected("34200.5,1,1,1,7.5,1", "price");
  expect_rejected("34200.5,1,1,1,1,+1", "direction");
  expect_rejected("34200.5,1,99999999999999999999,1,1,1", "order id");
}

TEST(LobsterMessage, TypeRange) {
  expect_rejected("34200.5,0,1,1,1,1", "type");
  expect_rejected("34200.5,8,1,1,1,1", "type");
  expect_rejected("34200.5,-1,1,1,1,1", "type");
  for (int t = 1; t <= 7; ++t) EXPECT_EQ(must_parse("34200.5," + std::to_string(t) + ",1,1,1,1").type, t);
}

TEST(LobsterMessage, DirectionRules) {
  expect_rejected("34200.5,4,1,1,1,0", "direction");
  expect_rejected("34200.5,1,1,1,1,0", "direction");
  expect_rejected("34200.5,5,1,1,1,2", "direction");
  expect_rejected("34200.5,7,1,1,1,2", "direction");
  expect_rejected("34200.5,7,1,1,1,-2", "direction");
  EXPECT_EQ(must_parse("34200.5,7,0,0,0,0").direction, 0);
  EXPECT_EQ(must_parse("34200.5,6,0,100,5000,0").direction, 0);
  EXPECT_EQ(must_parse("34200.5,7,0,0,0,-1").direction, -1);
  EXPECT_EQ(must_parse("34200.5,6,0,100,5000,1").direction, 1);
  EXPECT_EQ(must_parse("34200.5,4,1,1,1,-1").direction, -1);
}

TEST(LobsterMessage, SizeAndPriceRules) {
  for (int t = 1; t <= 5; ++t) {
    expect_rejected("34200.5," + std::to_string(t) + ",1,0,5000,1", "size");
    expect_rejected("34200.5," + std::to_string(t) + ",1,-3,5000,1", "size");
  }
  for (int t = 1; t <= 4; ++t) {
    expect_rejected("34200.5," + std::to_string(t) + ",1,1,0,1", "price");
    expect_rejected("34200.5," + std::to_string(t) + ",1,1,-5,1", "price");
  }
  EXPECT_EQ(must_parse("34200.5,5,1,1,0,1").price, 0);  // hidden executions may carry no price
  EXPECT_EQ(must_parse("34200.5,6,0,0,0,0").size, 0);
  EXPECT_EQ(must_parse("34200.5,7,0,-1,-1,0").size, -1);
}

TEST(LobsterMessage, CarriageReturnIsStrippedOnceAndRecorded) {
  const auto r = replay::parse_message_line("34200.5,1,7,100,5000,-1\r");
  ASSERT_TRUE(r.ok()) << r.error;
  EXPECT_TRUE(r.cr_stripped);
  EXPECT_EQ(r.value->direction, -1);
  EXPECT_FALSE(replay::parse_message_line("34200.5,1,7,100,5000,-1").cr_stripped);
  EXPECT_FALSE(replay::parse_message_line("34200.5,1,7,100,5000,-1\r\r").ok());
  EXPECT_FALSE(replay::parse_message_line("\r").ok());
}

TEST(LobsterMessage, FieldsAreRead) {
  const LobsterMsg m = must_parse("34200.004241176,1,16113575,18,5853300,1");
  EXPECT_EQ(m, (LobsterMsg{34'200'004'241'176, 1, 16113575, 18, 5853300, 1}));
}

TEST(LobsterMessage, FormatIsCanonical) {
  EXPECT_EQ(replay::format_message_line({34'200'500'000'000, 1, 7, 100, 5000, -1}), "34200.500000000,1,7,100,5000,-1");
  EXPECT_EQ(replay::format_message_line({0, 7, 0, 0, 0, 0}), "0.000000000,7,0,0,0,0");
  EXPECT_EQ(replay::format_message_line({34'200'000'000'001, 4, 7, 1, 5, 1}), "34200.000000001,4,7,1,5,1");
}

TEST(LobsterMessage, RoundTrip) {
  const std::vector<std::string> lines = {
      "34200.004241176,1,16113575,18,5853300,1",   "34200.025552483,1,16120456,18,5853800,-1",
      "34200.201743788,3,16120456,18,5853800,-1",  "34200.201743788,4,16113575,10,5853300,1",
      "34200.3,2,16113575,5,5853300,1",            "34201,5,0,200,5853900,1",
      "34300.5,6,0,1000,5853400,0",                "34301,7,0,0,0,0",
      "34200.123456789012,1,16113575,18,5853300,1", "9223372036.854775807,7,-5,-6,-7,-1",
      "34200.5,1,7,100,5000,-1\r"};
  for (const std::string& line : lines) {
    const auto first = replay::parse_message_line(line);
    ASSERT_TRUE(first.ok()) << line << ": " << first.error;
    const std::string text = replay::format_message_line(*first.value);
    const auto second = replay::parse_message_line(text);
    ASSERT_TRUE(second.ok()) << text << ": " << second.error;
    EXPECT_EQ(*second.value, *first.value) << line;
    EXPECT_FALSE(second.time_truncated) << text;
    EXPECT_EQ(replay::format_message_line(*second.value), text);
  }
}

TEST(LobsterBook, FullRow) {
  const auto r = replay::parse_book_line("5853800,200,5853300,100,5853900,50,5853200,75", 2);
  ASSERT_TRUE(r.ok()) << r.error;
  EXPECT_FALSE(r.oracle_malformed);
  ASSERT_EQ(r.value->asks.size(), 2u);
  ASSERT_EQ(r.value->bids.size(), 2u);
  EXPECT_EQ(r.value->asks[0], (BookLevel{5853800, 200}));
  EXPECT_EQ(r.value->bids[0], (BookLevel{5853300, 100}));
  EXPECT_EQ(r.value->asks[1], (BookLevel{5853900, 50}));
  EXPECT_EQ(r.value->bids[1], (BookLevel{5853200, 75}));
}

TEST(LobsterBook, EmptyLevelsWhateverThePlaceholderPrice) {
  const auto a = replay::parse_book_line("5853800,200,5853300,100,9999999999,0,-9999999999,0", 2);
  ASSERT_TRUE(a.ok()) << a.error;
  EXPECT_FALSE(a.oracle_malformed);
  EXPECT_TRUE(a.value->asks[1].empty());
  EXPECT_TRUE(a.value->bids[1].empty());
  EXPECT_FALSE(a.value->asks[0].empty());
  // Placeholders of other values, equal ones, and ones that would break the order if non-empty.
  const auto b = replay::parse_book_line("5853800,200,5853300,100,0,0,0,0", 2);
  ASSERT_TRUE(b.ok());
  EXPECT_FALSE(b.oracle_malformed);
  const auto c = replay::parse_book_line("9999999999,0,-9999999999,0,9999999999,0,-9999999999,0", 2);
  ASSERT_TRUE(c.ok());
  EXPECT_FALSE(c.oracle_malformed);
}

TEST(LobsterBook, NonMonotoneRowIsFlaggedButReturned) {
  const auto asks = replay::parse_book_line("5853900,10,5853300,10,5853800,10,5853200,10", 2);
  ASSERT_TRUE(asks.ok());
  EXPECT_TRUE(asks.oracle_malformed);
  const auto bids = replay::parse_book_line("5853800,10,5853200,10,5853900,10,5853300,10", 2);
  ASSERT_TRUE(bids.ok());
  EXPECT_TRUE(bids.oracle_malformed);
  const auto equal = replay::parse_book_line("5853800,10,5853300,10,5853800,10,5853200,10", 2);
  ASSERT_TRUE(equal.ok());
  EXPECT_TRUE(equal.oracle_malformed);
  ASSERT_EQ(equal.value->asks.size(), 2u);
}

TEST(LobsterBook, NonEmptyLevelAfterAnEmptyOneIsMalformed) {
  const auto r = replay::parse_book_line("5853800,10,5853300,10,9999999999,0,5853200,10,5854000,10,5853100,10", 3);
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.oracle_malformed);  // ask level 3 after an empty ask level 2; the bids are fine
}

TEST(LobsterBook, Rejections) {
  auto rejected = [](const std::string& line, int levels, const std::string& part) {
    const auto r = replay::parse_book_line(line, levels);
    EXPECT_FALSE(r.ok()) << line;
    EXPECT_NE(r.error.find(part), std::string::npos) << line << ": reason was \"" << r.error << "\"";
  };
  rejected("5853800,200,5853300,100", 2, "8 fields, found 4");
  rejected("5853800,200,5853300,100,1", 1, "4 fields, found 5");
  rejected("", 1, "4 fields, found 1");
  rejected("5853800,-1,5853300,100", 1, "ask size");
  rejected("5853800,1,5853300,-100", 1, "bid size");
  rejected("5853800,1,x,100", 1, "bid price");
  rejected("5853800,,5853300,100", 1, "ask size");
  rejected("5853800,1.5,5853300,100", 1, "ask size");
  rejected("a,1,5853300,100", 1, "ask price");
  rejected("5853800,1,5853300,100", 0, "levels");
  rejected("5853800,1,5853300,100", -3, "levels");
  rejected("5853800,1,5853300,99999999999999999999", 1, "bid size");
}

TEST(LobsterBook, CarriageReturn) {
  const auto r = replay::parse_book_line("5853800,200,5853300,100\r", 1);
  ASSERT_TRUE(r.ok()) << r.error;
  EXPECT_TRUE(r.cr_stripped);
}

TEST(LobsterFilename, RealNames) {
  EXPECT_EQ(replay::levels_from_filename("AAPL_2012-06-21_34200000_57600000_orderbook_10.csv"), 10);
  EXPECT_EQ(replay::levels_from_filename("AAPL_2012-06-21_34200000_57600000_message_10.csv"), 10);
  EXPECT_EQ(replay::levels_from_filename("MSFT_2012-06-21_34200000_57600000_orderbook_1.csv"), 1);
  EXPECT_EQ(replay::levels_from_filename("AMZN_2012-06-21_34200000_57600000_orderbook_50.csv"), 50);
}

TEST(LobsterFilename, DirectoryPrefix) {
  EXPECT_EQ(replay::levels_from_filename("data/AAPL_2012-06-21_34200000_57600000_orderbook_5.csv"), 5);
  EXPECT_EQ(replay::levels_from_filename("/abs/dir_1/x_orderbook_3/AAPL_2012-06-21_34200000_57600000_message_10.csv"), 10);
  EXPECT_EQ(replay::levels_from_filename("./AAPL_2012-06-21_34200000_57600000_message_2.csv"), 2);
}

TEST(LobsterFilename, BadNames) {
  for (const char* bad :
       {"", "orderbook_10.csv", "AAPL_2012-06-21_34200000_57600000_orderbook_.csv",
        "AAPL_2012-06-21_34200000_57600000_orderbook_10.txt", "AAPL_2012-06-21_34200000_57600000_orderbook_10",
        "AAPL_2012-06-21_34200000_57600000_orderbook_0.csv", "AAPL_2012-06-21_34200000_57600000_book_10.csv",
        "AAPL_2012-06-21_34200000_57600000_orderbook_-1.csv", "AAPL_2012-06-21_34200000_57600000_orderbook_1a.csv",
        "AAPL_2012-06-21_34200000_57600000_orderbook_99999999999.csv", "AAPL_orderbook_10.csv/",
        "dir_orderbook_10.csv/AAPL.csv", ".csv"}) {
    EXPECT_FALSE(replay::levels_from_filename(bad).has_value()) << bad;
  }
}

TEST(LobsterFilename, OnlyTheEndingMatters) {
  EXPECT_EQ(replay::levels_from_filename("_message_5.csv"), 5);
}

TEST(LobsterColumns, LevelsFromColumnCount) {
  EXPECT_EQ(replay::count_fields("1,2,3,4"), 4u);
  EXPECT_EQ(replay::count_fields("1,2,3,4\r"), 4u);
  EXPECT_EQ(replay::count_fields(""), 1u);
  EXPECT_EQ(replay::levels_from_columns(40), 10);
  EXPECT_EQ(replay::levels_from_columns(4), 1);
  EXPECT_FALSE(replay::levels_from_columns(0).has_value());
  EXPECT_FALSE(replay::levels_from_columns(6).has_value());
  EXPECT_FALSE(replay::levels_from_columns(3).has_value());
}

}  // namespace
