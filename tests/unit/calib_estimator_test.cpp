// The estimator (upgrade 3, C1) on the hand-written fixtures of tests/fixtures/calib/<case>/ and on
// small generated days. The two worked examples of the spec's section 1.3 are reproduced exactly.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

#include "calib/calibration.hpp"
#include "calib/estimator.hpp"
#include "calib/sha256.hpp"
#include "replay/lobster.hpp"

namespace {

namespace fs = std::filesystem;
using calib::kBuy;
using calib::kSell;
using calib::LifetimeEnd;

constexpr std::int64_t kS = calib::kNsPerSecond;
const fs::path kFixtures = fs::path(LOBSIM_SOURCE_DIR) / "tests" / "fixtures" / "calib";
const std::string kStem = "TEST_2012-06-21_34200000_57600000";

fs::path message_file(const std::string& name) { return kFixtures / name / (kStem + "_message_3.csv"); }
fs::path orderbook_file(const std::string& name) { return kFixtures / name / (kStem + "_orderbook_3.csv"); }

calib::EstimateResult run_fixture(const std::string& name, calib::EstimateOptions o = {}) {
  o.keep_lifetimes = true;
  return calib::estimate_files(message_file(name).string(), orderbook_file(name).string(), o);
}

calib::EstimateOptions window(std::int64_t from_s, std::int64_t to_s) {
  calib::EstimateOptions o;
  o.from_ns = from_s * kS;
  o.to_ns = to_s * kS;
  return o;
}

const calib::LifetimeRecord& record(const calib::EstimateResult& r, std::int64_t id) {
  for (const calib::LifetimeRecord& x : r.lifetimes) {
    if (x.order_id == id) return x;
  }
  ADD_FAILURE() << "no lifetime record for order " << id;
  return r.lifetimes.front();
}

bool has_record(const calib::EstimateResult& r, std::int64_t id) {
  return std::any_of(r.lifetimes.begin(), r.lifetimes.end(),
                     [&](const calib::LifetimeRecord& x) { return x.order_id == id; });
}

// A generated day: messages as given, each orderbook row from the true book after the message.
struct Day {
  std::string messages, orderbook;
};

struct Msg {
  std::int64_t time_ns;
  int type;
  std::int64_t id, size, price;
  int direction;
};

Day make_day(const std::vector<Msg>& msgs, int levels) {
  std::map<std::int64_t, std::int64_t> asks, bids;
  std::map<std::int64_t, std::pair<int, std::int64_t>> price_of;  // id -> (direction, price)
  Day d;
  for (const Msg& m : msgs) {
    auto& side = m.direction > 0 ? bids : asks;
    if (m.type == 1) {
      side[m.price] += m.size;
      price_of[m.id] = {m.direction, m.price};
    } else if (m.type >= 2 && m.type <= 4) {
      side[m.price] -= m.size;
      if (side[m.price] <= 0) side.erase(m.price);
    }
    d.messages += replay::format_message_line(
                      replay::LobsterMsg{m.time_ns, m.type, m.id, m.size, m.price, m.direction}) +
                  "\n";
    std::vector<std::pair<std::int64_t, std::int64_t>> a(asks.begin(), asks.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> b(bids.rbegin(), bids.rend());
    std::string row;
    for (int i = 0; i < levels; ++i) {
      const auto u = static_cast<std::size_t>(i);
      row += i == 0 ? "" : ",";
      row += u < a.size() ? std::to_string(a[u].first) + "," + std::to_string(a[u].second) : "9999999999,0";
      row += ",";
      row += u < b.size() ? std::to_string(b[u].first) + "," + std::to_string(b[u].second) : "-9999999999,0";
    }
    d.orderbook += row + "\n";
  }
  return d;
}

calib::EstimateResult run_day(const Day& d, calib::EstimateOptions o = {}, int levels = 3) {
  std::istringstream m(d.messages), b(d.orderbook);
  calib::InputInfo info;
  info.levels = levels;
  info.ticker = "GEN";
  info.date = "2012-06-21";
  info.readme_synthetic = true;
  return calib::estimate(m, b, info, o);
}

// A two-sided book (sell 100 @ ask, buy 100 @ bid) and then the given type-1 prices, one buy per second.
std::vector<Msg> limit_orders(std::int64_t bid, std::int64_t ask, const std::vector<std::int64_t>& buys) {
  std::vector<Msg> v{{34200 * kS, 1, 1, 100, ask, -1}, {34200 * kS, 1, 2, 100, bid, 1}};
  std::int64_t t = 34201 * kS, id = 10;
  for (const std::int64_t p : buys) v.push_back({t++, 1, id++, 10, p, 1});
  return v;
}

// ---------------------------------------------------------------- section 1.3, Delta

TEST(Placement, WorkedExampleOfSection13) {
  // Bid 100.00, ask 100.03, tick $0.01 = 100 units, so s = 3: a buy at 99.98 is Delta 2, at 100.01
  // Delta -1, at 100.02 Delta -2 = -(s - 1).
  const calib::EstimateResult r = run_fixture("placement_example");
  const calib::Calibration& c = r.calibration;
  EXPECT_EQ(c.tick_units, 100);
  EXPECT_TRUE(r.warnings.empty());
  EXPECT_EQ(record(r, 3).delta, 2);
  EXPECT_EQ(record(r, 4).delta, -1);
  EXPECT_EQ(record(r, 5).delta, -2);
  for (const std::int64_t id : {3, 4, 5}) EXPECT_EQ(record(r, id).spread, 3) << "order " << id;
  const calib::Pmf& p = c.placement[kBuy][calib::spread_bucket(3)];
  EXPECT_EQ(p.values, (std::vector<std::int64_t>{-2, -1, 2}));
  EXPECT_EQ(p.counts, (std::vector<std::uint64_t>{1, 1, 1}));
  for (int b = 0; b < 2; ++b) EXPECT_TRUE(c.placement[kBuy][b].empty());
  for (int b = 0; b < 3; ++b) EXPECT_TRUE(c.placement[kSell][b].empty());
  // Line 1 has no row before it, and line 2's row before shows no bid: both counted, not placed.
  EXPECT_EQ(c.report.at("placement").at("excluded").at("no_previous_row"), 1);
  EXPECT_EQ(c.report.at("placement").at("excluded").at("one_sided_previous_row"), 1);
  EXPECT_EQ(c.report.at("placement").at("samples"), 3);
  EXPECT_EQ(c.rates.limit[kBuy].count, 4u);  // every type-1 counts in the rate, placed or not
  EXPECT_EQ(c.rates.limit[kSell].count, 1u);
}

// ---------------------------------------------------------------- section 1.3, Kaplan-Meier

TEST(KaplanMeier, WorkedExampleOfSection13) {
  // A cancelled at 1 s, B executed at 2 s (censored), C cancelled at 3 s, D alive at the end, 5 s
  // (censored): S = 3/4 at 1 s with n = 4, then 3/8 at 3 s with n = 2.
  const calib::EstimateResult r = run_fixture("km_example", window(34200, 34205));
  const calib::Survival& s = r.calibration.lifetime[calib::distance_bucket(1)];
  EXPECT_EQ(s.times_ns, (std::vector<std::int64_t>{1 * kS, 3 * kS}));
  EXPECT_EQ(s.survival, (std::vector<double>{3.0 / 4.0, 3.0 / 8.0}));
  EXPECT_EQ(s.at_risk, (std::vector<std::uint64_t>{4, 2}));
  EXPECT_EQ(s.events, (std::vector<std::uint64_t>{1, 1}));
  EXPECT_EQ(s.orders, 4u);
  EXPECT_EQ(s.cancelled, 2u);
  EXPECT_EQ(s.censored_executed, 1u);
  EXPECT_EQ(s.censored_window_end, 1u);
  EXPECT_EQ(s.censored_left_window, 0u);
  EXPECT_EQ(s.median_ns(), 3 * kS);
  EXPECT_EQ(record(r, 11).end, LifetimeEnd::Cancelled);
  EXPECT_EQ(record(r, 11).lifetime_ns(), 1 * kS);
  EXPECT_EQ(record(r, 12).end, LifetimeEnd::Executed);
  EXPECT_EQ(record(r, 12).lifetime_ns(), 2 * kS);
  EXPECT_EQ(record(r, 13).lifetime_ns(), 3 * kS);
  EXPECT_EQ(record(r, 14).end, LifetimeEnd::WindowEnd);
  EXPECT_EQ(record(r, 14).lifetime_ns(), 5 * kS);
  for (int b : {0, 2, 3}) EXPECT_EQ(r.calibration.lifetime[b].orders, 0u);
  // The delete of order 2 (excluded: one-sided row before it) is not mistaken for anything else.
  EXPECT_EQ(r.calibration.report.at("lifetime").at("to_an_excluded_order").at("type_3"), 1);
  EXPECT_EQ(r.calibration.report.at("lifetime").at("unknown_id").at("type_3"), 0);
}

TEST(KaplanMeier, CensoredAtAnEventTimeAreStillAtRisk) {
  // Events at 1 and 2, one censoring at 1: at t = 1, n = 3 (the censored order counts), so S = 2/3.
  const calib::Survival s = calib::kaplan_meier({2, 1}, {1});
  EXPECT_EQ(s.times_ns, (std::vector<std::int64_t>{1, 2}));
  EXPECT_EQ(s.at_risk, (std::vector<std::uint64_t>{3, 1}));
  EXPECT_EQ(s.events, (std::vector<std::uint64_t>{1, 1}));
  EXPECT_EQ(s.survival, (std::vector<double>{2.0 / 3.0, 0.0}));
}

TEST(KaplanMeier, TiedEventsAndNoEvents) {
  const calib::Survival s = calib::kaplan_meier({5, 5, 7}, {6, 9});
  EXPECT_EQ(s.times_ns, (std::vector<std::int64_t>{5, 7}));
  EXPECT_EQ(s.at_risk, (std::vector<std::uint64_t>{5, 2}));
  EXPECT_EQ(s.events, (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(s.survival, (std::vector<double>{3.0 / 5.0, 3.0 / 10.0}));
  EXPECT_TRUE(calib::kaplan_meier({}, {1, 2}).times_ns.empty());
  EXPECT_THROW(calib::kaplan_meier({0}, {}), std::invalid_argument);
}

// ---------------------------------------------------------------- leaving the window

TEST(Censoring, AnOrderLeavingThePublishedWindowIsCensoredOnThatRow) {
  // X (id 4, bid 99.98) is the third of three bid levels on line 4: inside. Line 5's new bid pushes
  // it to the fourth level, worse than the published L-th (99.99): censored at line 5's time.
  const calib::EstimateResult r = run_fixture("leave_window");
  const calib::LifetimeRecord& x = record(r, 4);
  EXPECT_EQ(x.end, LifetimeEnd::LeftWindow);
  EXPECT_EQ(x.end_line, 5u);
  EXPECT_EQ(x.end_ns, 34202 * kS);
  EXPECT_EQ(x.lifetime_ns(), 1 * kS);
  // Its later delete belongs to a censored order: neither an event nor an unknown id.
  EXPECT_EQ(r.calibration.report.at("lifetime").at("after_censoring").at("type_3"), 1);
  EXPECT_EQ(r.calibration.report.at("lifetime").at("unknown_id").at("type_3"), 0);
  const calib::Survival& s = r.calibration.lifetime[calib::distance_bucket(2)];
  EXPECT_EQ(s.censored_left_window, 1u);
  EXPECT_TRUE(s.times_ns.empty());
  // Order 3 sits exactly at the published third level on line 5: not worse, so still tracked.
  EXPECT_EQ(record(r, 3).end, LifetimeEnd::WindowEnd);
  EXPECT_EQ(record(r, 5).end, LifetimeEnd::Cancelled);
  EXPECT_EQ(record(r, 5).lifetime_ns(), 3 * kS);
}

TEST(Censoring, ASideShowingFewerThanLLevelsHidesNothing) {
  // Y (id 6, ask 100.07) arrives when the ask side shows 2 of 3 levels (the third written 0,0): the
  // whole side is visible, so Y never leaves. Later it is the third of three asks: still inside.
  const calib::EstimateResult r = run_fixture("leave_window");
  const calib::LifetimeRecord& y = record(r, 6);
  EXPECT_EQ(y.end, LifetimeEnd::WindowEnd);
  EXPECT_EQ(y.end_ns, r.calibration.window_to_ns);
  EXPECT_EQ(record(r, 7).end, LifetimeEnd::WindowEnd);
  std::uint64_t left = 0;
  for (const calib::Survival& s : r.calibration.lifetime) left += s.censored_left_window;
  EXPECT_EQ(left, 1u);  // X only
}

// ---------------------------------------------------------------- window bounds

TEST(Window, OrdersFromBeforeTheWindowAreExcludedAndTheBoundsAreExact) {
  const calib::EstimateResult r = run_fixture("window_bounds", window(34201, 34203));
  const calib::Calibration& c = r.calibration;
  EXPECT_EQ(c.window_from_ns, 34201 * kS);
  EXPECT_EQ(c.window_to_ns, 34203 * kS);
  EXPECT_EQ(c.observed_ns, 2 * kS);
  // The message at exactly --from is observed; the one at exactly --to is not, nor anything after.
  ASSERT_TRUE(r.first_message_ns && r.last_message_ns);
  EXPECT_EQ(*r.first_message_ns, 34201 * kS);
  EXPECT_EQ(*r.last_message_ns, 34202 * kS + 500'000'000);
  EXPECT_EQ(c.report.at("observed").at("first_line"), 4);
  EXPECT_EQ(c.report.at("observed").at("last_line"), 7);
  EXPECT_EQ(c.report.at("observed").at("bound_line"), 8);
  EXPECT_EQ(c.rates.limit[kBuy].count, 1u);  // Q only: P is before the window, line 8 at --to
  EXPECT_EQ(c.rates.limit[kSell].count, 0u);
  EXPECT_DOUBLE_EQ(c.rates.limit[kBuy].per_s, 0.5);
  // P (submitted before the window) and order 2 (a seed of the opening row) are excluded: their
  // messages in the window are counted as unknown ids, never as events or executions.
  EXPECT_FALSE(has_record(r, 3));
  EXPECT_FALSE(has_record(r, 2));
  EXPECT_EQ(c.report.at("lifetime").at("unknown_id").at("type_3"), 1);
  EXPECT_EQ(c.report.at("lifetime").at("unknown_id").at("type_4"), 1);
  EXPECT_EQ(c.report.at("lifetime").at("tracked_orders"), 1);
  const calib::LifetimeRecord& q = record(r, 4);
  EXPECT_EQ(q.end, LifetimeEnd::Cancelled);
  EXPECT_EQ(q.lifetime_ns(), 1 * kS + 500'000'000);
  // The pre-window order's execution is still an aggressive order of the window.
  EXPECT_EQ(c.rates.aggressive[kSell].count, 1u);
  EXPECT_EQ(c.aggressive_size.values, (std::vector<std::int64_t>{30}));
}

// A stream over `text` that records, and fails, any attempt to read past its end: standing for a
// file that goes on into a later period which must stay unread.
class Fence : public std::streambuf {
 public:
  explicit Fence(std::string text) : text_(std::move(text)) { rewind(); }
  bool crossed() const { return crossed_; }

 protected:
  int_type underflow() override {
    crossed_ = true;
    throw std::runtime_error("read past the fence");
  }
  pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override {
    if (off != 0 || dir != std::ios_base::beg) return pos_type(off_type(-1));
    rewind();
    return pos_type(0);
  }
  pos_type seekpos(pos_type pos, std::ios_base::openmode which) override {
    return seekoff(off_type(pos), std::ios_base::beg, which);
  }

 private:
  void rewind() { setg(text_.data(), text_.data(), text_.data() + text_.size()); }
  std::string text_;
  bool crossed_{false};
};

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// The first `lines` lines of a fixture file.
std::string first_lines(const fs::path& p, std::size_t lines) {
  std::istringstream in(read_text(p));
  std::string out;
  std::string l;
  for (std::size_t i = 0; i < lines && std::getline(in, l); ++i) out += l + "\n";
  return out;
}

// Estimates from the first `lines` lines of a fixture, fenced: any read past them is caught.
void expect_unread_after(const std::string& fixture, std::size_t lines, calib::EstimateOptions o) {
  Fence mbuf(first_lines(message_file(fixture), lines));
  Fence bbuf(first_lines(orderbook_file(fixture), lines));
  std::istream m(&mbuf), b(&bbuf);
  calib::InputInfo info;
  info.levels = 3;
  info.ticker = "TEST";
  info.date = "2012-06-21";
  calib::EstimateResult r;
  EXPECT_NO_THROW(r = calib::estimate(m, b, info, o)) << fixture;
  EXPECT_FALSE(mbuf.crossed()) << fixture << ": the message file was read past line " << lines;
  EXPECT_FALSE(bbuf.crossed()) << fixture << ": the orderbook file was read past line " << lines;
  EXPECT_EQ(r.calibration.lifetime, run_fixture(fixture, o).calibration.lifetime) << fixture;
  EXPECT_EQ(r.calibration.rates, run_fixture(fixture, o).calibration.rates) << fixture;
  EXPECT_EQ(r.calibration.report.at("window").at("data_goes_past_the_window_end"), true) << fixture;
}

TEST(Window, NothingAtOrAfterTheWindowEndIsReadBeyondTheBoundLine) {
  // The streams end right after the line that crosses the window end, as if the file went on into
  // a test period: neither the first pass over the messages nor the replay may read on.
  // km_example: line 11 is at exactly 34205 s.
  expect_unread_after("km_example", 11, window(34200, 34205));
  // bursts: a burst at 34202 s (lines 8-9) whose lookahead meets line 10, at the window end 34203 s.
  expect_unread_after("bursts", 10, window(34200, 34203));
}

TEST(Window, TheDataSpanIsUsedWhenTheDefaultWindowIsNotCovered) {
  const calib::EstimateResult r = run_fixture("km_example");
  const calib::Calibration& c = r.calibration;
  EXPECT_EQ(c.window_from_ns, 34200 * kS);
  EXPECT_EQ(c.window_to_ns, 34205 * kS + 1);  // the last message included
  EXPECT_EQ(c.observed_ns, 5 * kS + 1);
  EXPECT_NE(c.window_note.find("whole span"), std::string::npos) << c.window_note;
  EXPECT_EQ(calib::output_stem(c), "TEST_2012-06-21_34200-34206");
}

TEST(Window, AGivenWindowIsClippedToTheDataAndAnEmptyOneIsAnError) {
  const calib::EstimateResult r = run_fixture("km_example", window(34000, 34203));
  EXPECT_EQ(r.calibration.window_from_ns, 34200 * kS);
  EXPECT_EQ(r.calibration.window_to_ns, 34203 * kS);
  EXPECT_NE(r.calibration.window_note.find("clipped"), std::string::npos);
  EXPECT_THROW(run_fixture("km_example", window(40000, 41000)), calib::EstimateError);
  EXPECT_THROW(run_fixture("km_example", window(34203, 34203)), std::invalid_argument);
}

// ---------------------------------------------------------------- tick

TEST(Tick, AutoUsesOneHundredForAGcdThatIsAMultipleOfIt) {
  EXPECT_EQ(calib::choose_tick(100, nullptr), 100);
  std::string w;
  EXPECT_EQ(calib::choose_tick(500, &w), 100);
  EXPECT_TRUE(w.empty());
  const calib::EstimateResult r = run_day(make_day(limit_orders(1000000, 1000500, {999800, 999700}), 3));
  EXPECT_EQ(r.calibration.tick_units, 100);
  EXPECT_TRUE(r.warnings.empty());
}

TEST(Tick, AutoUsesAnotherGcdWithAWarning) {
  std::string w;
  EXPECT_EQ(calib::choose_tick(50, &w), 50);
  EXPECT_NE(w.find("not a multiple of 100"), std::string::npos);
  // Prices in 50-unit steps: GCD 50.
  const calib::EstimateResult r = run_day(make_day(limit_orders(1000000, 1000500, {999950, 999900}), 3));
  EXPECT_EQ(r.calibration.tick_units, 50);
  ASSERT_EQ(r.warnings.size(), 1u);
  EXPECT_NE(r.warnings[0].find("50"), std::string::npos);
  // Prices in simulator ticks, as in the fake day: GCD 1, so tick 1.
  const calib::EstimateResult f = run_day(make_day(limit_orders(9987, 9990, {9986, 9985}), 3));
  EXPECT_EQ(f.calibration.tick_units, 1);
  EXPECT_EQ(f.warnings.size(), 1u);
  EXPECT_EQ(f.calibration.placement[kBuy][calib::spread_bucket(3)].values, (std::vector<std::int64_t>{1, 2}));
}

TEST(Tick, SubpennyPricesAreCountedAndExcluded) {
  calib::EstimateOptions o;
  o.tick_units = 100;
  const calib::EstimateResult r = run_day(make_day(limit_orders(1000000, 1000500, {999900, 999950, 999800}), 3), o);
  const calib::Calibration& c = r.calibration;
  EXPECT_EQ(c.tick_units, 100);
  EXPECT_TRUE(r.warnings.empty());
  EXPECT_EQ(c.report.at("tick").at("subpenny_limit_orders_excluded"), 1);
  EXPECT_EQ(c.rates.limit[kBuy].count, 3u);  // order 2 and the two on the grid
  EXPECT_EQ(c.placement[kBuy][calib::spread_bucket(5)].values, (std::vector<std::int64_t>{1, 2}));
  EXPECT_EQ(c.limit_size.total(), 4u);  // the subpenny order's size is not a sample either
  // A subpenny order at the best makes the next order's reference quote off the grid: counted.
  const calib::EstimateResult b = run_day(make_day(limit_orders(1000000, 1000500, {1000050, 999800}), 3), o);
  EXPECT_EQ(b.calibration.report.at("tick").at("subpenny_limit_orders_excluded"), 1);
  EXPECT_EQ(b.calibration.report.at("placement").at("excluded").at("best_quote_off_tick"), 1);
  EXPECT_EQ(b.calibration.report.at("placement").at("samples"), 0);
}

TEST(Tick, MedianMidIsTheTimeWeightedLowerMedianFlooredToTicks) {
  // Bid 100.00 / ask 100.05 (mid 10002.5 ticks) on six rows within 1 s, then bid 100.01 (mid
  // 10003 ticks) on two rows for 3 s. By time the median is 10003; by rows it would be 10002.
  std::vector<Msg> v{{34200 * kS, 1, 1, 100, 1000500, -1},       {34200 * kS, 1, 2, 100, 1000000, 1},
                     {34200 * kS + 100, 1, 3, 10, 999000, 1},    {34200 * kS + 200, 1, 4, 10, 998000, 1},
                     {34200 * kS + 300, 1, 5, 10, 997000, 1},    {34200 * kS + 400, 1, 6, 10, 996000, 1},
                     {34201 * kS, 1, 7, 100, 1000100, 1},        {34204 * kS, 1, 8, 100, 999900, 1}};
  const calib::EstimateResult r = run_day(make_day(v, 3));
  EXPECT_EQ(r.calibration.median_mid_ticks, 10003);
  EXPECT_EQ(r.calibration.report.at("median_mid").at("two_sided_ns"), 4 * kS + 1);
}

// ---------------------------------------------------------------- bursts and sizes

TEST(Bursts, AggressiveOrdersAreBurstsCountedByTheAggressorSide) {
  const calib::EstimateResult r = run_fixture("bursts");
  const calib::Calibration& c = r.calibration;
  // Lines 5-6: one buy of 60 (Direction -1 is the resting sell). Line 7: other Direction, a sell
  // of 20. Lines 8-9: other time, a sell of 10. Line 11: a sell of 20.
  EXPECT_EQ(c.rates.aggressive[kBuy].count, 1u);
  EXPECT_EQ(c.rates.aggressive[kSell].count, 3u);
  EXPECT_EQ(c.aggressive_size.values, (std::vector<std::int64_t>{10, 20, 60}));
  EXPECT_EQ(c.aggressive_size.counts, (std::vector<std::uint64_t>{1, 2, 1}));
  EXPECT_EQ(c.report.at("aggressive").at("burst_messages"), 6);
  EXPECT_EQ(c.report.at("sign_autocorrelation").at("n"), 4);
  EXPECT_EQ(c.report.at("messages").at("by_type").at("type_4"), 6);
}

TEST(Sizes, NearestRankPercentile) {
  calib::Pmf p{{1, 2, 3}, {998, 1, 1}};  // n = 1000: rank 999 is the 2
  EXPECT_EQ(calib::percentile_999(p), 2);
  EXPECT_EQ(calib::pool_above_999(p), 1u);
  EXPECT_EQ(p.values, (std::vector<std::int64_t>{1, 2}));
  EXPECT_EQ(p.counts, (std::vector<std::uint64_t>{998, 2}));
  calib::Pmf q{{1, 2, 3}, {997, 1, 1}};  // n = 999: rank 999 is the largest, nothing moves
  EXPECT_EQ(calib::pool_above_999(q), 0u);
  EXPECT_EQ(q.values.size(), 3u);
  calib::Pmf one{{7}, {1}};
  EXPECT_EQ(calib::percentile_999(one), 7);
  EXPECT_THROW(calib::percentile_999(calib::Pmf{}), std::invalid_argument);
}

TEST(Sizes, SizesAboveThe999thPercentileArePooledIntoTheTopBin) {
  // 2,000 limit orders: 1,997 of 10, then 20, 500 and 700. Rank 1,998 is the 20, so the 500 and the
  // 700 are counted as 20.
  std::vector<Msg> v{{34200 * kS, 1, 1, 10, 1000500, -1}};
  std::int64_t t = 34201 * kS;
  for (std::int64_t i = 0; i < 1999; ++i) {
    const std::int64_t size = i < 1996 ? 10 : (i == 1996 ? 20 : (i == 1997 ? 500 : 700));
    v.push_back({t, 1, 100 + i, size, 1000000, 1});
    t += 1'000'000;
  }
  const calib::EstimateResult r = run_day(make_day(v, 3));
  const calib::Calibration& c = r.calibration;
  EXPECT_EQ(c.limit_size.values, (std::vector<std::int64_t>{10, 20}));
  EXPECT_EQ(c.limit_size.counts, (std::vector<std::uint64_t>{1997, 3}));
  EXPECT_EQ(c.pooled_limit_size, 2u);
  EXPECT_EQ(c.report.at("sizes").at("limit_size").at("percentile_999"), 20);
}

// ---------------------------------------------------------------- statistics

TEST(PlacementFit, RecoversAKnownPowerLaw) {
  // Counts proportional to (1 + Delta)^-1.6 on 0..30: delta0 = 1, mu = 0.6.
  calib::Pmf p;
  for (std::int64_t d = 0; d <= 30; ++d) {
    p.values.push_back(d);
    p.counts.push_back(static_cast<std::uint64_t>(std::llround(1e9 * std::pow(1.0 + static_cast<double>(d), -1.6))));
  }
  const std::optional<calib::PlacementFit> f = calib::fit_placement(p);
  ASSERT_TRUE(f.has_value());
  EXPECT_NEAR(f->delta0, 1.0, 1e-3);
  EXPECT_NEAR(f->mu, 0.6, 1e-3);
  EXPECT_EQ(f->samples, p.total());
  EXPECT_FALSE(calib::fit_placement(calib::Pmf{{3}, {10}}).has_value());
}

TEST(PlacementFit, TheTailOfZAboveTheExactTermsIsAccurate) {
  // Support 0..2,000: Z's terms above 1,023 go through the Euler-Maclaurin tail. Exact model
  // counts (2 + Delta)^-2.5 give back delta0 = 2 and mu = 1.5.
  calib::Pmf p;
  for (std::int64_t d = 0; d <= 2000; ++d) {
    p.values.push_back(d);
    p.counts.push_back(static_cast<std::uint64_t>(std::llround(1e15 * std::pow(2.0 + static_cast<double>(d), -2.5))));
  }
  const std::optional<calib::PlacementFit> f = calib::fit_placement(p);
  ASSERT_TRUE(f.has_value());
  EXPECT_NEAR(f->delta0, 2.0, 1e-3);
  EXPECT_NEAR(f->mu, 1.5, 1e-3);
}

TEST(SignAutocorrelation, Definition) {
  const std::vector<std::int8_t> alt{1, -1, 1, -1, 1, -1};
  EXPECT_NEAR(*calib::sign_autocorrelation(alt, 1), -5.0 / 6.0, 1e-12);
  EXPECT_NEAR(*calib::sign_autocorrelation(alt, 2), 4.0 / 6.0, 1e-12);
  EXPECT_FALSE(calib::sign_autocorrelation(alt, 6).has_value());
  EXPECT_FALSE(calib::sign_autocorrelation({1, 1, 1}, 1).has_value());  // c_0 = 0
}

// ---------------------------------------------------------------- the file and its inputs

TEST(Output, TheCalibrationRoundTripsThroughSaveAndLoad) {
  const calib::EstimateResult r = run_fixture("km_example", window(34200, 34205));
  const fs::path dir = fs::path(::testing::TempDir()) / "calib_estimator_roundtrip";
  fs::create_directories(dir);
  const fs::path path = dir / (calib::output_stem(r.calibration) + ".json");
  EXPECT_EQ(path.filename().string(), "TEST_2012-06-21_34200-34205.json");
  calib::save(path.string(), r.calibration);
  const calib::Calibration back = calib::load(path.string());
  EXPECT_EQ(back, r.calibration);
  EXPECT_NO_THROW(calib::validate(back));
  EXPECT_EQ(back.source, "synthetic");  // README_SYNTHETIC.txt beside the fixture
  EXPECT_EQ(back.ticker, "TEST");
  EXPECT_EQ(back.date, "2012-06-21");
  EXPECT_EQ(back.levels, 3);
  ASSERT_EQ(back.inputs.size(), 2u);
  EXPECT_EQ(back.inputs[0].name, kStem + "_message_3.csv");
  EXPECT_EQ(back.inputs[0].sha256, calib::sha256_file(message_file("km_example").string()));
  EXPECT_EQ(back.inputs[1].name, kStem + "_orderbook_3.csv");
  EXPECT_FALSE(back.code_commit.empty());
  const std::string md = calib::summary_markdown(r.calibration, r.warnings);
  EXPECT_NE(md.find("SYNTHETIC"), std::string::npos);
}

TEST(Output, SourceFollowsTheReadmeUnlessGiven) {
  const fs::path dir = fs::path(::testing::TempDir()) / "calib_estimator_source";
  fs::create_directories(dir);
  for (const fs::path& f : {message_file("km_example"), orderbook_file("km_example")}) {
    fs::copy_file(f, dir / f.filename(), fs::copy_options::overwrite_existing);
  }
  const std::string m = (dir / message_file("km_example").filename()).string();
  const std::string b = (dir / orderbook_file("km_example").filename()).string();
  EXPECT_EQ(calib::estimate_files(m, b, {}).calibration.source, "real");  // no README_SYNTHETIC.txt
  calib::EstimateOptions o;
  o.source = "synthetic";
  EXPECT_EQ(calib::estimate_files(m, b, o).calibration.source, "synthetic");
  o.source = "made-up";
  EXPECT_THROW(calib::estimate_files(m, b, o), std::invalid_argument);
}

// A ticker that starts with SYNTH is made up (scripts/make_fake_day.sh): its files are synthetic wherever they are.
TEST(Output, ASynthTickerIsSyntheticEvenWithoutAReadme) {
  const fs::path dir = fs::path(::testing::TempDir()) / "calib_estimator_synth_ticker";
  fs::create_directories(dir);
  const auto renamed = [](const fs::path& f) {
    const std::string n = f.filename().string();
    return "SYNTHX" + n.substr(n.find('_'));
  };
  const fs::path m = dir / renamed(message_file("km_example")), b = dir / renamed(orderbook_file("km_example"));
  fs::copy_file(message_file("km_example"), m, fs::copy_options::overwrite_existing);
  fs::copy_file(orderbook_file("km_example"), b, fs::copy_options::overwrite_existing);
  ASSERT_FALSE(fs::exists(dir / "README_SYNTHETIC.txt"));
  EXPECT_EQ(calib::estimate_files(m.string(), b.string(), {}).calibration.source, "synthetic");
}

TEST(Sha256, KnownVectors) {
  EXPECT_EQ(calib::sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(calib::sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(calib::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  calib::Sha256 s;  // a million 'a', in uneven pieces
  const std::string chunk(997, 'a');
  std::size_t left = 1'000'000;
  while (left > 0) {
    const std::size_t n = std::min(left, chunk.size());
    s.update(chunk.data(), n);
    left -= n;
  }
  EXPECT_EQ(s.hex(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(LobsterFileName, Parts) {
  const auto f = replay::parse_lobster_filename("data/lobster/AAPL_2012-06-21_34200000_57600000_message_10.csv");
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->ticker, "AAPL");
  EXPECT_EQ(f->date, "2012-06-21");
  EXPECT_EQ(f->start_ms, 34200000);
  EXPECT_EQ(f->end_ms, 57600000);
  EXPECT_EQ(f->kind, "message");
  EXPECT_EQ(f->levels, 10);
  const auto g = replay::parse_lobster_filename("BRK_A_2012-06-21_1_2_orderbook_5.csv");
  ASSERT_TRUE(g.has_value());
  EXPECT_EQ(g->ticker, "BRK_A");
  for (const char* bad : {"AAPL_2012-6-21_34200000_57600000_message_10.csv", "AAPL_2012-06-21_34200000_57600000_trades_10.csv",
                          "_2012-06-21_1_2_message_1.csv", "AAPL_2012-06-21_1_2_message_0.csv",
                          "AAPL_2012-06-21_1_2_message_10.txt", "2012-06-21_1_2_message_1.csv"}) {
    EXPECT_FALSE(replay::parse_lobster_filename(bad).has_value()) << bad;
  }
}

}  // namespace
