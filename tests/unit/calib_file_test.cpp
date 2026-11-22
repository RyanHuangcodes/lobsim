#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "calib/calibration.hpp"

using namespace calib;
using nlohmann::json;

namespace {

Pmf pmf(std::vector<std::int64_t> v, std::vector<std::uint64_t> c) { return Pmf{std::move(v), std::move(c)}; }

Survival curve() {
  Survival s;
  s.times_ns = {1000, 5000, 20000};
  s.survival = {0.9, 0.6, 0.3};
  s.at_risk = {10, 9, 5};
  s.events = {1, 3, 3};
  s.orders = 12;
  s.cancelled = 7;
  s.censored_executed = 3;
  s.censored_window_end = 1;
  s.censored_left_window = 1;
  return s;
}

Calibration make() {
  Calibration c;
  c.source = "hand-made";
  c.ticker = "TEST";
  c.date = "2012-06-21";
  c.window_from_ns = 34200LL * 1000000000LL;
  c.window_to_ns = 57600LL * 1000000000LL;
  c.observed_ns = c.window_to_ns - c.window_from_ns;
  c.window_note = "whole day";
  c.inputs = {{"msg.csv", "abcd"}};
  c.code_commit = "unknown";
  c.levels = 10;
  c.tick_units = 100;
  c.median_mid_ticks = 5000;
  c.rates.limit = {Rate{100, 2.5}, Rate{90, 2.25}};
  c.rates.aggressive = {Rate{10, 0.25}, Rate{12, 0.3}};
  for (int s = 0; s < 2; ++s) {
    c.placement[s][0] = pmf({0, 1, 2}, {5, 3, 1});
    c.placement[s][1] = pmf({-1, 0, 4}, {1, 2, 3});
    c.placement[s][2] = pmf({-5, 0, 7}, {1, 2, 3});
  }
  c.placement_fit[kBuy] = PlacementFit{0.5, 1.25, 77, "least squares"};
  c.limit_size = pmf({1, 2, 5}, {10, 5, 1});
  c.aggressive_size = pmf({1, 3}, {4, 1});
  c.pooled_limit_size = 2;
  c.pooled_aggressive_size = 0;
  for (auto& l : c.lifetime) l = curve();
  c.report = json{{"note", "free form"}, {"n", 3}};
  return c;
}

void expect_invalid(const Calibration& c, const std::string& substr) {
  try {
    validate(c);
    FAIL() << "validate accepted; expected error containing: " << substr;
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find(substr), std::string::npos) << e.what();
  }
}

std::string slurp(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream b;
  b << in.rdbuf();
  return b.str();
}

}  // namespace

TEST(CalibFile, ValidBaselinePasses) { EXPECT_NO_THROW(validate(make())); }

TEST(CalibFile, RoundTripSaveLoad) {
  const std::string p1 = ::testing::TempDir() + "calib_rt1.json", p2 = ::testing::TempDir() + "calib_rt2.json";
  const Calibration c = make();
  save(p1, c);
  const Calibration d = load(p1);
  EXPECT_EQ(c, d);
  save(p2, d);
  EXPECT_EQ(slurp(p1), slurp(p2));
  const std::string text = slurp(p1);
  ASSERT_FALSE(text.empty());
  EXPECT_EQ(text.back(), '\n');
  std::remove(p1.c_str());
  std::remove(p2.c_str());
}

TEST(CalibFile, RoundTripWithoutFitAndReport) {
  Calibration c = make();
  c.placement_fit = {};
  c.report = json::object();
  const json j = to_json(c);
  EXPECT_TRUE(j["placement_fit"]["buy"].is_null());
  EXPECT_EQ(from_json(j), c);
}

TEST(CalibFile, MissingReportDefaultsToEmpty) {
  json j = to_json(make());
  j.erase("report");
  EXPECT_EQ(from_json(j).report, json::object());
}

TEST(CalibFile, LoadMissingFileNamesPath) {
  try {
    load("/nonexistent/dir/x.json");
    FAIL();
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("/nonexistent/dir/x.json"), std::string::npos);
  }
}

