// The validation flow (upgrade 3, C5): the verdict rule, the period rules, the leakage check against the
// estimator's reported range, and a short end-to-end validation on a small generated day with a stand-in
// estimator (the real estimator has its own tests; here it only has to report what it read).
#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "calib/calibration.hpp"
#include "calib/estimator.hpp"
#include "calib/validate.hpp"
#include "sim/config.hpp"
#include "sim/lobster_export.hpp"
#include "sim/simulator.hpp"

using namespace calib;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const std::string kSource = LOBSIM_SOURCE_DIR;
constexpr std::int64_t kNs = 1'000'000'000;

// ---- verdicts -------------------------------------------------------------------------------------

TEST(Judge, MatchWhenIntervalsOverlapOrTouch) {
  // data 10 [8, 12]; sim 13 [11, 15] overlaps; sim [12, 14] touches at 12.
  EXPECT_EQ(judge(10, 8, 12, 13, 11, 15).kind, VerdictKind::Match);
  EXPECT_EQ(judge(10, 8, 12, 13, 12, 14).kind, VerdictKind::Match);
  EXPECT_EQ(judge(10, 8, 12, 0.5, 0, 1).kind, VerdictKind::Miss);  // far below: not an overlap
}

TEST(Judge, CloseWhenGapIsWithinTwiceTheHalfWidth) {
  // data half-width 2: a gap of 4 is close (exactly 2 x 2), a gap of 4.5 is a miss.
  const Verdict at_limit = judge(10, 8, 12, 17, 16, 18);
  EXPECT_EQ(at_limit.kind, VerdictKind::Close);
  EXPECT_DOUBLE_EQ(at_limit.gap, 4.0);
  EXPECT_EQ(judge(10, 8, 12, 17.5, 16.5, 18.5).kind, VerdictKind::Miss);
  // The gap on the low side counts the same way: sim [2, 4], data [8, 12]: gap 4.
  EXPECT_EQ(judge(10, 8, 12, 3, 2, 4).kind, VerdictKind::Close);
}

TEST(Judge, MissCarriesTheRatioSimOverData) {
  const Verdict v = judge(10, 9, 11, 40, 39, 41);
  EXPECT_EQ(v.kind, VerdictKind::Miss);
  EXPECT_DOUBLE_EQ(v.ratio, 4.0);
  EXPECT_TRUE(std::isnan(judge(0, -1, 1, 40, 39, 41).ratio));  // data value 0: no ratio
}

TEST(Judge, ZeroWidthDataIntervalNeedsAnOverlap) {
  EXPECT_EQ(judge(5, 5, 5, 5, 5, 5).kind, VerdictKind::Match);
  EXPECT_EQ(judge(5, 5, 5, 6, 6, 6).kind, VerdictKind::Miss);  // gap 1 > 2 x 0
}

TEST(Judge, UndefinedWhenAnyNumberIsMissing) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(judge(nan, 1, 2, 1, 1, 2).kind, VerdictKind::Undefined);
  EXPECT_EQ(judge(1, nan, 2, 1, 1, 2).kind, VerdictKind::Undefined);
  EXPECT_EQ(judge(1, 1, 2, 1, nan, nan).kind, VerdictKind::Undefined);
}

// ---- periods ---------------------------------------------------------------------------------------

TEST(Periods, ParseAndOrder) {
  const Period p = parse_period("36000-45900", "--train");
  EXPECT_EQ(p.from_ns, 36000 * kNs);
  EXPECT_EQ(p.to_ns, 45900 * kNs);
  for (const char* bad : {"", "36000", "a-b", "5-5", "9-3", "-1-5", "1-86401", "1-2-3"})
    EXPECT_THROW(parse_period(bad, "--train"), std::invalid_argument) << bad;
  const Period test = parse_period("45900-55800", "--test");
  EXPECT_NO_THROW(check_period_order(p, test));  // adjacent is fine: [A, B) and [B, D) do not overlap
  EXPECT_THROW(check_period_order(parse_period("36000-46000", "x"), test), std::invalid_argument);  // overlap
  EXPECT_THROW(check_period_order(test, p), std::invalid_argument);                                 // out of order
}

