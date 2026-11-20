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
                         ::testing::Values("basic_types", "burst", "anomalies", "resync", "strict_failure", "matching",
                                           "hidden_exec"),
                         [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });

// Every case directory is listed above, so a new fixture cannot be skipped by accident.
TEST(LobsterFixtureList, EveryDirectoryIsTested) {
  std::vector<std::string> dirs;
  for (const fs::directory_entry& e : fs::directory_iterator(kFixtures)) {
    if (e.is_directory()) dirs.push_back(e.path().filename().string());
  }
  std::sort(dirs.begin(), dirs.end());
  EXPECT_EQ(dirs, (std::vector<std::string>{"anomalies", "basic_types", "burst", "hidden_exec", "matching", "resync",
                                       "strict_failure"}));
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

// ---------------------------------------------------------------- matching check (spec 7)
//
// tests/fixtures/lobster/matching holds one burst of each class; every row of it is exact, so the
// previous row is always trusted down to depth 3 (L). Its bursts, by last line:
//   9 exact_via_synthetic, 11 exact, 13 queue_order, 15 size_split, 16 price_skip (better),
//   18 length, 22 price_skip (worse), 23 excluded_unknown, 27 excluded_untrusted (3 prices).
// Each test replays the fixture up to the end of one burst and checks that, against the previous
// burst's end, exactly one counter went up by one at the given trust depth.

std::string first_lines(const fs::path& file, std::size_t n) {
  std::istringstream in(read_text(file));
  std::string out, line;
  for (std::size_t i = 0; i < n && std::getline(in, line); ++i) out += line + "\n";
  return out;
}

replay::Summary replay_prefix(const std::string& name, std::size_t n, int levels = 3) {
  const fs::path dir = kFixtures / name;
  std::istringstream m(first_lines(find_file(dir, "_message_"), n));
  std::istringstream b(first_lines(find_file(dir, "_orderbook_"), n));
  replay::Replayer r(levels, replay::ReplayOptions{});
  return r.run(m, b);
}

std::vector<std::pair<std::string, std::uint64_t>> fields(const replay::MatchCounts& c) {
  return {{"exact", c.exact},
          {"exact_via_synthetic", c.exact_via_synthetic},
          {"queue_order", c.queue_order},
          {"price_skip", c.price_skip},
          {"price_skip_better", c.price_skip_better},
          {"price_skip_worse", c.price_skip_worse},
          {"size_split", c.size_split},
          {"length", c.length},
          {"excluded_unknown", c.excluded_unknown},
          {"excluded_untrusted", c.excluded_untrusted}};
}

const replay::MatchCounts& at_depth(const replay::Summary& s, int trust_depth) {
  for (const replay::MatchCounts& c : s.matching.by_trust_depth) {
    if (c.trust_depth == trust_depth) return c;
  }
  ADD_FAILURE() << "no trust depth " << trust_depth;
  return s.matching.by_trust_depth.front();
}

// Lines (from, to] of the matching fixture hold exactly one burst, of the classes `expected`.
void expect_burst(std::size_t from, std::size_t to, int trust_depth, const std::vector<std::string>& expected) {
  const replay::Summary before = replay_prefix("matching", from);
  const replay::Summary after = replay_prefix("matching", to);
  EXPECT_EQ(after.matching.bursts, before.matching.bursts + 1);
  const auto a = fields(at_depth(after, trust_depth));
  const auto b = fields(at_depth(before, trust_depth));
  for (std::size_t i = 0; i < a.size(); ++i) {
    const bool raised = std::find(expected.begin(), expected.end(), a[i].first) != expected.end();
    EXPECT_EQ(a[i].second, b[i].second + (raised ? 1 : 0)) << a[i].first << " for the burst ending at line " << to;
  }
}

TEST(MatchingClass, Exact) { expect_burst(9, 11, 3, {"exact"}); }

TEST(MatchingClass, ExactViaSynthetic) {
  // Message 1's order lives in the seed: its execution is SYN@2000 on both sides.
  expect_burst(7, 9, 3, {"exact_via_synthetic"});
}

TEST(MatchingClass, QueueOrder) {
  expect_burst(11, 13, 3, {"queue_order"});
  const replay::Summary s = replay_prefix("matching", 13);
  ASSERT_EQ(s.matching.queue_order_examples.size(), 1u);
  EXPECT_EQ(s.matching.queue_order_examples[0].data, (std::vector<std::string>{"5 10 @ 2000"}));
  EXPECT_EQ(s.matching.queue_order_examples[0].predicted, (std::vector<std::string>{"2 10 @ 2000"}));
}

TEST(MatchingClass, PriceSkipBetterAndWorse) {
  // The data executes at 2200 while 2000 is the best ask: the prediction is at a better price.
  expect_burst(15, 16, 3, {"price_skip", "price_skip_better"});
  // The data fills 10 at 2000 where the book has 5 there: the prediction moves on to 2100, a worse price.
  expect_burst(18, 22, 3, {"price_skip", "price_skip_worse"});
}

TEST(MatchingClass, SizeSplit) { expect_burst(13, 15, 3, {"size_split"}); }

TEST(MatchingClass, Length) {
  // The second fill is at a price where the book has nothing more: the prediction is one fill short.
  expect_burst(16, 18, 3, {"length"});
}

TEST(MatchingClass, ExcludedUnknown) {
  // An unknown id with no synthetic order to take it from; it is excluded at every trust depth,
  // before the trust rule is looked at.
  expect_burst(22, 23, 3, {"excluded_unknown"});
  expect_burst(22, 23, 1, {"excluded_unknown"});
}

TEST(MatchingClass, ExcludedUntrusted) {
  // Three distinct prices need a trust depth of at least 4: excluded at 3 although it would match.
  expect_burst(23, 27, 3, {"excluded_untrusted"});
  // One price needs depth 2: every burst is excluded at trust depth 1.
  expect_burst(9, 11, 1, {"excluded_untrusted"});
  // The previous row was not exact before its correction (resync fixture, line 4): excluded at 3.
  const replay::Summary s = replay_prefix("resync", 5);
  EXPECT_EQ(at_depth(s, 3).excluded_untrusted, 1u);
  EXPECT_EQ(s.matching.bursts, 1u);
}

TEST(MatchingBurst, AnotherTypeOrTimeOrDirectionEndsABurst) {
  // basic_types: executions at lines 5, 11 and 12; 11 and 12 differ in time only.
  EXPECT_EQ(replay_prefix("basic_types", 16).matching.bursts, 3u);
  // burst: lines 6-8 (one time, one Direction) and 10-11.
  const replay::Summary s = replay_prefix("burst", 12);
  EXPECT_EQ(s.matching.bursts, 2u);
  EXPECT_EQ(s.matching.burst_messages, 5u);
}

TEST(MatchingBurst, TheLookaheadIsCapped) {
  const fs::path dir = kFixtures / "matching";
  std::istringstream m(read_text(find_file(dir, "_message_")));
  std::istringstream b(read_text(find_file(dir, "_orderbook_")));
  replay::ReplayOptions o;
  o.max_lookahead = 2;  // the 3-execution burst at line 20 needs 2 lines ahead, plus one to see its end
  replay::Replayer r(3, o);
  EXPECT_THROW(r.run(m, b), replay::InputError);
}

TEST(MatchingBurst, AgreementRateIsAgreeOverScored) {
  const replay::Summary s = replay_prefix("matching", 27);
  const replay::MatchCounts& c = s.matching.headline();
  EXPECT_EQ(c.trust_depth, 3);
  EXPECT_EQ(c.agree(), 2u);
  EXPECT_EQ(c.scored(), 7u);  // 9 bursts - 1 excluded_unknown - 1 excluded_untrusted
}

// ---- synthetic fills of one order are merged before the comparison (D4)
//
// Three levels, resync mode, every row exact. A row is "ask price, ask size, bid price, bid size" per
// level; the helper below writes level 1 and pads the other two levels as empty.

std::string row3(const std::string& ask, const std::string& bid) {
  const std::string none_ask = "9999999999,0", none_bid = "-9999999999,0";
  return (ask.empty() ? none_ask : ask) + "," + (bid.empty() ? none_bid : bid) + "," + none_ask + "," + none_bid +
         "," + none_ask + "," + none_bid + "\n";
}

TEST(MatchingMerge, TwoUnknownIdsInsideOneSeededLevelAreOneSyntheticFill) {
  // Message 1 is consumed by the seed: one synthetic ask of 30 at 100. The burst (lines 2-3) executes
  // unknown ids 777 (10) and 778 (20), both attributed to that synthetic order. The engine fills it
  // in one piece: SYN 30 @ 100. Without the merge the data would be two fills against one.
  const std::string messages = "34200.000000001,1,1,30,100,-1\n"
                               "34200.000000002,4,777,10,100,-1\n"
                               "34200.000000002,4,778,20,100,-1\n";
  const std::string book = row3("100,30", "") + row3("100,20", "") + row3("", "");
  const replay::Summary s = run_text(messages, book, 3);
  EXPECT_EQ(s.matching.bursts, 1u);
  EXPECT_EQ(s.matching.burst_messages, 2u);
  const replay::MatchCounts& c = at_depth(s, 3);
  EXPECT_EQ(c.exact_via_synthetic, 1u);
  EXPECT_EQ(c.length, 0u);
  EXPECT_EQ(c.agree(), 1u);
  EXPECT_EQ(c.scored(), 1u);
}

TEST(MatchingMerge, ASeededLevelThenARealOrderBehindIt) {
  // The seed is a synthetic ask of 30 at 100; real order 5 (10) joins behind it. The burst takes the
  // seed with ids 777 (10) and 778 (20), then order 5: data SYN 30, then 5 10 @ 100.
  const std::string messages = "34200.000000001,1,1,30,100,-1\n"
                               "34200.000000002,1,5,10,100,-1\n"
                               "34200.000000003,4,777,10,100,-1\n"
                               "34200.000000003,4,778,20,100,-1\n"
                               "34200.000000003,4,5,10,100,-1\n";
  const std::string book = row3("100,30", "") + row3("100,40", "") + row3("100,30", "") + row3("100,10", "") +
                           row3("", "");
  const replay::Summary s = run_text(messages, book, 3);
  EXPECT_EQ(s.matching.burst_messages, 3u);
  const replay::MatchCounts& c = at_depth(s, 3);
  EXPECT_EQ(c.exact_via_synthetic, 1u);
  EXPECT_EQ(c.exact, 0u);
  EXPECT_EQ(c.length, 0u);
}

TEST(MatchingMerge, TwoRealOrdersAtOnePriceAreNeverMerged) {
  // Seed: a bid of 10 at 90 only. Real asks 5 and 6 (10 each) at 100. Burst: both execute fully.
  const std::string head = "34200.000000001,1,1,10,90,1\n"
                           "34200.000000002,1,5,10,100,-1\n"
                           "34200.000000003,1,6,10,100,-1\n";
  const std::string rows = row3("", "90,10") + row3("100,10", "90,10") + row3("100,20", "90,10");
  {
    const replay::Summary s = run_text(head + "34200.000000004,4,5,10,100,-1\n34200.000000004,4,6,10,100,-1\n",
                                       rows + row3("100,10", "90,10") + row3("", "90,10"), 3);
    const replay::MatchCounts& c = at_depth(s, 3);
    EXPECT_EQ(c.exact, 1u);  // two real fills against two real fills, no synthetic order involved
    EXPECT_EQ(c.exact_via_synthetic, 0u);
  }
  {
    // 4 from each order: the data holds two fills, the prediction one of 8 against order 5. Merging
    // real orders would hide this; compared one by one it is a size split at the first fill.
    const replay::Summary s = run_text(head + "34200.000000004,4,5,4,100,-1\n34200.000000004,4,6,4,100,-1\n",
                                       rows + row3("100,16", "90,10") + row3("100,12", "90,10"), 3);
    const replay::MatchCounts& c = at_depth(s, 3);
    EXPECT_EQ(c.size_split, 1u);
    EXPECT_EQ(c.exact, 0u);
    ASSERT_EQ(s.matching.size_split_examples.size(), 1u);
    EXPECT_EQ(s.matching.size_split_examples[0].data, (std::vector<std::string>{"5 4 @ 100", "6 4 @ 100"}));
    EXPECT_EQ(s.matching.size_split_examples[0].predicted, (std::vector<std::string>{"5 8 @ 100"}));
  }
}

TEST(MatchingMerge, TwoAggressorsAtOneInstantSplittingOneRealOrderAreOneFill) {
  // Found in the synthetic fake day: two buy orders of 6 and 4 lots arrive at the same nanosecond.
  // The first takes order 5 (2) and 4 of order 6 (5); the second takes the last 1 of order 6 and 3
  // of order 7. The burst rule reads the four executions as one aggressive order of 10, which fills
  // 5 2, 6 5, 7 3: order 6 in one piece. Compared one by one the data's "6 4" would be a size split.
  const std::string head = "34200.000000001,1,1,10,90,1\n"
                           "34200.000000002,1,5,2,100,-1\n"
                           "34200.000000003,1,6,5,100,-1\n"
                           "34200.000000004,1,7,3,100,-1\n";
  const std::string rows = row3("", "90,10") + row3("100,2", "90,10") + row3("100,7", "90,10") +
                           row3("100,10", "90,10");
  const replay::Summary s = run_text(head +
                                         "34200.000000005,4,5,2,100,-1\n"
                                         "34200.000000005,4,6,4,100,-1\n"
                                         "34200.000000005,4,6,1,100,-1\n"
                                         "34200.000000005,4,7,3,100,-1\n",
                                     rows + row3("100,8", "90,10") + row3("100,4", "90,10") +
                                         row3("100,3", "90,10") + row3("", "90,10"),
                                     3);
  EXPECT_EQ(s.matching.bursts, 1u);
  EXPECT_EQ(s.matching.burst_messages, 4u);
  const replay::MatchCounts& c = at_depth(s, 3);
  EXPECT_EQ(c.exact, 1u);
  EXPECT_EQ(c.size_split, 0u);
  EXPECT_EQ(c.agree(), 1u);
  EXPECT_EQ(c.scored(), 1u);
}

TEST(MatchingMerge, FillsOfOneRealOrderAreMergedOnlyWhenConsecutive) {
  // Order 6 is named again after order 7: no aggressive order can do that (after 7 it never comes
  // back to 6, which is ahead of 7), so nothing is merged and the burst is a mismatch.
  const std::string head = "34200.000000001,1,1,10,90,1\n"
                           "34200.000000002,1,6,5,100,-1\n"
                           "34200.000000003,1,7,3,100,-1\n";
  const std::string rows = row3("", "90,10") + row3("100,5", "90,10") + row3("100,8", "90,10");
  const replay::Summary s = run_text(head +
                                         "34200.000000004,4,6,4,100,-1\n"
                                         "34200.000000004,4,7,3,100,-1\n"
                                         "34200.000000004,4,6,1,100,-1\n",
                                     rows + row3("100,4", "90,10") + row3("100,1", "90,10") + row3("", "90,10"), 3);
  const replay::MatchCounts& c = at_depth(s, 3);
  EXPECT_EQ(c.agree(), 0u);
  EXPECT_EQ(c.scored(), 1u);
  EXPECT_EQ(c.size_split, 1u);  // first difference: order 6, 4 in the data, 5 predicted
  ASSERT_EQ(s.matching.size_split_examples.size(), 1u);
  EXPECT_EQ(s.matching.size_split_examples[0].data,
            (std::vector<std::string>{"6 4 @ 100", "7 3 @ 100", "6 1 @ 100"}));
  EXPECT_EQ(s.matching.size_split_examples[0].predicted, (std::vector<std::string>{"6 5 @ 100", "7 3 @ 100"}));
}

}  // namespace

