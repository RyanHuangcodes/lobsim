#pragma once
// The market fingerprint: one statistics module for LOBSTER-format files, run on data and on exported
// simulations alike (lobsim_export_lobster --lobster-like --filter-window --price-scale <tick_units>).
// It computes the scorecard of spec 5 and the uncertainty of every scalar in it.
//
// Conventions (the full list of estimator definitions is in docs/FINGERPRINT.md and DECISIONS.md):
//   * Prices and times are integers (LOBSTER price units, nanoseconds after midnight) until the log
//     of a mid price is taken. Everything else is a statistic and may be a double.
//   * Row k of the orderbook file is the book right after message k. The book "at time t" is the row
//     of the last message at or before t. A level of size 0 is empty, whatever its price.
//   * The window is [from, to) in nanoseconds. Only messages inside it count; the book that is in
//     force when the window opens is the row of the last message before it (a carry-in, used for time
//     weights, fixed-clock mids and placement distances, never counted as an event).
//   * Statistics are computed from per-second sufficient statistics ("bins" of the window); the point
//     estimate is the same code run on all bins, and a bootstrap resample is the same code run on a
//     recombination of block sums. So an interval can never disagree with its own point estimate.
#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "calib/calibration.hpp"

namespace calib {

inline constexpr int kFingerprintClocks = 3;   // fixed clocks of the volatility metric
inline constexpr int kAbsAcfLags = 10;         // autocorrelation of |r|, lags 1..10 (10 s returns)
inline constexpr int kSignAcfLags = 3;         // trade-sign autocorrelation lags
inline constexpr std::array<int, kFingerprintClocks> kClockSeconds{1, 10, 60};
inline constexpr std::array<int, kSignAcfLags> kSignLags{1, 10, 100};

struct FingerprintWindow {
  std::optional<std::int64_t> from_ns;  // default: the time of the first message
  std::optional<std::int64_t> to_ns;    // default: one nanosecond after the last message
};

struct FillStat {
  std::uint64_t orders{0};   // limit orders submitted in the window (in this distance bucket)
  std::uint64_t filled{0};   // of them, at least partly executed by a type 4 on their id in the window
  double prob{0.0};          // filled / orders; NaN if there are no orders
};

struct VolStat {
  int clock_s{0};
  double stdev{0.0};           // sample std (n - 1) of the log mid returns; NaN if n < 2
  std::uint64_t n{0};          // returns used
  std::uint64_t skipped{0};    // clock intervals without a two-sided mid at both ends
};

struct Fingerprint {
  // ---- what was measured
  int levels{0};                    // L, the number of levels of the orderbook file
  std::int64_t tick_units{1};
  std::int64_t from_ns{0}, to_ns{0};
  std::int64_t total_ns{0};         // window time covered: to - from for the whole window
  std::uint64_t messages{0};        // messages inside the window
  // ---- time accounting (nanoseconds; the three states partition total_ns)
  std::int64_t no_state_ns{0};      // before any row existed (only when the window opens before the file)
  std::int64_t empty_ns{0};         // both sides empty
  std::int64_t one_sided_ns{0};     // exactly one side empty
  std::int64_t two_sided_ns{0};     // both sides non-empty; the spread PMF has exactly this weight
  std::int64_t crossed_ns{0};       // of two_sided_ns: ask <= bid (spread <= 0), kept in the PMF
  // ---- spread: time-weighted PMF in ticks, weights in nanoseconds (two-sided time only)
  Pmf spread_pmf;
  double spread_mean{0.0};          // ticks; NaN if the PMF is empty
  double spread_one_tick_share{0.0};  // share of two-sided time at spread 1 tick
  double one_sided_share{0.0};      // one_sided_ns / total_ns
  // ---- depth: time-weighted mean size (shares) at level 1..L; [kBuy|kSell][level - 1]; time with a
  // book (total_ns - no_state_ns) is the weight; an empty level counts 0.
  std::array<std::vector<double>, 2> depth;
  // ---- returns on fixed clocks 1 s, 10 s, 60 s
  std::array<VolStat, kFingerprintClocks> vol;
  double kurtosis_10s{0.0};         // excess kurtosis (m4 / m2^2 - 3, population moments) of the 10 s returns
  double acf_ret_1s_lag1{0.0};
  std::array<double, kAbsAcfLags> acf_abs_10s{};          // lags 1..10
  // ---- aggressive orders = bursts; sign +1 = the aggressor buys
  std::uint64_t bursts{0};
  std::array<double, kSignAcfLags> sign_acf{};           // lags 1, 10, 100 (in bursts)
  // ---- limit orders submitted in the window
  std::array<FillStat, kDistanceBuckets> fill;           // by placement distance bucket
  FillStat fill_all;                                     // every order, including those with no distance
  std::uint64_t placement_unknown{0};                    // orders with no best price on their side before them
  std::uint64_t submitted_shares{0};
  std::uint64_t executed_shares{0};
  double executed_share{0.0};       // executed_shares / submitted_shares
  // ---- sanity: input rates re-measured (events per second of window time)
  std::array<double, 2> lambda_limit{};   // type 1 messages by side
  double lambda_limit_total{0.0};
  std::array<double, 2> lambda_market{};  // bursts by the aggressor's side
  double lambda_market_total{0.0};
  // ---- what the whole file showed (not additive, so copied unchanged into every derived fingerprint)
  std::uint64_t off_grid_rows{0};         // two-sided rows whose spread is not a whole number of ticks (truncated)
  std::uint64_t duplicate_order_ids{0};   // type 1 messages whose id is still resting (the first order keeps it)
  std::uint64_t reused_order_ids{0};      // type 1 messages whose id's earlier order in the window had ended: new orders
};

// One scalar of the scorecard, by name. NaN = undefined (no data for it).
struct NamedScalar {
  std::string name;
  double value;
};
// Every scalar metric of a fingerprint, in a fixed order with fixed names:
//   spread_mean, spread_one_tick_share, one_sided_share, depth_buy_L1..Ln, depth_sell_L1..Ln,
//   vol_1s, vol_10s, vol_60s, kurtosis_10s, acf_ret_1s_lag1, acf_abs_10s_lag1..10,
//   sign_acf_lag1/10/100, fill_le0, fill_1_2, fill_3_9, fill_ge10, fill_all, executed_share,
//   lambda_limit_buy/sell/total, lambda_market_buy/sell/total.
std::vector<NamedScalar> scalars(const Fingerprint& f);

nlohmann::json to_json(const Fingerprint& f);
void write_json(std::ostream& out, const Fingerprint& f);  // 2-space indentation, trailing newline

// ---- uncertainty ---------------------------------------------------------------------------

struct BootstrapOptions {
  int block_seconds{300};      // moving-block length (5 minutes)
  int resamples{1000};
  std::uint64_t seed{20120621};
};

struct ScalarInterval {
  std::string name;
  double value;       // the point estimate
  double lo, hi;      // 2.5 and 97.5 percentiles of the resampled values; NaN if fewer than 20 are defined
  double se;          // standard deviation of the resampled values
  std::uint32_t defined;  // resamples in which the scalar was defined
};

struct BootstrapResult {
  BootstrapOptions options;
  int blocks_per_resample{0};
  int block_seconds_used{0};  // min(options.block_seconds, window seconds)
  bool degenerate{false};     // the window is no longer than one block: every resample is the data itself
  std::vector<ScalarInterval> scalars;
};

// A distance between two distributions on integers (spread PMF in ticks; depth profile over level
// numbers). W1 in the unit of the variable, TV in [0, 1]. valid = false if either side is empty.
struct Distance {
  double w1{0.0}, tv{0.0};
  bool valid{false};
};

struct Spread {  // the point value and the bootstrap spread of a statistic
  double value{0.0};
  double mean{0.0}, se{0.0}, lo{0.0}, hi{0.0};  // lo/hi: 2.5 and 97.5 percentiles
  std::uint32_t defined{0};
};

struct DistributionDistances {
  Distance spread;                          // spread PMFs
  std::array<Distance, 2> depth;            // depth profiles per side (normalized shape over levels 1..L)
  // The depth profile is turned into a Pmf over level numbers 1..L with counts = round(1000 * mean
  // size), levels with 0 mean size left out, so W1 is in levels.
};

// The yardstick "how far is too far": the same distances between the first and the second half of the
// window (halves of the bins), with the bootstrap spread of each distance (each half resampled on
// its own, with the same block length).
struct HalvesYardstick {
  DistributionDistances point;
  Spread spread_w1, spread_tv;
  std::array<Spread, 2> depth_w1, depth_tv;
  int blocks_per_half[2]{0, 0};
};

// Distances between two fingerprints (data vs simulation, or any two). The depth distances need the
// same L on both sides.
DistributionDistances distances(const Fingerprint& a, const Fingerprint& b);

// ---- simulation runs -----------------------------------------------------------------------

struct RunSummary {
  std::string name;
  std::uint32_t n{0};         // runs in which the scalar was defined
  double mean{0.0}, sd{0.0};  // sd: sample (n - 1)
  // The interval for the MEAN over runs: mean +- t(0.975, n - 1) * sd / sqrt(n). This is the one to
  // compare with the data's bootstrap interval (both are intervals for a "typical" value).
  double ci_lo{0.0}, ci_hi{0.0};
  // The range the runs themselves cover: empirical 2.5 and 97.5 percentiles (linear interpolation
  // between order statistics). Wider than the CI; shows run-to-run scatter.
  double run_lo{0.0}, run_hi{0.0};
};
// One summary per scalar name of scalars(); runs must share L. Throws if `runs` is empty or L differs.
std::vector<RunSummary> combine_runs(const std::vector<Fingerprint>& runs);
// Two-sided 97.5% quantile of Student's t with `df` degrees of freedom (df >= 1).
double student_t975(int df);

nlohmann::json to_json(const BootstrapResult& b);
nlohmann::json to_json(const std::vector<RunSummary>& runs);
nlohmann::json to_json(const DistributionDistances& d);
nlohmann::json to_json(const HalvesYardstick& h);

// ---- the pass over the files -----------------------------------------------------------------

// All the per-second sufficient statistics of one window of one day. Build it once (one pass over the
// two files), then ask for the fingerprint, the bootstrap and the halves yardstick without reading
// the files again.
class Prepared {
 public:
  // Both streams are read to their end (or until a message at or after the window's end). The row
  // count must equal the message count. `levels` is read from the first row. Throws
  // std::runtime_error naming the line on a malformed line, a time that goes backwards, or a
  // mismatch between the two files.
  static Prepared from_streams(std::istream& messages, std::istream& orderbook, std::int64_t tick_units,
                               const FingerprintWindow& window = {});
  static Prepared from_files(const std::string& message_path, const std::string& orderbook_path,
                             std::int64_t tick_units, const FingerprintWindow& window = {});

  Prepared(Prepared&&) noexcept;
  Prepared& operator=(Prepared&&) noexcept;
  ~Prepared();

  int bins() const;  // seconds in the window (the last may be partial)
  Fingerprint fingerprint() const;                            // the whole window
  Fingerprint fingerprint_of_bins(int first, int last) const;  // bins [first, last)
  BootstrapResult bootstrap(const BootstrapOptions& options = {}) const;
  HalvesYardstick halves(const BootstrapOptions& options = {}) const;

 struct Impl;  // internal (src/calib/fingerprint_impl.hpp)

 private:
  explicit Prepared(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Convenience: the whole-window fingerprint of two streams / files, without uncertainty.
Fingerprint compute_fingerprint(std::istream& messages, std::istream& orderbook, std::int64_t tick_units,
                                const FingerprintWindow& window = {});
Fingerprint compute_fingerprint(const std::string& message_path, const std::string& orderbook_path,
                                std::int64_t tick_units, const FingerprintWindow& window = {});

}  // namespace calib