// ---- the leakage check -------------------------------------------------------------------------------

TEST(Leakage, AnObservedRangeInsideTheTrainingPeriodPasses) {
  const Period train{100 * kNs, 200 * kNs}, test{200 * kNs, 300 * kNs};
  EXPECT_NO_THROW(check_estimator_range(100 * kNs, 200 * kNs - 1, train, test));
  EXPECT_NO_THROW(check_estimator_range(150 * kNs, 150 * kNs, train, test));
}

TEST(Leakage, AnObservedRangeThatTouchesTheTestPeriodIsRefused) {
  const Period train{100 * kNs, 200 * kNs}, test{200 * kNs, 300 * kNs};
  EXPECT_THROW(check_estimator_range(100 * kNs, 200 * kNs, train, test), LeakageError);      // the last message is the test's first instant
  EXPECT_THROW(check_estimator_range(100 * kNs, 250 * kNs, train, test), LeakageError);      // ends inside the test period
  EXPECT_THROW(check_estimator_range(250 * kNs, 260 * kNs, train, test), LeakageError);      // entirely inside it
  EXPECT_THROW(check_estimator_range(99 * kNs, 150 * kNs, train, test), LeakageError);       // starts before the training period
  EXPECT_THROW(check_estimator_range(std::nullopt, std::nullopt, train, test), LeakageError);  // nothing to check
}

// ---- baseline configuration ----------------------------------------------------------------------------

TEST(Baseline, NoiseTradersAreTheSectionOfTheDefaultConfig) {
  std::ifstream f(kSource + "/configs/default.json");
  ASSERT_TRUE(f);
  json file = json::parse(f);
  json mine = baseline_noise_traders_json();
  json theirs = file.at("noise_traders");
  EXPECT_EQ(mine, theirs);
}

// ---- a small day and a stand-in estimator ------------------------------------------------------------------

struct SmallDay {
  fs::path dir;
  std::string messages, orderbook;
};

// 300 s of the noise traders alone (tests/fixtures/calib/noise_only_day.json: every spread bucket is visited
// within a short training period; with the market maker the spread is almost always 2 or more), the last 250 s
// exported as a LOBSTER-like day at L = 5: file times 34200 s to 34450 s.
SmallDay make_small_day(const std::string& name) {
  SmallDay d;
  d.dir = fs::temp_directory_path() / ("lobsim_validate_test_" + name + "_" + std::to_string(::getpid()));
  fs::remove_all(d.dir);
  fs::create_directories(d.dir);
  const sim::SimConfig cfg = sim::load_config(kSource + "/tests/fixtures/calib/noise_only_day.json");
  std::stringstream log;
  {
    sim::Simulator s(cfg, sim::SimulatorOptions{&log});
    s.run();
  }
  sim::ExportSpec spec;
  spec.levels = 5;
  spec.filter = true;
  spec.like.on = true;
  spec.like.window_start_ns = 50 * kNs;
  std::stringstream m, b;
  sim::export_order_log(log, spec, m, b);
  const std::string stem = "TST_2012-06-21_34200000_57600000_";
  d.messages = (d.dir / (stem + "message_5.csv")).string();
  d.orderbook = (d.dir / (stem + "orderbook_5.csv")).string();
  std::ofstream(d.messages, std::ios::binary) << m.str();
  std::ofstream(d.orderbook, std::ios::binary) << b.str();
  std::ofstream(d.dir / "README_SYNTHETIC.txt") << "synthetic\n";
  return d;
}

