// The order log reproduces the run: parsing the text and replaying the requests into a
// fresh OrderBook gives the engine event hash the exchange recorded (SPEC 5.4, 5.7).
#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "sim/order_log.hpp"
#include "sim/replay.hpp"
#include "support/sim_support.hpp"

using namespace sim;

namespace {

ReplayResult replay_text(const std::string& text) {
  std::istringstream in(text);
  return replay_log(in);
}

std::string hex16(std::uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

// A unique directory for one test, removed on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& tag)
      : path_(std::filesystem::temp_directory_path() / ("lobsim_" + tag + "_" + std::to_string(::getpid()))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void write_text(const std::filesystem::path& p, const std::string& text) {
  std::ofstream f(p, std::ios::binary);
  f << text;
}

std::string read_text(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

}  // namespace

TEST(Replay, LogParsedFromTextReproducesTheEngineHash) {
  for (std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    std::ostringstream log;
    Simulator s(simtest::noise_config(10, seed, 60.0), SimulatorOptions{&log});
    s.run();
    const ReplayResult r = replay_text(log.str());
    EXPECT_EQ(r.event_hash, s.exchange().event_hash()) << "seed " << seed;
    EXPECT_EQ(r.requests, s.exchange().requests());
    EXPECT_EQ(r.events, s.exchange().events());
    EXPECT_EQ(r.trades, s.exchange().trades());
    EXPECT_GT(s.exchange().trades(), 0u);
  }
}

TEST(Replay, ParseThenFormatOfASimulatorLogIsByteIdentical) {
  for (std::uint64_t seed : {1ULL, 2ULL}) {
    std::ostringstream log;
    Simulator s(simtest::noise_config(10, seed, 30.0), SimulatorOptions{&log});
    s.run();
    const OrderLog parsed = parse_order_log(log.str());
    EXPECT_EQ(format_order_log(parsed), log.str()) << "seed " << seed;
    EXPECT_EQ(parsed.requests.size(), s.exchange().requests());
    EXPECT_EQ(parsed.requests.size(), replay_text(log.str()).requests);
    EXPECT_GT(parsed.requests.size(), 0u);
  }
}

TEST(Replay, ReplayIsSensitiveToTheLog) {
  std::ostringstream log;
  Simulator s(simtest::noise_config(10, 4, 20.0), SimulatorOptions{&log});
  s.run();
  std::string text = log.str();
  // Drop the last request line: the hash must change (or the replay is blind).
  text.pop_back();
  text.erase(text.rfind('\n') + 1);
  EXPECT_NE(replay_text(text).event_hash, s.exchange().event_hash());
}

TEST(Replay, AFileWrittenByARunReplaysToTheHashInSummaryJson) {
  const TempDir dir("replay_files");
  const simtest::RunFiles f = simtest::run_with_metrics(simtest::default_config_for_files());
  write_text(dir.path() / "orders.log", f.orders_log);
  write_text(dir.path() / "summary.json", f.summary_json);

  std::ifstream in(dir.path() / "orders.log", std::ios::binary);
  ASSERT_TRUE(in);
  const ReplayResult r = replay_log(in);
  const auto summary = nlohmann::json::parse(read_text(dir.path() / "summary.json"));
  EXPECT_EQ(hex16(r.event_hash), summary.at("engine_event_hash").get<std::string>());
  EXPECT_EQ(r.trades, summary.at("counts").at("trades").get<std::uint64_t>());
  EXPECT_EQ(r.max_price, 1'000'000);
}

TEST(Replay, AMalformedLineIsAnErrorNamingItsLineNumber) {
  std::ostringstream log;
  Simulator s(simtest::noise_config(3, 4, 5.0), SimulatorOptions{&log});
  s.run();
  std::string text = log.str();
  // Corrupt the 5th line (line 1 is the header): replace its first space with a letter.
  std::size_t pos = 0;
  for (int i = 0; i < 4; ++i) pos = text.find('\n', pos) + 1;
  const std::size_t sp = text.find(' ', pos);
  ASSERT_NE(sp, std::string::npos);
  text.replace(sp, 1, "x");
  try {
    replay_text(text);
    FAIL() << "expected a ReplayError";
  } catch (const ReplayError& e) {
    EXPECT_EQ(e.line(), 5u);
    EXPECT_NE(std::string(e.what()).find("line 5"), std::string::npos);
  }
}

namespace {
std::string log_with_times(std::initializer_list<TimeNs> times) {
  std::string text = format_header(1'000'000) + "\n";
  for (const TimeNs t : times) text += format_request(t, simtest::limit(lob::Side::Buy, 100, 1)) + "\n";
  return text;
}
}  // namespace

TEST(Replay, ATimeThatGoesBackwardsIsAnErrorNamingItsLineNumber) {
  // Request 3 (line 4) is earlier than request 2 (line 3).
  try {
    replay_text(log_with_times({10, 30, 20}));
    FAIL() << "expected a ReplayError";
  } catch (const ReplayError& e) {
    EXPECT_EQ(e.line(), 4u);
    EXPECT_NE(std::string(e.what()).find("goes backwards"), std::string::npos) << e.what();
  }
}

TEST(Replay, EqualConsecutiveTimesAreAccepted) {
  EXPECT_EQ(replay_text(log_with_times({10, 10, 10, 11})).requests, 4u);
}

// The log of a real run is accepted too: LogParsedFromTextReproducesTheEngineHash and the file
// round-trip below go through replay_log, which now checks that times never decrease.

TEST(Replay, AMissingOrMalformedHeaderIsAnError) {
  EXPECT_THROW(replay_text(""), ReplayError);
  try {
    replay_text("not a header\n");
    FAIL() << "expected a ReplayError";
  } catch (const ReplayError& e) {
    EXPECT_EQ(e.line(), 1u);
  }
}
