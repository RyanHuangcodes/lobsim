// lobsim_viz_export: turn a simulator run folder into a viz bundle (meta.json + data.bin).
//
//   lobsim_viz_export --run <dir> --out <dir>
//       [--levels 10] [--ladder-dt-ms 100] [--heatmap-dt-ms 250]
//       [--heatmap-pad-ticks 30] [--budget-mb 8]
//
// Rebuilds the order book from <run>/orders.log exactly as src/sim/replay.cpp does, verifies the
// recomputed engine event hash against <run>/summary.json and the recomputed MM PnL against
// market_maker.whole_run.pnl_total, then writes the bundle. On any verification failure or bad
// input it prints to stderr and exits 2 without writing a bundle. See viz/BUNDLE.md.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "viz_core.hpp"

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: lobsim_viz_export --run <dir> --out <dir> [--levels N] [--ladder-dt-ms N] "
               "[--heatmap-dt-ms N] [--heatmap-pad-ticks N] [--budget-mb N]\n");
  return 2;
}

bool parse_i64(const char* s, std::int64_t& out) {
  char* end = nullptr;
  const long long v = std::strtoll(s, &end, 10);
  if (end == s || *end != '\0') return false;
  out = v;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  viz::ExportOptions opts;
  bool have_run = false, have_out = false;

  for (int i = 1; i < argc; i += 2) {  // every flag takes one value
    const std::string a = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (value == nullptr) return usage();
    std::int64_t n = 0;
    if (a == "--run") {
      opts.run_dir = value;
      have_run = true;
    } else if (a == "--out") {
      opts.out_dir = value;
      have_out = true;
    } else if (a == "--levels") {
      if (!parse_i64(value, n) || n <= 0) return usage();
      opts.levels = static_cast<std::size_t>(n);
    } else if (a == "--ladder-dt-ms") {
      if (!parse_i64(value, n) || n <= 0) return usage();
      opts.ladder_dt_ns = n * 1'000'000;
    } else if (a == "--heatmap-dt-ms") {
      if (!parse_i64(value, n) || n <= 0) return usage();
      opts.heatmap_dt_ns = n * 1'000'000;
    } else if (a == "--heatmap-pad-ticks") {
      if (!parse_i64(value, n) || n < 0) return usage();
      opts.heatmap_pad_ticks = n;
    } else if (a == "--budget-mb") {
      if (!parse_i64(value, n) || n <= 0) return usage();
      opts.budget_bytes = n * 1024 * 1024;
    } else {
      return usage();
    }
  }
  if (!have_run || !have_out) return usage();

  return viz::run_export(opts);
}