// Reports what it was asked for: the calibration is the hand-made fixture, relabelled; the observed range is
// the requested window, or what `range` says when a test wants to misbehave.
struct StandIn {
  std::string source{"synthetic"};  // what the stand-in's calibration says it is made from
  std::string window_note;          // what it says about its window
  std::function<std::pair<std::int64_t, std::int64_t>(std::int64_t, std::int64_t)> range = [](std::int64_t from, std::int64_t to) {
    return std::make_pair(from, to - 1);
  };
  mutable std::vector<std::pair<std::int64_t, std::int64_t>> windows;  // every window it was asked for
  EstimateFn fn() const {
    return [this](const std::string&, const std::string&, std::int64_t from, std::int64_t to, std::optional<std::int64_t>) {
      windows.emplace_back(from, to);
      EstimatorOutput out;
      out.calibration = load(kSource + "/tests/fixtures/calib/flow_small.json");
      out.calibration.source = source;
      out.calibration.window_note = window_note;
      out.calibration.ticker = "TST";
      out.calibration.median_mid_ticks = 10000;
      const auto r = range(from, to);
      out.first_message_ns = r.first;
      out.last_message_ns = r.second;
      out.summary_markdown = "stand-in estimator\n";
      return out;
    };
  }
};

// One generated day for all the end-to-end tests.
class Validate : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { day_ = new SmallDay(make_small_day("shared")); }
  static void TearDownTestSuite() {
    fs::remove_all(day_->dir);
    delete day_;
    day_ = nullptr;
  }
  static SmallDay* day_;
};
SmallDay* Validate::day_ = nullptr;

ValidateOptions small_options(const SmallDay& d, const std::string& out) {
  ValidateOptions o;
  o.messages = d.messages;
  o.orderbook = d.orderbook;
  o.out_dir = out;
  o.train = parse_period("34200-34330", "train");
  o.test = parse_period("34330-34450", "test");
  o.seeds = 2;
  o.warmup_s = 10.0;
  o.bootstrap_resamples = 30;
  return o;
}

TEST_F(Validate, RunsEndToEndAndWritesTheScorecardInputs) {
  const SmallDay& d = *day_;
  StandIn est;
  const ValidateResult r = run_validation(small_options(d, (d.dir / "out").string()), est.fn());
  EXPECT_TRUE(fs::exists(d.dir / "out" / "validation.json"));
  EXPECT_TRUE(fs::exists(d.dir / "out" / "TST_2012-06-21_34200-34330.json"));
  EXPECT_TRUE(fs::exists(d.dir / "out" / "TST_2012-06-21_34200-34330.md"));
  const json& j = r.json;
  EXPECT_EQ(j.at("label"), "SYNTHETIC");
  EXPECT_EQ(j.at("tick_units"), 1);
  EXPECT_TRUE(j.at("leakage").at("checked").get<bool>());
  // The scorecard has every scalar of the fingerprint: 3 spread/one-sided + 2L depth + 3 vol + 2 + 10 + 3 + 5 + 1 + 6.
  EXPECT_EQ(j.at("scorecard").size(), static_cast<std::size_t>(3 + 2 * 5 + 3 + 1 + 1 + 10 + 3 + 4 + 1 + 1 + 6));
  int total = 0;
  for (const char* k : {"match", "close", "miss", "n/a"}) total += j.at("verdict_counts").at("calibrated").at(k).get<int>();
  EXPECT_EQ(static_cast<std::size_t>(total), j.at("scorecard").size());
  // The estimator was asked for the training window first, and then for the two simulations' export windows;
  // it was never given a window that reaches the test period of the data.
  ASSERT_EQ(est.windows.size(), 3u);
  EXPECT_EQ(est.windows[0].first, 34200 * kNs);
  EXPECT_EQ(est.windows[0].second, 34330 * kNs);
  // The markdown has a row for every scorecard entry, and says the data is synthetic in its title.
  const std::string md = render_markdown(j, false);
  for (const json& row : j.at("scorecard")) EXPECT_NE(md.find("| `" + row.at("name").get<std::string>() + "` |"), std::string::npos) << row.at("name");
  EXPECT_NE(md.find("(SYNTHETIC data, NOT real market data)"), std::string::npos);
  EXPECT_NE(md.find("Trade-sign memory and volatility clustering are expected misses"), std::string::npos);
  fs::remove_all(d.dir / "out");
}

