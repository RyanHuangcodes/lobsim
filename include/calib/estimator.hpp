#pragma once
// The estimator (upgrade 3, C1): measures the background order flow of one LOBSTER-format day,
// inside a time window, into a calib::Calibration (include/calib/calibration.hpp).
//
// It is an observer on the replayer (replay::ReplayObserver): it sees every message of the window
// with the published orderbook rows before and after it, which are the true book, and never reads
// the replay book. The replay stops before the first message at or after the window end, so no
// message of a later period is ever read (EstimateResult::first_message_ns/last_message_ns say what
// was). Prices and times are integers (file units or ticks, nanoseconds); only rates,
// probabilities, survival values, fits and autocorrelations are floating point.
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "calib/calibration.hpp"

namespace calib {

inline constexpr std::int64_t kNsPerSecond = 1'000'000'000;
// The default window, 10:00 to 15:30: the open and the close behave differently.
inline constexpr std::int64_t kDefaultFromNs = 36'000 * kNsPerSecond;
inline constexpr std::int64_t kDefaultToNs = 55'800 * kNsPerSecond;

struct EstimateOptions {
  // The window [from_ns, to_ns), nanoseconds after midnight. A bound left out takes its default.
  // With neither given and the default window not covered by the data, the data's whole span is
  // used (window_note says so). A given window is clipped to the data's messages.
  std::optional<std::int64_t> from_ns;
  std::optional<std::int64_t> to_ns;
  std::optional<std::int64_t> tick_units;  // file price units per tick; nullopt: auto (see choose_tick)
  // "real" or "synthetic"; nullopt: "synthetic" when README_SYNTHETIC.txt is beside the message file.
  std::optional<std::string> source;
  bool keep_lifetimes{false};  // fill EstimateResult::lifetimes
};

enum class LifetimeEnd : std::uint8_t { Cancelled, Executed, WindowEnd, LeftWindow };
const char* to_string(LifetimeEnd e) noexcept;

// One limit order that entered the lifetime statistics.
struct LifetimeRecord {
  std::int64_t order_id{0};
  int side{kBuy};
  std::int64_t price{0};   // file units
  std::int64_t delta{0};   // placement distance, ticks
  std::int64_t spread{0};  // spread before the order, ticks
  int bucket{0};           // distance_bucket(delta)
  std::int64_t submit_ns{0};
  std::uint64_t submit_line{0};
  LifetimeEnd end{LifetimeEnd::WindowEnd};
  std::int64_t end_ns{0};       // the event or censoring time: the message's time, or the window end
  std::uint64_t end_line{0};    // the line that ended it; 0 at the window end
  std::int64_t lifetime_ns() const { return end_ns - submit_ns; }
};

struct EstimateResult {
  Calibration calibration;
  std::vector<std::string> warnings;  // for the person running it (also in calibration.report)
  // The observed message time range: the earliest and latest time of a message line handed to the
  // estimator (nullopt when none was). It lies inside the window [window_from_ns, window_to_ns).
  std::optional<std::int64_t> first_message_ns;
  std::optional<std::int64_t> last_message_ns;
  std::vector<LifetimeRecord> lifetimes;  // with keep_lifetimes only, in submission order
};

// Input that exists but cannot be estimated (no message line parses, no limit order for the auto
// tick, no two-sided book in the window, an empty window). The command line exits 1 on it.
class EstimateError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// What the estimator is told about the input besides its lines.
struct InputInfo {
  int levels{0};  // L of the orderbook file
  std::string ticker;
  std::string date;
  std::vector<InputFile> inputs;  // file names and SHA-256
  bool readme_synthetic{false};   // README_SYNTHETIC.txt is beside the message file
};

// Estimates from two seekable streams: the message stream is read twice, once for the data's span
// and the tick, then through the replayer. Throws EstimateError, replay::InputError (input that
// cannot be replayed), or std::invalid_argument (options out of range).
EstimateResult estimate(std::istream& messages, std::istream& orderbook, const InputInfo& info,
                        const EstimateOptions& options);

// The same from files. L, ticker and date come from the LOBSTER file names
// (replay::parse_lobster_filename); both files are hashed; README_SYNTHETIC.txt is looked for beside
// the message file. Throws replay::InputError for a file name that is not a LOBSTER name, a level
// count the two names disagree on, or a file that cannot be read.
EstimateResult estimate_files(const std::string& messages_path, const std::string& orderbook_path,
                              const EstimateOptions& options);

// <TICKER>_<DATE>_<from>-<to>: the window in whole seconds after midnight (from rounded down, to up).
std::string output_stem(const Calibration& c);
// The short human summary written beside the JSON file.
std::string summary_markdown(const Calibration& c, const std::vector<std::string>& warnings);

// The git commit the estimator was built from ("-dirty" appended when tracked files had
// uncommitted changes), or "unknown".
std::string build_commit();

// ---- the statistics, exposed for tests and later phases ----------------------------------------

// The auto tick: the GCD of the type-1 prices of the window. A multiple of 100 gives 100 (real
// LOBSTER: $0.01 is 100 units of $0.0001); otherwise the GCD itself, with a warning in *warning.
std::int64_t choose_tick(std::int64_t gcd, std::string* warning);

// The 99.9th percentile by nearest rank: the value at rank n - floor(n / 1000) (1-based) of the n
// samples in ascending order, i.e. the smallest value v with at least ceil(0.999 n) samples <= v.
// Throws std::invalid_argument for an empty Pmf.
std::int64_t percentile_999(const Pmf& p);
// Moves every sample above percentile_999 into its bin; returns the number moved.
std::uint64_t pool_above_999(Pmf& p);

// Kaplan-Meier from the lifetimes that ended in a cancellation (events) and the censored ones. At a
// time with both, the censored orders are still at risk (counted in n_i). Event times must be > 0.
// Fills times_ns, survival, at_risk and events only.
Survival kaplan_meier(std::vector<std::int64_t> event_ns, std::vector<std::int64_t> censored_ns);

// The search range of mu in fit_placement. An estimate at either end means the data is not
// power-law shaped there (on a near-uniform PMF the family tends to an exponential as delta0 and
// mu grow together); the report says so.
inline constexpr double kFitMuLow = -3.0;
inline constexpr double kFitMuHigh = 20.0;

// Discrete maximum-likelihood fit of P(Delta) = (delta0 + Delta)^-(1 + mu) / Z on Delta = 0..Delta_max,
// the largest observed value; `nonneg` holds the samples with Delta >= 0. nullopt with fewer than 2
// samples or a single distinct value.
std::optional<PlacementFit> fit_placement(const Pmf& nonneg);

// Sample autocorrelation of a +1/-1 sequence at `lag`: c_k / c_0 with
// c_k = (1/n) sum_{i < n-k} (x_i - mean)(x_{i+k} - mean). nullopt when n <= lag or c_0 = 0.
std::optional<double> sign_autocorrelation(const std::vector<std::int8_t>& signs, std::size_t lag);

}  // namespace calib
