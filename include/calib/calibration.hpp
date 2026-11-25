#pragma once
// The calibration file: what the estimator (`lobsim_calibrate estimate`) measures from one
// LOBSTER-format day, and what the calibrated background flow (CalibratedFlowAgent) reads.
// The JSON layout is documented in calibration/SCHEMA.md.
//
// Units: prices, placement distances and spreads in ticks; sizes in shares; times in integer
// nanoseconds. Only rates, probabilities, survival values and fitted parameters are floating
// point, and none of them is ever turned back into a price or a time except through an integer
// table (see CalibratedFlowAgent).
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace calib {

inline constexpr int kSchemaVersion = 1;

// Side index used by every per-side array in this file: 0 = buy, 1 = sell.
inline constexpr int kBuy = 0;
inline constexpr int kSell = 1;

// An empirical distribution on integers. `values` is strictly increasing and every count is
// positive; counts[i] is the number of samples equal to values[i] (a hand-made file may use any
// positive weights). An empty Pmf (no values) means "no samples".
struct Pmf {
  std::vector<std::int64_t> values;
  std::vector<std::uint64_t> counts;
  std::uint64_t total() const;  // the sum of counts (validate() checks it does not overflow)
  bool empty() const { return values.empty(); }
  bool operator==(const Pmf&) const = default;
};

// Spread buckets: s = 1, s = 2, s >= 3 (spread in ticks, s >= 1).
inline constexpr int kSpreadBuckets = 3;
int spread_bucket(std::int64_t spread_ticks);  // throws std::invalid_argument for s < 1
const char* spread_bucket_name(int bucket);    // "1", "2", "3+"

// Distance buckets for lifetimes and fill probabilities: Delta <= 0, 1-2, 3-9, >= 10 (ticks).
inline constexpr int kDistanceBuckets = 4;
int distance_bucket(std::int64_t delta_ticks);
const char* distance_bucket_name(int bucket);  // "<=0", "1-2", "3-9", ">=10"

struct Rate {
  std::uint64_t count{0};  // events in the observed time (0 in a hand-made file)
  double per_s{0.0};       // events per second: count / observed seconds, or stated by hand
  bool operator==(const Rate&) const = default;
};

struct Rates {
  std::array<Rate, 2> limit{};       // type-1 submissions, by the order's side
  std::array<Rate, 2> aggressive{};  // bursts of type-4 messages, by the aggressor's side (the
                                     // opposite of the LOBSTER Direction, which is the resting side)
  bool operator==(const Rates&) const = default;
};

// Kaplan-Meier survival of limit-order lifetimes (submission to full cancel) in one distance
// bucket. times_ns are the distinct lifetimes at which at least one cancellation happened,
// ascending and positive; survival[i] = S(times_ns[i]) = prod_{j <= i} (1 - events[j] / at_risk[j]),
// non-increasing, in [0, 1]. S(t) = 1 before times_ns[0]. at_risk and events are either both empty
// (a hand-made curve) or the same length as times_ns.
struct Survival {
  std::vector<std::int64_t> times_ns;
  std::vector<double> survival;
  std::vector<std::uint64_t> at_risk;  // n_i
  std::vector<std::uint64_t> events;   // d_i
  std::uint64_t orders{0};             // orders that entered this bucket
  std::uint64_t cancelled{0};          // lifetimes ended by a full delete (type 3)
  std::uint64_t censored_executed{0};  // fully executed first
  std::uint64_t censored_window_end{0};
  std::uint64_t censored_left_window{0};  // priced worse than the published L-th level on its side
  // The smallest times_ns[i] with survival[i] <= 0.5; nullopt if the curve never gets there.
  std::optional<std::int64_t> median_ns() const;
  bool operator==(const Survival&) const = default;
};

// P(Delta) proportional to 1 / (delta0 + Delta)^(1 + mu) for Delta >= 0. Reported, never used by
// the simulator (it samples the empirical PMF).
struct PlacementFit {
  double delta0{0.0};
  double mu{0.0};
  std::uint64_t samples{0};
  std::string method;  // how it was fitted, in words
  bool operator==(const PlacementFit&) const = default;
};

struct InputFile {
  std::string name;    // file name only, never a full path
  std::string sha256;  // lowercase hex
  bool operator==(const InputFile&) const = default;
};

struct Calibration {
  int schema_version{kSchemaVersion};
  std::string source;  // "real", "synthetic" or "hand-made": every number is labelled by this
  std::string ticker;
  std::string date;
  // The estimator's window, in nanoseconds after midnight ([from, to)), and the time the rates
  // are divided by. window_note says how the window was chosen (e.g. the data's whole span).
  std::int64_t window_from_ns{0};
  std::int64_t window_to_ns{0};
  std::int64_t observed_ns{0};
  std::string window_note;
  std::vector<InputFile> inputs;
  std::string code_commit;  // git commit of the estimator, or "unknown"
  int levels{0};            // L of the orderbook file (0 in a hand-made file)
  std::int64_t tick_units{1};        // file price units per tick
  std::int64_t median_mid_ticks{0};  // floor of the median mid-price, in ticks: the simulator's start price
  Rates rates;
  // [side][spread bucket]; values are placement distances Delta in ticks (>= -(s - 1)).
  std::array<std::array<Pmf, kSpreadBuckets>, 2> placement{};
  std::array<std::optional<PlacementFit>, 2> placement_fit{};  // [side], report only
  // Sizes in shares. Sizes above the 99.9th percentile are counted in the top bin (pooled_* says
  // how many samples were moved there).
  Pmf limit_size;
  Pmf aggressive_size;  // the total size of each aggressive order (sum over its burst)
  std::uint64_t pooled_limit_size{0};
  std::uint64_t pooled_aggressive_size{0};
  std::array<Survival, kDistanceBuckets> lifetime{};
  // Diagnostics and validation targets (counters, excluded messages, sign autocorrelation, ...).
  // Written for people and for the validation report; never read by the simulator.
  nlohmann::json report = nlohmann::json::object();
  bool operator==(const Calibration&) const = default;
};

// Throws std::runtime_error naming the offending field (e.g. "placement[buy][2].values: not
// strictly increasing") for any violation of the rules stated above.
void validate(const Calibration& c);
nlohmann::json to_json(const Calibration& c);
Calibration from_json(const nlohmann::json& j);  // validates; unknown keys are an error
Calibration load(const std::string& path);       // throws std::runtime_error naming the path
// Writes to_json(c) with 2-space indentation and a trailing newline. The same Calibration always
// gives the same bytes.
void save(const std::string& path, const Calibration& c);

// Distances between two distributions on integers, both normalized by their totals. Both throw
// std::invalid_argument if either Pmf is empty.
double total_variation(const Pmf& p, const Pmf& q);  // 1/2 sum_x |P(x) - Q(x)|
double wasserstein1(const Pmf& p, const Pmf& q);     // sum over consecutive integers x of |F_P(x) - F_Q(x)|

}  // namespace calib