TEST_F(Validate, ResultsDoNotDependOnTheThreadCount) {
  const SmallDay& d = *day_;
  StandIn est;
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.threads = 1;
  const json one = run_validation(o, est.fn()).json;
  o.out_dir = (d.dir / "four").string();
  o.threads = 4;
  const json four = run_validation(o, est.fn()).json;
  EXPECT_EQ(one.dump(), four.dump());
  fs::remove_all(d.dir / "out");
}

TEST_F(Validate, AnEstimatorThatReadsTheTestPeriodAbortsTheRun) {
  const SmallDay& d = *day_;
  StandIn est;
  // Asked for the training window, it reports having read up to 20 s into the test period.
  est.range = [](std::int64_t from, std::int64_t to) { return std::make_pair(from, to + 20 * kNs); };
  const ValidateOptions o = small_options(d, (d.dir / "out").string());
  EXPECT_THROW(run_validation(o, est.fn()), LeakageError);
  EXPECT_FALSE(fs::exists(d.dir / "out" / "validation.json"));  // nothing was simulated or reported
  EXPECT_EQ(est.windows.size(), 1u);                            // and the run stopped right after the estimator
  fs::remove_all(d.dir / "out");
}

// The real estimator: what it reports having read is what the check works from.
EstimateFn real_estimator() {
  return [](const std::string& messages, const std::string& orderbook, std::int64_t from_ns, std::int64_t to_ns,
            std::optional<std::int64_t> tick_units) {
    calib::EstimateOptions eo;
    eo.from_ns = from_ns;
    eo.to_ns = to_ns;
    eo.tick_units = tick_units;
    calib::EstimateResult r = calib::estimate_files(messages, orderbook, eo);
    EstimatorOutput out;
    out.first_message_ns = r.first_message_ns;
    out.last_message_ns = r.last_message_ns;
    out.calibration = std::move(r.calibration);
    return out;
  };
}

TEST_F(Validate, TheRealEstimatorStaysInsideTheTrainingPeriod) {
  const SmallDay& d = *day_;
  const ValidateOptions o = small_options(d, (d.dir / "out").string());
  const EstimatorOutput e = real_estimator()(d.messages, d.orderbook, o.train.from_ns, o.train.to_ns, std::nullopt);
  ASSERT_TRUE(e.first_message_ns && e.last_message_ns);
  EXPECT_GE(*e.first_message_ns, o.train.from_ns);
  EXPECT_LT(*e.last_message_ns, o.train.to_ns);
  EXPECT_NO_THROW(check_estimator_range(e.first_message_ns, e.last_message_ns, o.train, o.test));
}

// The leakage test: an estimator window that ends inside the test period makes the real estimator read test
// messages, and the check must see it in the range the estimator reports.
TEST_F(Validate, TheCheckFiresWhenTheRealEstimatorIsGivenAWindowThatReachesIntoTheTestPeriod) {
  const SmallDay& d = *day_;
  const ValidateOptions o = small_options(d, (d.dir / "out").string());
  const std::int64_t inside_test = o.test.from_ns + 30 * kNs;
  const EstimatorOutput e = real_estimator()(d.messages, d.orderbook, o.train.from_ns, inside_test, std::nullopt);
  ASSERT_TRUE(e.last_message_ns);
  EXPECT_GE(*e.last_message_ns, o.test.from_ns);  // it did read test-period messages
  EXPECT_THROW(check_estimator_range(e.first_message_ns, e.last_message_ns, o.train, o.test), LeakageError);
  // And the whole flow, with the real estimator deliberately handed that window, aborts.
  const EstimateFn leaky = [&](const std::string& m, const std::string& b, std::int64_t from, std::int64_t, std::optional<std::int64_t> t) {
    return real_estimator()(m, b, from, inside_test, t);
  };
  EXPECT_THROW(run_validation(o, leaky), LeakageError);
  EXPECT_FALSE(fs::exists(d.dir / "out" / "validation.json"));
}

