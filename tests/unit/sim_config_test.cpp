#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <unistd.h>
#include <string>

#include "sim/config.hpp"
#include "support/sim_support.hpp"

using namespace sim;
using nlohmann::json;

namespace {

// Applies `edit` to the default config and expects parse_config to throw an error that
// names `field`.
void expect_error(const std::function<void(json&)>& edit, const std::string& field) {
  json j = simtest::default_json();
  edit(j);
  try {
    (void)parse_config(j);
    ADD_FAILURE() << "no error for " << field;
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find(field), std::string::npos) << e.what();
  }
}

}  // namespace

TEST(Config, DefaultMatchesSpecValues) {
  const SimConfig c = simtest::default_config();
  EXPECT_EQ(c.seed, 42u);
  EXPECT_EQ(c.duration_ns, 3'600'000'000'000LL);
  EXPECT_EQ(c.warmup_ns, 60'000'000'000LL);
  EXPECT_EQ(c.max_price, 1'000'000);
  EXPECT_EQ(c.initial_price, 10'000);
  EXPECT_EQ(c.fundamental_dt_ns, 100'000'000);
  EXPECT_EQ(c.sample_interval_ns, 1'000'000'000);
  EXPECT_EQ(c.noise_traders.count, 10u);
  EXPECT_EQ(c.noise_traders.latency.entry, 200'000);
  EXPECT_EQ(c.noise_traders.latency.jitter, 50'000);
  ASSERT_TRUE(c.informed);
  EXPECT_EQ(c.informed->latency.feed, 100'000);
  ASSERT_TRUE(c.mm);
  EXPECT_EQ(c.requote_interval_ns, 100'000'000);
  EXPECT_EQ(c.mm->latency.jitter, 10'000);
  EXPECT_EQ(c.markout_horizons_ns, (std::vector<TimeNs>{1'000'000'000, 5'000'000'000, 30'000'000'000}));
}

TEST(Config, ShippedFileMatchesTheSpecJson) {
  // configs/default.json is read from the source tree through the compile definition.
  const SimConfig a = load_config(std::string(LOBSIM_SOURCE_DIR) + "/configs/default.json");
  EXPECT_EQ(to_json(a), to_json(simtest::default_config()));
}

TEST(Config, ToJsonRoundTrips) {
  const SimConfig c = simtest::default_config();
  const SimConfig d = parse_config(to_json(c));
  EXPECT_EQ(to_json(c), to_json(d));
  EXPECT_EQ(c.duration_ns, d.duration_ns);
  EXPECT_EQ(c.noise_traders.latency, d.noise_traders.latency);
}

TEST(Config, InformedAndMmAreOptional) {
  json j = simtest::default_json();
  j.erase("informed");
  j.erase("mm");
  const SimConfig c = parse_config(j);
  EXPECT_FALSE(c.informed);
  EXPECT_FALSE(c.mm);
  EXPECT_FALSE(to_json(c).contains("mm"));
}

TEST(Config, UnknownKeysAreErrors) {
  expect_error([](json& j) { j["sead"] = 1; }, "sead");
  expect_error([](json& j) { j["noise_traders"]["lamda"] = 1; }, "noise_traders.lamda");
  expect_error([](json& j) { j["noise_traders"]["latency_us"]["jiter"] = 1; }, "jiter");
  expect_error([](json& j) { j["mm"]["extra"] = 1; }, "mm.extra");
  expect_error([](json& j) { j["fundamental"]["x"] = 1; }, "fundamental.x");
}

TEST(Config, MissingAndWrongTypeAreErrors) {
  expect_error([](json& j) { j.erase("seed"); }, "seed");
  expect_error([](json& j) { j["noise_traders"].erase("D"); }, "noise_traders.D");
  expect_error([](json& j) { j["seed"] = "x"; }, "seed");
  expect_error([](json& j) { j["noise_traders"]["count"] = 2.5; }, "noise_traders.count");
  expect_error([](json& j) { j["markout_horizons_s"] = 3; }, "markout_horizons_s");
  expect_error([](json& j) { j["informed"].erase("latency_us"); }, "informed.latency_us");
}

TEST(Config, BadValuesNameTheField) {
  expect_error([](json& j) { j["duration_s"] = 0; }, "duration_s");
  expect_error([](json& j) { j["duration_s"] = -5; }, "duration_s");
  expect_error([](json& j) { j["warmup_s"] = 4000; }, "warmup_s");
  expect_error([](json& j) { j["max_price"] = 1; }, "max_price");
  expect_error([](json& j) { j["initial_price"] = 0; }, "initial_price");
  expect_error([](json& j) { j["initial_price"] = 2000000; }, "initial_price");
  expect_error([](json& j) { j["fundamental"]["dt_s"] = 0; }, "fundamental.dt_s");
  expect_error([](json& j) { j["fundamental"]["dt_s"] = 1e-12; }, "fundamental.dt_s");
  expect_error([](json& j) { j["sample_interval_s"] = 0; }, "sample_interval_s");
  expect_error([](json& j) { j["noise_traders"]["lambda"] = 0; }, "noise_traders.lambda");
  expect_error([](json& j) { j["noise_traders"]["p_limit"] = 1.5; }, "noise_traders.p_limit");
  expect_error([](json& j) { j["noise_traders"]["p_limit"] = -0.1; }, "noise_traders.p_limit");
  expect_error([](json& j) { j["noise_traders"]["order_lifetime_mean_s"] = 0; }, "order_lifetime_mean_s");
  expect_error([](json& j) { j["noise_traders"]["D"] = 0; }, "noise_traders.D");
  expect_error([](json& j) { j["noise_traders"]["max_size"] = 0; }, "noise_traders.max_size");
  expect_error([](json& j) { j["noise_traders"]["latency_us"]["entry"] = -1; }, "noise_traders.latency_us.entry");
  expect_error([](json& j) { j["noise_traders"]["latency_us"]["jitter"] = -1; }, "noise_traders.latency_us.jitter");
  expect_error([](json& j) { j["informed"]["lambda"] = 0; }, "informed.lambda");
  expect_error([](json& j) { j["informed"]["size"] = 0; }, "informed.size");
  expect_error([](json& j) { j["mm"]["gamma"] = 0; }, "mm.gamma");
  expect_error([](json& j) { j["mm"]["requote_interval_ms"] = 0; }, "mm.requote_interval_ms");
  expect_error([](json& j) { j["mm"]["requote_interval_ms"] = 1e-9; }, "mm.requote_interval_ms");
  expect_error([](json& j) { j["markout_horizons_s"] = json::array({1, 0}); }, "markout_horizons_s");
  expect_error([](json& j) { j["duration_s"] = 1e30; }, "duration_s");
}

TEST(Config, UnreadableFileThrows) {
  EXPECT_THROW(load_config("/nonexistent/dir/none.json"), std::runtime_error);
}

TEST(Config, BadJsonFileThrowsAndSaysSo) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("lobsim_Config_BadJsonFileThrowsAndSaysSo_" + std::to_string(::getpid()) + ".json");
  {
    std::ofstream out(path);
    out << "{ \"seed\": 42, \"duration_s\": ";  // truncated
  }
  try {
    (void)load_config(path.string());
    ADD_FAILURE() << "no error for a truncated JSON file";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("not valid JSON"), std::string::npos) << e.what();
  }
  std::filesystem::remove(path);
}

TEST(Config, AnyUnsigned64BitSeedIsAccepted) {
  json j = simtest::default_json();
  j["seed"] = std::uint64_t{18446744073709551615ULL};
  EXPECT_EQ(parse_config(j).seed, 18446744073709551615ULL);
  EXPECT_EQ(parse_config(to_json(parse_config(j))).seed, 18446744073709551615ULL);
  j = simtest::default_json();
  j["seed"] = json::parse("9223372036854775808");  // 2^63, as parsed from a file
  EXPECT_EQ(parse_config(j).seed, 9223372036854775808ULL);
  j["seed"] = -1;
  EXPECT_THROW(parse_config(j), std::runtime_error);
}

TEST(Config, DurationBoundKeepsTimeArithmeticFromOverflowing) {
  json j = simtest::default_json();
  j["duration_s"] = 1e9;
  j["warmup_s"] = 0;
  EXPECT_EQ(parse_config(j).duration_ns, kMaxConfigNs);
  expect_error([](json& x) { x["duration_s"] = 1.0000001e9; }, "duration_s");
  expect_error([](json& x) { x["noise_traders"]["latency_us"]["entry"] = 2e15; }, "noise_traders.latency_us.entry");
  // now (<= 1e18) + a drawn duration (<= kFarFutureNs) + nothing else stays below 2^63.
  static_assert(kMaxConfigNs + kFarFutureNs < std::numeric_limits<TimeNs>::max());
}

TEST(Config, SecondsToNsClampsAndNeverReturnsLessThanOne) {
  EXPECT_EQ(seconds_to_ns(1.5), 1'500'000'000);
  EXPECT_EQ(seconds_to_ns(0.0), 1);
  EXPECT_EQ(seconds_to_ns(-3.0), 1);
  EXPECT_EQ(seconds_to_ns(-1e300), 1);
  EXPECT_EQ(seconds_to_ns(1e-12), 1);
  EXPECT_EQ(seconds_to_ns(3.9e9), 3'900'000'000'000'000'000LL);
  EXPECT_EQ(seconds_to_ns(4.0e9), kFarFutureNs);
  EXPECT_EQ(seconds_to_ns(1e30), kFarFutureNs);
  EXPECT_EQ(seconds_to_ns(std::numeric_limits<double>::infinity()), kFarFutureNs);
  EXPECT_EQ(seconds_to_ns(std::nan("")), kFarFutureNs);
  // Even at the end of the longest run, adding the largest draw cannot overflow.
  EXPECT_GT(std::numeric_limits<TimeNs>::max() - kMaxConfigNs, kFarFutureNs);
}
