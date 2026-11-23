#!/usr/bin/env bash
# Parameter recovery (spec upgrade 3, C3): simulate the hand-made truth with configs/calibrated.json,
# export the run as LOBSTER-like days, estimate each over its whole span and compare with the
# truth. SYNTHETIC: every number this writes describes a simulation of a hand-made calibration.
#
#   scripts/run_recovery.sh [--bin-dir build/release] [--out build/recovery] [--window-s 7200]
#                           [--seed 20240101] [--tolerance-scale 1] [--config configs/calibrated.json]
#
# Variants, all from one simulator run:
#   unfiltered      --no-filter-window at L = 40: the published levels hold (almost) the whole book,
#                   so nothing is hidden. Judged.
#   filtered        the price-window filter on, at L = 10, as LOBSTER files are: the biases are
#                   measured (compare --filtered). Judged by the filtered rules.
#   unfiltered_L10  --no-filter-window at L = 10: no message is hidden, but an order more than ten
#                   levels deep counts as having left the window, so the level limit is the only
#                   difference from "unfiltered". Reported, not part of the exit code: its deepest
#                   lifetime bucket cannot be measured (see docs/parameter_recovery.md).
# All keep the lobster-like window start (300 s of warm-up), hidden executions and the halt; the
# estimator ignores types 5 and 7. Writes <out>/<variant>/compare.md and prints the tables.
# Exit codes: 0 both judged comparisons passed, 2 one failed, 1 anything else went wrong.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=build/release OUT=build/recovery WINDOW=7200 SEED=20240101 SCALE=1 CONFIG=configs/calibrated.json
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || { echo "run_recovery.sh: $1 needs a value" >&2; exit 1; }
  case "$1" in
    --bin-dir) BIN=$2 ;;
    --out) OUT=$2 ;;
    --window-s) WINDOW=$2 ;;
    --seed) SEED=$2 ;;
    --tolerance-scale) SCALE=$2 ;;
    --config) CONFIG=$2 ;;
    *) echo "run_recovery.sh: unknown argument $1" >&2; exit 1 ;;
  esac
  shift 2
done
WARMUP=300
mkdir -p "$OUT"
rm -rf "$OUT/run" "$OUT/unfiltered" "$OUT/filtered" "$OUT/unfiltered_L10"
start=$(date +%s)
"$BIN/lobsim_run" --config "$CONFIG" --seed "$SEED" --duration-s $((WARMUP + WINDOW)) --out "$OUT/run" > "$OUT/run.txt"
status=0
variant() {  # name, levels, judged (1) or reported only (0), extra exporter arguments...
  local name=$1 levels=$2 judged=$3 rc=0
  shift 3
  local d=$OUT/$name
  local stem=$d/files/RECOV_2012-06-21_34200000_57600000
  "$BIN/lobsim_export_lobster" --run "$OUT/run" --levels "$levels" --lobster-like --window-start-s $WARMUP "$@" \
    --ticker RECOV --out "$d/files" > "$OUT/$name.export.txt"
  "$BIN/lobsim_calibrate" estimate --messages "${stem}_message_${levels}.csv" \
    --orderbook "${stem}_orderbook_${levels}.csv" --out "$d/est" > "$OUT/$name.estimate.txt" 2>&1
  local est
  est=$(ls "$d"/est/*.json)
  local cmp=(--truth calibration/recovery_truth.json --estimate "$est" --tolerance-scale "$SCALE" --md "$d/compare.md")
  if [ "$name" = filtered ]; then cmp+=(--filtered --depth-p95-from "${stem}_orderbook_${levels}.csv"); fi
  echo "== $name (L = $levels, SYNTHETIC)"
  "$BIN/lobsim_calibrate" compare "${cmp[@]}" || rc=$?
  if [ $rc -ne 0 ] && [ $rc -ne 2 ]; then exit 1; fi
  if [ $rc -eq 2 ] && [ "$judged" = 1 ]; then status=2; fi
}
variant unfiltered 40 1 --no-filter-window
variant filtered 10 1
variant unfiltered_L10 10 0 --no-filter-window
echo "run_recovery.sh: seed $SEED, window $WINDOW s, $(( $(date +%s) - start )) s; tables in $OUT/*/compare.md"
exit $status
