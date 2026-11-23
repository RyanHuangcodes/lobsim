// The replayer's calibration hooks (upgrade 3): the observer and the time bound, on the replay
// fixtures of tests/fixtures/lobster/. Without them the replay is unchanged (the replay fixture
// tests compare summary.json exactly); with them it must be unchanged too, up to the bound.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "replay/lobster.hpp"
#include "replay/replayer.hpp"

namespace {

namespace fs = std::filesystem;

const fs::path kFixtures = fs::path(LOBSIM_SOURCE_DIR) / "tests" / "fixtures" / "lobster";

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) out.push_back(l);
  return out;
}

fs::path find_file(const std::string& fixture, const std::string& kind) {
  for (const fs::directory_entry& e : fs::directory_iterator(kFixtures / fixture)) {
    if (e.path().filename().string().find(kind) != std::string::npos) return e.path();
  }
  ADD_FAILURE() << "no " << kind << " file in " << fixture;
  return {};
}

struct Seen {
  std::uint64_t line;
  std::optional<replay::LobsterMsg> msg;
  std::optional<replay::BookRow> before, after;
  bool before_malformed, after_malformed;
  std::optional<std::int64_t> ask_bound, bid_bound;
  std::size_t burst_size, burst_position;
};

class Recorder : public replay::ReplayObserver {
 public:
  void on_message(const replay::ObservedMessage& m) override {
    Seen s{m.line, std::nullopt, std::nullopt, std::nullopt, m.before.malformed, m.after.malformed,
           m.after.ask_bound, m.after.bid_bound, m.burst_size, m.burst_position};
    if (m.msg != nullptr) s.msg = *m.msg;
    if (m.before.row != nullptr) s.before = *m.before.row;
    if (m.after.row != nullptr) s.after = *m.after.row;
    seen.push_back(s);
  }
  void on_end(const replay::RunEnd& e) override { ends.push_back(e); }
  std::vector<Seen> seen;
  std::vector<replay::RunEnd> ends;
};

struct ObservedRun {
  replay::Summary summary;
  replay::RunEnd end;
  Recorder rec;
};

ObservedRun replay_fixture(const std::string& fixture, int levels, std::optional<std::int64_t> from,
                   std::optional<std::int64_t> stop, const std::string& orderbook_text = "") {
  std::istringstream m(read_text(find_file(fixture, "_message_")));
  std::istringstream b(orderbook_text.empty() ? read_text(find_file(fixture, "_orderbook_")) : orderbook_text);
  ObservedRun r;
  replay::Replayer rep(levels, replay::ReplayOptions{});
  if (from) rep.observe(&r.rec, *from);
  if (stop) rep.stop_before(*stop);
  r.summary = rep.run(m, b);
  r.end = rep.run_end();
  return r;
}

TEST(ReplayObserver, ObservingDoesNotChangeTheReplay) {
  for (const char* fixture : {"basic_types", "burst", "anomalies", "resync", "matching", "hidden_exec"}) {
    const ObservedRun plain = replay_fixture(fixture, 3, std::nullopt, std::nullopt);
    const ObservedRun watched = replay_fixture(fixture, 3, 0, std::nullopt);
    EXPECT_EQ(replay::summary_json(watched.summary, std::nullopt), replay::summary_json(plain.summary, std::nullopt))
        << fixture;
    EXPECT_EQ(watched.rec.seen.size(), plain.summary.lines) << fixture;
    ASSERT_EQ(watched.rec.ends.size(), 1u);
    EXPECT_EQ(watched.rec.ends[0].reason, replay::StopReason::EndOfInput);
    EXPECT_EQ(plain.end.reason, replay::StopReason::EndOfInput);
  }
}

TEST(ReplayObserver, SeesEveryMessageWithThePublishedRowsBeforeAndAfter) {
  const ObservedRun r = replay_fixture("burst", 3, 0, std::nullopt);
  const std::vector<std::string> msgs = lines_of(read_text(find_file("burst", "_message_")));
  const std::vector<std::string> rows = lines_of(read_text(find_file("burst", "_orderbook_")));
  ASSERT_EQ(r.rec.seen.size(), msgs.size());
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    const Seen& s = r.rec.seen[i];
    EXPECT_EQ(s.line, i + 1);
    ASSERT_TRUE(s.msg.has_value());
    EXPECT_EQ(*s.msg, *replay::parse_message_line(msgs[i]).value);
    ASSERT_TRUE(s.after.has_value());
    EXPECT_EQ(*s.after, *replay::parse_book_line(rows[i], 3).value);
    if (i == 0) {
      EXPECT_FALSE(s.before.has_value());  // nothing before line 1
    } else {
      ASSERT_TRUE(s.before.has_value());
      EXPECT_EQ(*s.before, *replay::parse_book_line(rows[i - 1], 3).value);
    }
  }
}