TEST_F(Validate, RunsWithTheRealEstimatorAndGivesSurvivalCurves) {
  const SmallDay& d = *day_;
  const ValidateResult r = run_validation(small_options(d, (d.dir / "out").string()), real_estimator());
  const json& s = r.json.at("survival");
  EXPECT_TRUE(s.at("calibrated").is_array());
  EXPECT_TRUE(s.at("baseline").is_array());
  EXPECT_TRUE(r.json.at("leakage").at("passed").get<bool>());
  EXPECT_FALSE(fs::exists(d.dir / "out" / "exports"));  // the exports of the first seed were removed after use
  fs::remove_all(d.dir / "out");
}

TEST_F(Validate, RefusesTrainAndTestThatOverlap) {
  const SmallDay& d = *day_;
  StandIn est;
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.train = parse_period("34200-34400", "train");  // runs into the test period
  EXPECT_THROW(run_validation(o, est.fn()), std::invalid_argument);
  EXPECT_TRUE(est.windows.empty());
  fs::remove_all(d.dir / "out");
}

TEST_F(Validate, ACalibrationFileSkipsTheEstimator) {
  const SmallDay& d = *day_;
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.calibration_file = kSource + "/tests/fixtures/calib/flow_small.json";
  o.start_price = 10000;
  const ValidateResult r = run_validation(o, EstimateFn{});
  EXPECT_FALSE(r.json.at("leakage").at("checked").get<bool>());
  EXPECT_FALSE(r.json.at("calibration").at("estimated").get<bool>());
  EXPECT_TRUE(r.json.at("survival").at("calibrated").is_null());  // no estimator: no simulated survival curves
  fs::remove_all(d.dir / "out");
}

TEST_F(Validate, MissingInputIsDataMissing) {
  StandIn est;
  ValidateOptions o;
  o.messages = "/nonexistent/X_2012-06-21_34200000_57600000_message_5.csv";
  o.orderbook = "/nonexistent/X_2012-06-21_34200000_57600000_orderbook_5.csv";
  o.train = parse_period("34200-34330", "train");
  o.test = parse_period("34330-34450", "test");
  EXPECT_THROW(run_validation(o, est.fn()), DataMissingError);
}

// ---- review fixes ---------------------------------------------------------------------------------------------

// S2: a folder of symbolic links to the files has no README_SYNTHETIC.txt beside it, but the files' own folder has.
TEST_F(Validate, SyntheticDataReachedThroughSymbolicLinksIsStillLabelledSynthetic) {
  const SmallDay& d = *day_;
  const fs::path links = d.dir / "links";
  fs::remove_all(links);
  fs::create_directories(links);
  fs::create_symlink(d.messages, links / fs::path(d.messages).filename());
  fs::create_symlink(d.orderbook, links / fs::path(d.orderbook).filename());
  ASSERT_FALSE(fs::exists(links / "README_SYNTHETIC.txt"));
  StandIn est;
  est.source = "real";  // the calibration alone would call this REAL
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.messages = (links / fs::path(d.messages).filename()).string();
  o.orderbook = (links / fs::path(d.orderbook).filename()).string();
  const ValidateResult r = run_validation(o, est.fn());
  EXPECT_EQ(r.json.at("label"), "SYNTHETIC");
  EXPECT_EQ(r.json.at("data_source"), "synthetic");
  fs::remove_all(d.dir / "out");
  fs::remove_all(links);
}