TEST(CalibFile, UnknownAndMissingKeys) {
  json j = to_json(make());
  json a = j;
  a["extra"] = 1;
  EXPECT_THROW(from_json(a), std::runtime_error);
  json b = j;
  b["rates"]["limit"]["buy"]["bogus"] = 1;
  EXPECT_THROW(from_json(b), std::runtime_error);
  json c = j;
  c["placement"]["buy"]["3+"]["extra"] = 1;
  EXPECT_THROW(from_json(c), std::runtime_error);
  json d = j;
  d["lifetime"]["1-2"]["nope"] = 1;
  EXPECT_THROW(from_json(d), std::runtime_error);
  for (const char* k : {"source", "rates", "placement", "placement_fit", "limit_size", "lifetime", "tick_units",
                        "median_mid_ticks", "inputs"}) {
    json m = j;
    m.erase(k);
    EXPECT_THROW(from_json(m), std::runtime_error) << k;
  }
  json e = j;
  e["rates"]["limit"].erase("sell");
  EXPECT_THROW(from_json(e), std::runtime_error);
  json f = j;
  f["lifetime"]["3-9"].erase("events");
  EXPECT_THROW(from_json(f), std::runtime_error);
}

TEST(CalibFile, WrongTypesRejected) {
  json j = to_json(make());
  json a = j;
  a["tick_units"] = "x";
  EXPECT_THROW(from_json(a), std::runtime_error);
  json b = j;
  b["limit_size"]["counts"][0] = -3;
  EXPECT_THROW(from_json(b), std::runtime_error);
  json c = j;
  c["rates"]["limit"]["buy"]["per_s"] = "fast";
  EXPECT_THROW(from_json(c), std::runtime_error);
}

TEST(CalibFile, FromJsonValidates) {
  json j = to_json(make());
  j["source"] = "bogus";
  EXPECT_THROW(from_json(j), std::runtime_error);
}

TEST(CalibValidate, Header) {
  Calibration c = make();
  c.schema_version = 2;
  expect_invalid(c, "schema_version");
  c = make();
  c.source = "fake";
  expect_invalid(c, "source");
  c = make();
  c.window_from_ns = c.window_to_ns + 1;
  expect_invalid(c, "window_from_ns");
  c = make();
  c.observed_ns = -1;
  expect_invalid(c, "observed_ns");
  c = make();
  c.tick_units = 0;
  expect_invalid(c, "tick_units");
  c = make();
  c.levels = -1;
  expect_invalid(c, "levels");
  c = make();
  c.median_mid_ticks = 0;
  expect_invalid(c, "median_mid_ticks");
}

TEST(CalibValidate, Rates) {
  Calibration c = make();
  c.rates.limit[kSell].per_s = -0.1;
  expect_invalid(c, "rates.limit[sell].per_s");
  c = make();
  c.rates.aggressive[kBuy].per_s = std::numeric_limits<double>::infinity();
  expect_invalid(c, "rates.aggressive[buy].per_s");
  c = make();
  c.rates.limit[kBuy].per_s = std::nan("");
  expect_invalid(c, "per_s");
}

TEST(CalibValidate, Pmf) {
  Calibration c = make();
  c.placement[kSell][2].counts[1] = 0;
  expect_invalid(c, "placement[sell][3+].counts[1]: must be positive");
  c = make();
  c.placement[kSell][2].counts.push_back(4);
  expect_invalid(c, "placement[sell][3+]: values and counts differ in length");
  c = make();
  c.placement[kBuy][2].values = {-5, 0, 0};
  expect_invalid(c, "placement[buy][3+].values[2]: not strictly increasing");
  c = make();
  c.limit_size.counts = {std::numeric_limits<std::uint64_t>::max(), 1, 1};
  expect_invalid(c, "limit_size");
  c = make();
  c.placement[kBuy][0].values = {-1, 0, 2};  // s = 1 cannot go below 0
  expect_invalid(c, "placement[buy][1]");
  c = make();
  c.placement[kSell][1].values = {-2, 0, 4};  // s = 2 cannot go below -1
  expect_invalid(c, "placement[sell][2]");
  c = make();
  c.placement[kSell][2].values = {-100, 0, 7};  // 3+ accepts anything
  EXPECT_NO_THROW(validate(c));
  c = make();
  c.limit_size.values = {0, 2, 5};
  expect_invalid(c, "limit_size.values[0]");
  c = make();
  c.aggressive_size.values = {-1, 3};
  expect_invalid(c, "aggressive_size.values[0]");
}