TEST(ReplayObserver, SeesTheBurstGrouping) {
  // burst: lines 6-8 are one burst (one time, one Direction), lines 10-11 another.
  const ObservedRun r = replay_fixture("burst", 3, 0, std::nullopt);
  std::vector<std::pair<std::size_t, std::size_t>> got;
  for (const Seen& s : r.rec.seen) got.emplace_back(s.burst_size, s.burst_position);
  const std::vector<std::pair<std::size_t, std::size_t>> want{{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {3, 0},
                                                              {3, 1}, {3, 2}, {0, 0}, {2, 0}, {2, 1}, {0, 0}};
  EXPECT_EQ(got, want);
}

TEST(ReplayObserver, ThePublishedWindowBounds) {
  // resync fixture, L = 3: a side gets a bound only when it shows all three levels.
  const ObservedRun r = replay_fixture("resync", 3, 0, std::nullopt);
  const std::vector<std::string> rows = lines_of(read_text(find_file("resync", "_orderbook_")));
  for (std::size_t i = 0; i < r.rec.seen.size(); ++i) {
    const replay::BookRow row = *replay::parse_book_line(rows[i], 3).value;
    const std::optional<std::int64_t> ask = row.asks[2].empty() ? std::nullopt : std::optional(row.asks[2].price);
    const std::optional<std::int64_t> bid = row.bids[2].empty() ? std::nullopt : std::optional(row.bids[2].price);
    EXPECT_EQ(r.rec.seen[i].ask_bound, ask) << "line " << i + 1;
    EXPECT_EQ(r.rec.seen[i].bid_bound, bid) << "line " << i + 1;
  }
  replay::Parsed<replay::BookRow> full = replay::parse_book_line("103,1,99,1,104,1,98,1", 2);
  replay::PublishedRow p = replay::published_row(full);
  EXPECT_EQ(p.ask_bound, 104);
  EXPECT_EQ(p.bid_bound, 98);
  p = replay::published_row(replay::parse_book_line("103,1,99,1,0,0,98,1", 2));
  EXPECT_FALSE(p.ask_bound.has_value());  // one ask of two: the whole side is visible
  EXPECT_EQ(p.bid_bound, 98);
  p = replay::published_row(replay::parse_book_line("104,1,99,1,103,1,98,1", 2));  // asks out of order
  EXPECT_TRUE(p.malformed);
  EXPECT_FALSE(p.ask_bound.has_value() || p.bid_bound.has_value());
  EXPECT_EQ(replay::published_row(replay::parse_book_line("x", 2)).row, nullptr);
}

TEST(ReplayObserver, LinesBeforeTheStartAreReplayedButNotObserved) {
  // basic_types: observe from line 6's time on; line 6 still sees line 5's row as "before".
  const std::vector<std::string> msgs = lines_of(read_text(find_file("basic_types", "_message_")));
  const std::vector<std::string> rows = lines_of(read_text(find_file("basic_types", "_orderbook_")));
  const std::int64_t t6 = replay::parse_message_line(msgs[5]).value->time_ns;
  ASSERT_LT(replay::parse_message_line(msgs[4]).value->time_ns, t6);
  const ObservedRun plain = replay_fixture("basic_types", 3, std::nullopt, std::nullopt);
  const ObservedRun r = replay_fixture("basic_types", 3, t6, std::nullopt);
  EXPECT_EQ(replay::summary_json(r.summary, std::nullopt), replay::summary_json(plain.summary, std::nullopt));
  ASSERT_FALSE(r.rec.seen.empty());
  EXPECT_EQ(r.rec.seen.front().line, 6u);
  ASSERT_TRUE(r.rec.seen.front().before.has_value());
  EXPECT_EQ(*r.rec.seen.front().before, *replay::parse_book_line(rows[4], 3).value);
  EXPECT_EQ(r.rec.seen.size(), msgs.size() - 5);
}

TEST(ReplayObserver, TheTimeBoundStopsBeforeTheFirstMessageAtOrAfterIt) {
  // burst: lines 6-8 share a time. A bound at that time stops after line 5, and the reader looks
  // at line 6 only: the orderbook text ends there, so reading any further line would be an error
  // (files of different lengths).
  const std::vector<std::string> msgs = lines_of(read_text(find_file("burst", "_message_")));
  const std::vector<std::string> rows = lines_of(read_text(find_file("burst", "_orderbook_")));
  const std::int64_t t6 = replay::parse_message_line(msgs[5]).value->time_ns;
  std::string six_rows;
  for (std::size_t i = 0; i < 6; ++i) six_rows += rows[i] + "\n";
  const ObservedRun r = replay_fixture("burst", 3, 0, t6, six_rows);
  EXPECT_EQ(r.summary.lines, 5u);
  EXPECT_EQ(r.end.reason, replay::StopReason::TimeBound);
  EXPECT_EQ(r.end.bound_line, 6u);
  EXPECT_EQ(r.end.bound_time_ns, t6);
  EXPECT_EQ(r.end.lines, 5u);
  EXPECT_EQ(r.rec.seen.size(), 5u);
  ASSERT_EQ(r.rec.ends.size(), 1u);
  EXPECT_EQ(r.rec.ends[0].bound_line, 6u);
  // Without the bound, the shortened orderbook text is an error: the bound is what kept it unread.
  EXPECT_THROW(replay_fixture("burst", 3, 0, std::nullopt, six_rows), replay::InputError);
  // One nanosecond later the whole burst is inside, and the next line (9, a later time) is the bound.
  const ObservedRun after = replay_fixture("burst", 3, 0, t6 + 1);
  EXPECT_EQ(after.summary.lines, 8u);
  EXPECT_EQ(after.end.bound_line, 9u);
  EXPECT_EQ(after.summary.matching.bursts, 1u);
  EXPECT_EQ(after.summary.matching.burst_messages, 3u);
}

}  // namespace
