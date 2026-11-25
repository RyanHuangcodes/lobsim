#pragma once
// Out-of-sample validation (upgrade 3, C5): estimate the order flow on a training period of a LOBSTER-format
// day, simulate it (and the old hand-picked noise traders as a baseline), export the simulations to LOBSTER
// files, and compare the market fingerprint (include/calib/fingerprint.hpp) of a different, later test period
// of the data with both. The command line is `lobsim_calibrate validate` (apps/calibrate_validate.cpp);
// everything it does is here, so tests can run it in-process with their own estimator.
//
// The estimator is injected as a function (EstimateFn): the validation never links it. That keeps the leakage
// check testable against an estimator that misbehaves, and the estimator's own code out of this module.
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "calib/calibration.hpp"
#include "calib/fingerprint.hpp"

namespace calib {

// A period [from_ns, to_ns) of the data's clock (nanoseconds after midnight).
struct Period {
  std::int64_t from_ns{0};
  std::int64_t to_ns{0};
  std::int64_t length_ns() const { return to_ns - from_ns; }
};

// "A-B" in whole seconds, 0 <= A < B <= 86400. Throws std::invalid_argument naming `what`.
Period parse_period(const std::string& text, const std::string& what);
// Throws std::invalid_argument unless train.to <= test.from (train strictly before test, no overlap).
void check_period_order(const Period& train, const Period& test);

// The estimator's answer: the calibration, the time range of the message lines it was handed, and its short
// human summary (written beside the JSON).
struct EstimatorOutput {
  Calibration calibration;
  std::optional<std::int64_t> first_message_ns;
  std::optional<std::int64_t> last_message_ns;
  std::string summary_markdown;
};
// Estimates from the message/orderbook files inside [from_ns, to_ns). `tick_units` nullopt: auto.
using EstimateFn = std::function<EstimatorOutput(const std::string& messages, const std::string& orderbook,
                                                 std::int64_t from_ns, std::int64_t to_ns,
                                                 std::optional<std::int64_t> tick_units)>;

// An input file does not exist (the command line exits 3, "data missing").
class DataMissingError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The leakage rule: thrown when what the estimator read is not inside the training period, or touches the
// test period.
class LeakageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
// Checks the estimator's observed message range [first, last] (inclusive times) against the periods:
// it must exist, lie inside [train.from, train.to) and not overlap [test.from, test.to). Throws LeakageError.
void check_estimator_range(std::optional<std::int64_t> first_ns, std::optional<std::int64_t> last_ns,
                           const Period& train, const Period& test);

// The test period is not covered by the data (no message in it, or the data starts or ends more than
// kCoverageSlackS seconds away from it); thrown before any estimation or simulation. Exit code 1.
class TestPeriodError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
// A data file may start up to this many seconds after the test period starts, or end this many seconds before
// it ends (a file that stops a few seconds early is fine; the fingerprint pads the last row to the window end).
constexpr std::int64_t kCoverageSlackS = 60;

struct ValidateOptions {
  std::string messages;
  std::string orderbook;
  std::string out_dir;  // empty: build/validation/<TICKER>_<DATE>/
  Period train;
  Period test;
  int seeds{20};
  std::uint64_t first_seed{1};            // seeds first_seed .. first_seed + seeds - 1, the same for both models
  std::optional<std::int64_t> tick_units;  // nullopt: auto
  std::string calibration_file;            // non-empty: use it, skip the estimation
  std::optional<std::int64_t> start_price; // default: the calibration's median_mid_ticks
  double warmup_s{300.0};                  // simulated before the exported (measured) window
  std::uint32_t owner_count{10'000};
  int threads{0};                          // 0: min(hardware threads, 8)
  int bootstrap_resamples{1000};
  bool keep_exports{false};                // keep the first seed's exported files under <out>/exports/
};

// The baseline's background flow: the `noise_traders` section of configs/default.json (the old hand-picked
// noise traders). A test compares this with the file.
nlohmann::json baseline_noise_traders_json();

// Everything one validation produced. `json` is validation.json; `markdown` is validation.md without the
// plot links (the plots are drawn from `json`, so they come after it).
struct ValidateResult {
  nlohmann::json json;
  Calibration calibration;
  std::string calibration_path;     // the JSON copy in the output directory
  std::string out_dir;
};

using ProgressFn = std::function<void(const std::string&)>;

// The whole flow of spec 6, steps 1-4, writing validation.json, the calibration (JSON and .md, when estimated),
// and returning the result. `estimate` may be empty when options.calibration_file is set. Throws
// std::runtime_error (bad input, simulation or export failure), LeakageError (the leakage rule), or
// std::invalid_argument (bad options).
ValidateResult run_validation(const ValidateOptions& options, const EstimateFn& estimate,
                              const ProgressFn& progress = {});

// ---- scorecard pieces, exposed for tests -------------------------------------------------------------

enum class VerdictKind { Match, Close, Miss, Undefined };
struct Verdict {
  VerdictKind kind{VerdictKind::Undefined};
  double gap{0.0};    // distance between the two intervals (0 when they overlap)
  double ratio{0.0};  // sim mean / data value (a miss only; NaN when the data value is 0)
};
// The spec's rule: match if the sim interval [sim_lo, sim_hi] overlaps the data interval [data_lo, data_hi];
// close if the gap between them is at most twice the data interval's half-width; otherwise miss. Undefined
// when any of the two intervals or the two values is not a finite number.
Verdict judge(double data_value, double data_lo, double data_hi, double sim_mean, double sim_lo, double sim_hi);
const char* to_string(VerdictKind k);

// The report text from the finished json (see run_validation).
std::string render_markdown(const nlohmann::json& validation, bool with_plots);

}  // namespace calib