TEST(CalibValidate, Survival) {
  Calibration c = make();
  c.lifetime[1].survival = {0.9, 0.95, 0.3};
  expect_invalid(c, "lifetime[1-2].survival: not non-increasing");
  c = make();
  c.lifetime[2].survival[0] = 1.5;
  expect_invalid(c, "lifetime[3-9].survival[0]: must be in [0, 1]");
  c = make();
  c.lifetime[2].survival[1] = -0.1;
  expect_invalid(c, "lifetime[3-9].survival[1]");
  c = make();
  c.lifetime[0].survival[1] = std::nan("");
  expect_invalid(c, "lifetime[<=0].survival[1]: not finite");
  c = make();
  c.lifetime[3].times_ns = {1000, 1000, 20000};
  expect_invalid(c, "lifetime[>=10].times_ns[1]: not strictly increasing");
  c = make();
  c.lifetime[3].times_ns[0] = 0;
  expect_invalid(c, "lifetime[>=10].times_ns[0]: must be positive");
  c = make();
  c.lifetime[0].survival.pop_back();
  expect_invalid(c, "lifetime[<=0].survival");
  c = make();
  c.lifetime[0].at_risk.pop_back();
  expect_invalid(c, "lifetime[<=0].at_risk/events");
  c = make();
  c.lifetime[0].at_risk.clear();  // events non-empty
  expect_invalid(c, "lifetime[<=0].at_risk/events");
  c = make();
  c.lifetime[0].at_risk.clear();
  c.lifetime[0].events.clear();  // a hand-made curve
  EXPECT_NO_THROW(validate(c));
  c = make();
  c.lifetime[0] = Survival{};  // an empty curve is fine
  EXPECT_NO_THROW(validate(c));
}

TEST(CalibValidate, PlacementFitFinite) {
  Calibration c = make();
  c.placement_fit[kBuy]->mu = std::numeric_limits<double>::infinity();
  expect_invalid(c, "placement_fit[buy].mu");
}

TEST(CalibSurvival, Median) {
  Survival s = curve();
  EXPECT_EQ(s.median_ns(), 20000);  // 0.9, 0.6, then 0.3 <= 0.5 at 20000
  s.survival = {0.9, 0.5, 0.3};
  EXPECT_EQ(s.median_ns(), 5000);  // <= 0.5 inclusive
  s.survival = {0.9, 0.8, 0.7};
  EXPECT_FALSE(s.median_ns().has_value());
  EXPECT_FALSE(Survival{}.median_ns().has_value());
}

TEST(CalibBuckets, Spread) {
  EXPECT_EQ(spread_bucket(1), 0);
  EXPECT_EQ(spread_bucket(2), 1);
  EXPECT_EQ(spread_bucket(3), 2);
  EXPECT_EQ(spread_bucket(7), 2);
  EXPECT_THROW(spread_bucket(0), std::invalid_argument);
  EXPECT_THROW(spread_bucket(-4), std::invalid_argument);
  EXPECT_STREQ(spread_bucket_name(0), "1");
  EXPECT_STREQ(spread_bucket_name(1), "2");
  EXPECT_STREQ(spread_bucket_name(2), "3+");
}

TEST(CalibBuckets, Distance) {
  EXPECT_EQ(distance_bucket(-4), 0);
  EXPECT_EQ(distance_bucket(0), 0);
  EXPECT_EQ(distance_bucket(1), 1);
  EXPECT_EQ(distance_bucket(2), 1);
  EXPECT_EQ(distance_bucket(3), 2);
  EXPECT_EQ(distance_bucket(9), 2);
  EXPECT_EQ(distance_bucket(10), 3);
  EXPECT_EQ(distance_bucket(1000), 3);
  EXPECT_STREQ(distance_bucket_name(0), "<=0");
  EXPECT_STREQ(distance_bucket_name(1), "1-2");
  EXPECT_STREQ(distance_bucket_name(2), "3-9");
  EXPECT_STREQ(distance_bucket_name(3), ">=10");
}

