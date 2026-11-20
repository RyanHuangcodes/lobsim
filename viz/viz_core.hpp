#pragma once

// lobsim_viz_export core logic (upgrade 4, phase V1). Everything that the unit tests exercise
// lives here so the CLI (viz/export_main.cpp) is a thin wrapper. No simulator (src/sim) or
// upgrade-2 replay (src/replay) code is used: only the engine (lob) and the orders.log codec
// (orderlog). See viz/BUNDLE.md for the on-disk bundle format this module produces.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lob/types.hpp"

namespace viz {

using TimeNs = std::int64_t;

// ---------------------------------------------------------------------------------------
// Numeric helpers (unit tested directly)
// ---------------------------------------------------------------------------------------

// Sentinels for missing values in data.bin (see BUNDLE.md).
inline constexpr std::int32_t kMissingI32 = std::numeric_limits<std::int32_t>::min();

// uint8 log encoding of a non-negative size against a maximum v_max:
//   v == 0        -> 0
//   0 < v         -> max(1, round(255 * ln(1+v) / ln(1+v_max)))
// so 0 maps to 0, any positive value maps to at least 1, and v_max maps to 255.
std::uint8_t encode_log_u8(double v, double v_max) noexcept;

// Time-weighted mean of a piecewise-constant resting size over an interval of length
// total_ns. `segments` is a list of (size, duration_ns) pairs covering the interval; any
// uncovered time counts as size 0. Returns 0 when total_ns <= 0.
//
// Worked example (unit tested): 100 resting for 0..100ms then 300 for 100..250ms over a
// 250ms column => (100*100 + 300*150) / 250 = 220.
double time_weighted_mean(const std::vector<std::pair<double, TimeNs>>& segments, TimeNs total_ns) noexcept;

// Converts an int64 tick value to the Int32 stored in data.bin, throwing std::overflow_error
// if it does not fit (a hard error, never a silent wrap).
std::int32_t to_i32_checked(std::int64_t v, const char* what);

// Asserts a nanosecond timestamp is exactly representable as Float64 (< 2^53); throws
// std::overflow_error otherwise.
double time_to_f64_checked(TimeNs t);

// ---------------------------------------------------------------------------------------
// Export driver
// ---------------------------------------------------------------------------------------

struct ExportOptions {
  std::string run_dir;
  std::string out_dir;
  std::size_t levels = 10;
  TimeNs ladder_dt_ns = 100'000'000;    // 100 ms
  TimeNs heatmap_dt_ns = 250'000'000;   // 250 ms
  std::int64_t heatmap_pad_ticks = 30;
  std::int64_t budget_bytes = 8 * 1024 * 1024;
};

// One top-of-ladder snapshot for testing: the top `levels` (price, size) per side at a time.
struct LadderSnapshot {
  TimeNs time{0};
  std::vector<std::pair<lob::Price, lob::Qty>> bids;  // best first
  std::vector<std::pair<lob::Price, lob::Qty>> asks;  // best first
};

// Replays `orders_log_text` (a full orders.log including its header) and returns the ladder of
// the top `levels` per side at each requested time, where the state at time t is the state after
// the last request with timestamp <= t. Used by the unit tests to check the ladder equals
// OrderBook::depth(). Throws on a malformed log.
std::vector<LadderSnapshot> sample_ladder(const std::string& orders_log_text, const std::vector<TimeNs>& times,
                                          std::size_t levels);

// Runs the whole export: rebuild the book from <run>/orders.log, verify the engine hash and
// MM PnL against <run>/summary.json, build every series + the heatmap, fit the budget, and
// write <out>/meta.json and <out>/data.bin. Returns the process exit code: 0 on success, 2 on
// any verification failure or bad input (no bundle written in that case). Diagnostics go to
// stderr; a one-line success note goes to stdout.
int run_export(const ExportOptions& opts);

}  // namespace viz
