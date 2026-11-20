// The replayer on the hand-written fixtures of tests/fixtures/lobster/<case>/: each case holds a
// message file, an orderbook file and expected_summary.json, written by hand from the LOBSTER
// format rules (not by the exporter). The deterministic part of summary.json must equal the
// expected file exactly; wall time is never compared.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "replay/lobster.hpp"
#include "replay/replayer.hpp"

namespace {

namespace fs = std::filesystem;

const fs::path kFixtures = fs::path(LOBSIM_SOURCE_DIR) / "tests" / "fixtures" / "lobster";

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  EXPECT_TRUE(f.good()) << "cannot read " << p;
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// The single file of `dir` whose name contains `kind` ("_message_" or "_orderbook_").
fs::path find_file(const fs::path& dir, const std::string& kind) {
  std::vector<fs::path> found;
  for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
    if (e.path().filename().string().find(kind) != std::string::npos) found.push_back(e.path());
  }
  EXPECT_EQ(found.size(), 1u) << dir << " must hold exactly one *" << kind << "* file";
  return found.empty() ? fs::path() : found.front();
}

replay::ReplayOptions options_from(const nlohmann::json& expected) {
  replay::ReplayOptions o;
  o.mode = replay::parse_mode(expected.at("mode").get<std::string>()).value();
  o.compare_depth = expected.at("compare_depth").get<int>();
  o.trust_depth = expected.at("trust_depth").get<int>();
  return o;
}

// Every message lands in exactly one "what happened" counter.
void expect_every_message_accounted(const replay::Counters& c) {
  const std::uint64_t outcomes = c.parse_rejected + c.consumed_by_seed + c.submitted + c.dup_id + c.submit_would_cross +
                                 c.submit_bad_price + c.cancelled + c.deleted + c.executed + c.direction_mismatch +
                                 c.price_mismatch + c.seed_attributed + c.unknown_id + c.hidden_exec + c.cross_trade +
                                 c.halt;
  EXPECT_EQ(outcomes, c.messages);
  std::uint64_t by_type = 0;
  for (int t = 1; t <= 7; ++t) by_type += c.by_type[t];
  EXPECT_EQ(by_type + c.parse_rejected, c.messages);
}

struct FixtureRun {
  replay::Summary summary;
  nlohmann::json expected;
};

FixtureRun run_fixture(const std::string& name) {
  const fs::path dir = kFixtures / name;
  FixtureRun r;
  r.expected = nlohmann::json::parse(read_text(dir / "expected_summary.json"));
  r.summary = replay::replay_files(find_file(dir, "_message_").string(), find_file(dir, "_orderbook_").string(),
                                   options_from(r.expected));
  return r;
}

class LobsterFixture : public ::testing::TestWithParam<std::string> {};

TEST_P(LobsterFixture, SummaryMatchesTheHandWrittenExpectation) {
  const FixtureRun r = run_fixture(GetParam());
  const nlohmann::json actual = nlohmann::json::parse(replay::summary_json(r.summary, std::nullopt));
  EXPECT_EQ(actual, r.expected) << "actual:\n" << actual.dump(2) << "\nexpected:\n" << r.expected.dump(2);
  expect_every_message_accounted(r.summary.counters);
  const fs::path divergences = kFixtures / GetParam() / "expected_divergences.csv";
  if (fs::exists(divergences)) {
    EXPECT_EQ(replay::divergences_csv(r.summary), read_text(divergences));
  }
}

