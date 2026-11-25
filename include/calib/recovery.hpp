#pragma once
// Parameter recovery (upgrade 3, C3): compares a calibration estimated from a simulated day with
// the hand-made truth the simulation sampled (calibration/recovery_truth.json), by the rules of
// spec section 4.3. Rates, placement PMFs, size PMFs and lifetime medians; every row says what it
// compared, with which measure, against which tolerance.
#include <array>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "calib/calibration.hpp"

namespace calib {

struct CompareOptions {
  double tolerance_scale{1.0};      // every tolerance is multiplied by this (the CI run uses 2)
  std::uint64_t min_samples{2000};  // a placement bucket with fewer estimated samples is skipped
  // Filtered mode: rates and the 3+ placement are reported, not judged; placement is compared on
  // Delta <= max_delta[side] only (nullopt for a side: no limit), with the tail above it reported;
  // lifetime medians get the filtered tolerance.
  bool filtered{false};
  std::array<std::optional<std::int64_t>, 2> max_delta{};
};

// The tolerances of spec section 4.3, before tolerance_scale.
inline constexpr double kRateTolerance = 0.03;              // relative
inline constexpr double kPlacementTolerance = 0.03;         // total variation
inline constexpr double kSizeTolerance = 0.02;              // total variation
inline constexpr double kLifetimeTolerance = 0.10;          // relative, unfiltered
inline constexpr double kLifetimeFilteredTolerance = 0.20;  // relative, filtered

enum class Verdict : std::uint8_t { Pass, Fail, Info, Skipped };
const char* to_string(Verdict v) noexcept;

struct CompareRow {
  std::string group;     // "rate", "placement", "placement tail", "size", "lifetime median", "fit"
  std::string item;      // e.g. "limit buy", "buy s=2", "limit_size", "<=0"
  std::string truth;     // the truth's side of the comparison, as text
  std::string estimate;  // the estimate's side
  std::string measure;   // what `value` is
  double value{0.0};
  std::optional<double> tolerance;  // |value| must be <= this; nullopt: informational or skipped
  Verdict verdict{Verdict::Info};
  std::string note;
};

struct Comparison {
  std::vector<CompareRow> rows;
  std::size_t count(Verdict v) const;
  std::size_t passed_in(const std::string& group) const;
  bool passed() const { return count(Verdict::Fail) == 0; }
};

// Throws std::invalid_argument for options out of range.
Comparison compare_recovery(const Calibration& truth, const Calibration& estimate, const CompareOptions& options);

// A fixed-width table for a terminal, and the same as Markdown; both end with the summary line
// "result: PASS|FAIL (P passed, F failed, I informational, S skipped)".
std::string comparison_text(const Comparison& c);
std::string comparison_markdown(const Comparison& c);
std::string comparison_summary(const Comparison& c);

// The samples of p with lo <= value and, when hi is given, value <= hi.
Pmf restrict_pmf(const Pmf& p, std::int64_t lo, std::optional<std::int64_t> hi);

// The depth of the published window, per side (0 = buy/bids, 1 = sell/asks): over the rows of an
// orderbook file where that side shows all L levels, the distance in ticks from the best level
// to the L-th, (best bid - L-th bid) / tick or (L-th ask - best ask) / tick, rounded down; p95 is
// its 95th percentile by nearest rank (the value at rank n - floor(n / 20) of the n sorted rows).
// A side that never shows L levels is never cut by the window: p95 nullopt. Malformed rows are
// skipped and counted; a row that does not parse is an error (std::runtime_error naming the line).
struct WindowDepth {
  int levels{0};
  std::array<std::optional<std::int64_t>, 2> p95{};
  std::array<std::uint64_t, 2> rows{};          // rows with all L levels on that side
  std::array<std::uint64_t, 2> fewer_levels{};  // rows where that side shows fewer than L
  std::uint64_t malformed{0};
};
WindowDepth window_depth(std::istream& orderbook, int levels, std::int64_t tick_units);

}  // namespace calib