// ---------------------------------------------------------------- hostile input

TEST(LobsterReplayInput, LevelCountAboveTheLimitIsRejectedBeforeAnythingIsSized) {
  EXPECT_NO_THROW(replay::Replayer(replay::kMaxLevels, {}));
  EXPECT_THROW(replay::Replayer(replay::kMaxLevels + 1, {}), std::invalid_argument);
  EXPECT_THROW(replay::Replayer(2'000'000'000, {}), std::invalid_argument);
  // From a file name: refused before either file is opened, with an input error (exit code 1).
  for (const char* name : {"x_orderbook_10001.csv", "x_orderbook_536870913.csv", "x_orderbook_2000000000.csv"}) {
    try {
      replay::replay_files("missing_message.csv", name, {});
      ADD_FAILURE() << name << " was accepted";
    } catch (const replay::InputError& e) {
      EXPECT_NE(std::string(e.what()).find("at most"), std::string::npos) << e.what();
    }
  }
}

TEST(LobsterReplayMatching, ABurstWhoseSizesDoNotFitIn64BitsIsExcludedAndCounted) {
  // Two executions at one time and Direction, on two seed orders of 2^63 - 1 lots each: both are
  // attributable, and their total is more than an int64 can hold.
  const std::string big = "9223372036854775807";
  const std::string row = "100," + big + ",-9999999999,0,101," + big + ",-9999999999,0\n";
  const std::string messages = "34200.000000001,1,1,10,100,-1\n"
                               "34200.000000002,4,777," + big + ",100,-1\n"
                               "34200.000000002,4,778," + big + ",101,-1\n";
  replay::ReplayOptions o;
  o.mode = replay::Mode::Report;
  const replay::Summary s = run_text(messages, row + row + row, 2, o);
  EXPECT_EQ(s.matching.bursts, 1u);
  EXPECT_EQ(s.matching.burst_messages, 2u);
  for (const replay::MatchCounts& c : s.matching.by_trust_depth) {
    EXPECT_EQ(c.excluded_unknown, 1u) << "trust depth " << c.trust_depth;
    EXPECT_EQ(c.scored(), 0u) << "trust depth " << c.trust_depth;
  }
}