INSTANTIATE_TEST_SUITE_P(Lobster, LobsterFixture,
                         ::testing::Values("basic_types", "burst", "anomalies", "resync", "strict_failure"),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

// Every case directory is listed above, so a new fixture cannot be skipped by accident.
TEST(LobsterFixtureList, EveryDirectoryIsTested) {
  std::vector<std::string> dirs;
  for (const fs::directory_entry& e : fs::directory_iterator(kFixtures)) {
    if (e.is_directory()) dirs.push_back(e.path().filename().string());
  }
  std::sort(dirs.begin(), dirs.end());
  EXPECT_EQ(dirs, (std::vector<std::string>{"anomalies", "basic_types", "burst", "resync", "strict_failure"}));
}

TEST(LobsterFixtureStrict, ReportShowsTheLineBothBooksAndTheLastMessages) {
  const FixtureRun r = run_fixture("strict_failure");
  ASSERT_TRUE(r.summary.strict_failure.has_value());
  const replay::StrictFailure& f = *r.summary.strict_failure;
  EXPECT_EQ(f.line, 4u);
  EXPECT_EQ(f.exact_depth, 0);
  ASSERT_EQ(f.recent.size(), 4u);
  EXPECT_EQ(f.recent.back(), "line 4: 34200.000000004,4,2,4,1000100,1");
  const std::string report = replay::strict_failure_report(r.summary);
  EXPECT_NE(report.find("line 4"), std::string::npos) << report;
  EXPECT_NE(report.find("1000100 x 10"), std::string::npos) << report;  // our ask
  EXPECT_NE(report.find("1000100 x 6"), std::string::npos) << report;   // the oracle's ask
}

// The Direction of an execution is the resting order's side. Reading it as the aggressor's side
// must give a wrong book on the fixtures that contain executions (the check the mutation canary
// relies on): every type-4 Direction is flipped here, and the rows must stop being exact.
TEST(LobsterFixtureDirection, ReadingDirectionAsTheAggressorBreaksTheBook) {
  for (const std::string name : {"basic_types", "burst"}) {
    const fs::path dir = kFixtures / name;
    std::istringstream original(read_text(find_file(dir, "_message_")));
    std::string flipped, line;
    while (std::getline(original, line)) {
      replay::LobsterMsg m = replay::parse_message_line(line).value.value();
      if (m.type == 4) m.direction = -m.direction;
      flipped += replay::format_message_line(m) + "\n";
    }
    std::istringstream messages(flipped);
    std::istringstream orderbook(read_text(find_file(dir, "_orderbook_")));
    replay::ReplayOptions o;
    o.mode = replay::Mode::Report;
    replay::Replayer replayer(3, o);
    const replay::Summary& s = replayer.run(messages, orderbook);
    EXPECT_LT(s.exact_rows[0], s.rows_compared) << name;
    EXPECT_GT(s.counters.direction_mismatch + s.counters.unknown_id, 0u) << name;
  }
}

// ---------------------------------------------------------------- input errors

replay::Summary run_text(const std::string& messages, const std::string& orderbook, int levels,
                         replay::ReplayOptions o = {}) {
  std::istringstream m(messages), b(orderbook);
  replay::Replayer r(levels, o);
  return r.run(m, b);
}

TEST(LobsterReplayInput, FilesOfDifferentLengthAreRejected) {
  const std::string msg = "34200.1,1,1,10,100,1\n34200.2,1,2,10,200,-1\n";
  EXPECT_THROW(run_text(msg, "0,0,100,10\n", 1), replay::InputError);
  EXPECT_THROW(run_text("34200.1,1,1,10,100,1\n", "0,0,100,10\n200,10,100,10\n", 1), replay::InputError);
}

TEST(LobsterReplayInput, FirstRowColumnsMustMatchTheLevelCount) {
  EXPECT_THROW(run_text("34200.1,1,1,10,100,1\n", "0,0,100,10\n", 2), replay::InputError);
  EXPECT_THROW(run_text("34200.1,1,1,10,100,1\n", "0,0,100,x\n", 1), replay::InputError);
}

TEST(LobsterReplayInput, OptionsOutOfRange) {
  replay::ReplayOptions o;
  o.compare_depth = 4;
  EXPECT_THROW(replay::Replayer(3, o), std::invalid_argument);
  o.compare_depth = 0;
  EXPECT_THROW(replay::Replayer(3, o), std::invalid_argument);
  o.compare_depth = 1;
  o.trust_depth = 0;
  EXPECT_THROW(replay::Replayer(3, o), std::invalid_argument);
}

TEST(LobsterReplayInput, LevelCountComesFromTheOrderbookFileName) {
  const fs::path dir = kFixtures / "burst";
  EXPECT_THROW(replay::replay_files((dir / "missing_message_3.csv").string(), find_file(dir, "_orderbook_").string(), {}),
               replay::InputError);
  EXPECT_THROW(replay::replay_files(find_file(dir, "_message_").string(), (dir / "expected_summary.json").string(), {}),
               replay::InputError);
}

TEST(LobsterReplayInput, BadLaterOracleRowsAreCountedNotFatal) {
  // Line 2's orderbook row has a wrong column count: counted, not compared.
  const replay::Summary s =
      run_text("34200.1,1,1,10,100,1\n34200.2,1,2,10,200,-1\n", "-1,0,100,10\n200,10\n", 1,
               replay::ReplayOptions{replay::Mode::Report, 1, 3, 100'000});
  EXPECT_EQ(s.counters.oracle_rejected, 1u);
  EXPECT_EQ(s.rows_compared, 1u);
  EXPECT_FALSE(s.strict_failure.has_value());
}

TEST(LobsterReplayInput, StrictStopsAtAnUnparsableOracleRow) {
  const replay::Summary s =
      run_text("34200.1,1,1,10,100,1\n34200.2,1,2,10,200,-1\n", "-1,0,100,10\n200,10\n", 1,
               replay::ReplayOptions{replay::Mode::Strict, 1, 3, 100'000});
  ASSERT_TRUE(s.strict_failure.has_value());
  EXPECT_TRUE(s.strict_failure->oracle_unparsable);
  EXPECT_EQ(s.strict_failure->line, 2u);
}

TEST(LobsterReplayReport, PercentIsIntegerArithmetic) {
  EXPECT_EQ(replay::percent(0, 0), "n/a");
  EXPECT_EQ(replay::percent(1, 3), "33.33");
  EXPECT_EQ(replay::percent(2, 3), "66.66");
  EXPECT_EQ(replay::percent(7, 7), "100.00");
  EXPECT_EQ(replay::percent(1, 200), "0.50");
}

}  // namespace