TEST_F(Validate, TheRealEstimatorMarksSyntheticDataReachedThroughSymbolicLinks) {
  const SmallDay& d = *day_;
  const fs::path links = d.dir / "links2";
  fs::remove_all(links);
  fs::create_directories(links);
  fs::create_symlink(d.messages, links / fs::path(d.messages).filename());
  fs::create_symlink(d.orderbook, links / fs::path(d.orderbook).filename());
  const EstimatorOutput e = real_estimator()((links / fs::path(d.messages).filename()).string(),
                                             (links / fs::path(d.orderbook).filename()).string(), 34200 * kNs, 34330 * kNs, std::nullopt);
  EXPECT_EQ(e.calibration.source, "synthetic");
  fs::remove_all(links);
}

// S3: --calibration FILE estimated from this data's test period is leakage.
TEST_F(Validate, ACalibrationFileEstimatedOnTheTestPeriodOfThisDataIsRefused) {
  const SmallDay& d = *day_;
  calib::EstimateOptions eo;
  eo.from_ns = 34200 * kNs;
  eo.to_ns = 34450 * kNs;  // the whole day: reaches into the test period 34330-34450
  const calib::EstimateResult er = calib::estimate_files(d.messages, d.orderbook, eo);
  const std::string file = (d.dir / "whole_day_calibration.json").string();
  save(file, er.calibration);
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.calibration_file = file;
  o.start_price = 10000;
  EXPECT_THROW(run_validation(o, EstimateFn{}), LeakageError);
  EXPECT_FALSE(fs::exists(d.dir / "out" / "validation.json"));
  // The same file used on a test period its window does not touch is allowed.
  o.train = parse_period("34200-34210", "train");
  o.test = parse_period("34430-34450", "test");
  Calibration early = er.calibration;
  early.window_to_ns = 34330 * kNs;
  save(file, early);
  EXPECT_NO_THROW(run_validation(o, EstimateFn{}));
  fs::remove_all(d.dir / "out");
}

TEST_F(Validate, ACalibrationOfTheSameTickerAndDateOverlappingTheTestPeriodIsRefused) {
  const SmallDay& d = *day_;
  Calibration c = load(kSource + "/tests/fixtures/calib/flow_small.json");
  c.ticker = "TST";
  c.date = "2012-06-21";
  c.window_from_ns = 34200 * kNs;
  c.window_to_ns = 34400 * kNs;  // overlaps the test period; no input hashes at all
  const std::string file = (d.dir / "same_ticker_date.json").string();
  save(file, c);
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.calibration_file = file;
  o.start_price = 10000;
  EXPECT_THROW(run_validation(o, EstimateFn{}), LeakageError);
  // Another ticker with the very same window is the cross-ticker check and stays allowed.
  c.ticker = "OTHER";
  save(file, c);
  EXPECT_NO_THROW(run_validation(o, EstimateFn{}));
  fs::remove_all(d.dir / "out");
}

TEST_F(Validate, TheReportSaysOnlyWhatWasCheckedAboutAGivenCalibration) {
  const SmallDay& d = *day_;
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.calibration_file = kSource + "/tests/fixtures/calib/flow_small.json";
  o.start_price = 10000;
  const ValidateResult r = run_validation(o, EstimateFn{});
  const std::string md = render_markdown(r.json, false);
  EXPECT_EQ(md.find("not estimated from this data"), std::string::npos) << md.substr(0, 1500);
  EXPECT_NE(md.find("flow_small.json"), std::string::npos);
  EXPECT_NE(md.find("ticker TOY"), std::string::npos);
  fs::remove_all(d.dir / "out");
}

// S4: a test period the data does not cover.
std::string refusal_text(const ValidateOptions& o, const EstimateFn& fn) {
  try {
    run_validation(o, fn);
  } catch (const std::runtime_error& e) {
    return e.what();
  }
  return "";
}

TEST_F(Validate, ATestPeriodWithNoMessageIsRefusedBeforeAnyWork) {
  const SmallDay& d = *day_;
  StandIn est;
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.test = parse_period("36000-36100", "test");
  const std::string why = refusal_text(o, est.fn());
  EXPECT_NE(why.find("test period"), std::string::npos) << why;
  EXPECT_TRUE(est.windows.empty());
  EXPECT_FALSE(fs::exists(d.dir / "out" / "validation.json"));
}