TEST(CalibPmf, Total) {
  EXPECT_EQ(pmf({1, 2}, {3, 4}).total(), 7u);
  EXPECT_EQ(Pmf{}.total(), 0u);
  EXPECT_TRUE(Pmf{}.empty());
}

TEST(CalibDistance, Identical) {
  const Pmf p = pmf({0, 2, 5}, {1, 2, 3});
  EXPECT_DOUBLE_EQ(total_variation(p, p), 0.0);
  EXPECT_DOUBLE_EQ(wasserstein1(p, p), 0.0);
  // Same distribution, different totals and different support spelling.
  EXPECT_DOUBLE_EQ(total_variation(p, pmf({0, 2, 5}, {2, 4, 6})), 0.0);
  EXPECT_DOUBLE_EQ(wasserstein1(p, pmf({0, 2, 5}, {2, 4, 6})), 0.0);
}

TEST(CalibDistance, DisjointSupports) {
  EXPECT_DOUBLE_EQ(total_variation(pmf({0, 1}, {1, 1}), pmf({5, 6}, {3, 1})), 1.0);
  // delta_0 vs delta_3: W1 = 3.
  EXPECT_DOUBLE_EQ(wasserstein1(pmf({0}, {1}), pmf({3}, {7})), 3.0);
  EXPECT_DOUBLE_EQ(total_variation(pmf({0}, {1}), pmf({3}, {7})), 1.0);
}

// P = {0: 1/4, 1: 1/4, 2: 1/2}, Q = {1: 1/2, 3: 1/2}.
// TV = 1/2 (|1/4 - 0| + |1/4 - 1/2| + |1/2 - 0| + |0 - 1/2|) = 1/2 * 1.5 = 0.75.
// F_P = 1/4, 1/2, 1, 1 at x = 0, 1, 2, 3; F_Q = 0, 1/2, 1/2, 1.
// |F_P - F_Q| = 1/4, 0, 1/2, 0, so W1 = 1/4 + 0 + 1/2 = 0.75.
TEST(CalibDistance, MixedByHand) {
  const Pmf p = pmf({0, 1, 2}, {1, 1, 2});
  const Pmf q = pmf({1, 3}, {5, 5});
  EXPECT_DOUBLE_EQ(total_variation(p, q), 0.75);
  EXPECT_DOUBLE_EQ(wasserstein1(p, q), 0.75);
  EXPECT_DOUBLE_EQ(total_variation(q, p), 0.75);
  EXPECT_DOUBLE_EQ(wasserstein1(q, p), 0.75);
}

// Gaps longer than one tick: P = delta_0, Q = {0: 1/2, 10: 1/2}: F differs by 1/2 over 10 ticks.
TEST(CalibDistance, LongGap) {
  EXPECT_DOUBLE_EQ(wasserstein1(pmf({0}, {1}), pmf({0, 10}, {1, 1})), 5.0);
}

TEST(CalibDistance, EmptyThrows) {
  EXPECT_THROW(total_variation(Pmf{}, pmf({0}, {1})), std::invalid_argument);
  EXPECT_THROW(wasserstein1(pmf({0}, {1}), Pmf{}), std::invalid_argument);
}

// The example in calibration/SCHEMA.md must load.
TEST(CalibFile, SchemaExampleLoads) {
  const std::string doc = slurp(std::string(LOBSIM_SOURCE_DIR) + "/calibration/SCHEMA.md");
  const auto a = doc.find("```json\n");
  ASSERT_NE(a, std::string::npos);
  const auto b = doc.find("```", a + 8);
  ASSERT_NE(b, std::string::npos);
  const Calibration c = from_json(json::parse(doc.substr(a + 8, b - a - 8)));
  EXPECT_EQ(c.source, "hand-made");
  EXPECT_EQ(c.median_mid_ticks, 1000);
  EXPECT_EQ(c.lifetime[2].median_ns(), 9000000);
}