TEST_F(Validate, ATestPeriodThatTheDataEndsMoreThanAMinuteBeforeIsRefused) {
  const SmallDay& d = *day_;
  StandIn est;
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.test = parse_period("34330-34600", "test");  // the data ends at about 34450
  const std::string why = refusal_text(o, est.fn());
  EXPECT_NE(why.find("ends"), std::string::npos) << why;
  EXPECT_TRUE(est.windows.empty());
  // A file that ends a few seconds early is fine.
  o.test = parse_period("34330-34470", "test");
  EXPECT_NO_THROW(run_validation(o, est.fn()));
  fs::remove_all(d.dir / "out");
}

TEST_F(Validate, ATestPeriodThatTheDataStartsMoreThanAMinuteAfterIsRefused) {
  const SmallDay& d = *day_;
  StandIn est;
  ValidateOptions o = small_options(d, (d.dir / "out").string());
  o.train = parse_period("34000-34050", "train");
  o.test = parse_period("34100-34300", "test");  // the data starts at about 34200
  const std::string why = refusal_text(o, est.fn());
  EXPECT_NE(why.find("starts"), std::string::npos) << why;
  EXPECT_TRUE(est.windows.empty());
}

TEST_F(Validate, AClippedTrainingWindowNoteReachesTheReport) {
  const SmallDay& d = *day_;
  StandIn est;
  est.window_note = "given window 34200-34330 s, clipped to the data (first message 34200.1 s)";
  const ValidateResult r = run_validation(small_options(d, (d.dir / "out").string()), est.fn());
  EXPECT_EQ(r.json.at("train").at("window_note"), est.window_note);
  EXPECT_NE(render_markdown(r.json, false).find("clipped to the data"), std::string::npos);
  fs::remove_all(d.dir / "out");
}

// S6: only the validation's own export folders are removed.
TEST_F(Validate, TheExportFolderOfTheUserIsNotDeleted) {
  const SmallDay& d = *day_;
  const fs::path out = d.dir / "out_s6";
  fs::remove_all(out);
  fs::create_directories(out / "exports");
  std::ofstream(out / "exports" / "mine.txt") << "keep me\n";
  const ValidateResult r = run_validation(small_options(d, out.string()), real_estimator());
  (void)r;
  EXPECT_TRUE(fs::exists(out / "exports" / "mine.txt"));
  EXPECT_FALSE(fs::exists(out / "exports" / "calibrated"));
  EXPECT_FALSE(fs::exists(out / "exports" / "baseline"));
  fs::remove_all(out);
}

// M8 and the resubmitted order ids.
TEST_F(Validate, AWindowTooShortForTheBootstrapIsSaidNextToTheTable) {
  const SmallDay& d = *day_;
  StandIn est;
  const ValidateResult r = run_validation(small_options(d, (d.dir / "out").string()), est.fn());
  ASSERT_TRUE(r.json.at("bootstrap").at("degenerate").get<bool>());  // 120 s against 300 s blocks
  const std::string md = render_markdown(r.json, false);
  const std::size_t warn = md.find("too short for the block bootstrap");
  const std::size_t table = md.find("| metric |");
  ASSERT_NE(warn, std::string::npos);
  ASSERT_NE(table, std::string::npos);
  EXPECT_LT(warn, table);
  EXPECT_GT(warn + 1200, table);  // next to the table, not in a footnote
  json ok = r.json;
  ok["bootstrap"]["degenerate"] = false;
  EXPECT_EQ(render_markdown(ok, false).find("too short for the block bootstrap"), std::string::npos);
  EXPECT_TRUE(r.json.at("data_checks").contains("resubmitted_order_ids"));
  EXPECT_NE(md.find("resubmitted order ids"), std::string::npos);
  fs::remove_all(d.dir / "out");
}

}  // namespace
