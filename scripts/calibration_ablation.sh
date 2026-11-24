#!/usr/bin/env bash
# Which part of a synthetic day does the calibrated background flow fail to reproduce? Takes
# each synthetic day of scripts/make_fake_day.sh apart: the full day, without the market maker,
# without the informed trader, and with the noise traders alone. Each variant is simulated,
# exported in LOBSTER format, calibrated on its first half and validated on its second half
# (`lobsim_calibrate validate`), and the rows that miss on the full day are printed side by side.
# All data here is SYNTHETIC (lobsim's own output), not market data.
#   scripts/calibration_ablation.sh [--bin-dir DIR] [--out DIR] [--seeds N]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
bin_dir="$root/build/release"
out="$root/build/calibration_ablation"
seeds=20
while [ $# -gt 0 ]; do
  case "$1" in
    --bin-dir) bin_dir="${2:?--bin-dir needs a value}"; shift 2 ;;
    --out) out="${2:?--out needs a value}"; shift 2 ;;
    --seeds) seeds="${2:?--seeds needs a value}"; shift 2 ;;
    *) echo "usage: calibration_ablation.sh [--bin-dir DIR] [--out DIR] [--seeds N]" >&2; exit 1 ;;
  esac
done
for b in lobsim_run lobsim_export_lobster lobsim_calibrate; do
  if [ ! -x "$bin_dir/$b" ]; then echo "calibration_ablation.sh: $bin_dir/$b not found; build first or pass --bin-dir" >&2; exit 1; fi
done

# The same warm-up, window and seeds as scripts/make_fake_day.sh, so "full" is that day exactly.
warmup=300
window=3600
mkdir -p "$out"
run_variant() {  # day config sim-seed hidden-seed variant removed-sections...
  local day="$1" config="$2" seed="$3" hidden_seed="$4" variant="$5"; shift 5
  local dir="$out/$day/$variant"
  rm -rf "$dir"; mkdir -p "$dir"
  python3 - "$config" "$dir/config.json" "$@" <<'PY'
import json, sys
config = json.load(open(sys.argv[1]))
for section in sys.argv[3:]:
    config.pop(section)
json.dump(config, open(sys.argv[2], "w"), indent=2)
PY
  "$bin_dir/lobsim_run" --config "$dir/config.json" --seed "$seed" --duration-s $((warmup + window)) --out "$dir/run" >/dev/null
  "$bin_dir/lobsim_export_lobster" --run "$dir/run" --levels 10 --lobster-like --window-start-s $warmup \
    --seed "$hidden_seed" --ticker "$day" --out "$dir/data" >/dev/null
  rm -rf "$dir/run"
  local messages
  messages="$(ls "$dir"/data/*_message_10.csv)"
  "$bin_dir/lobsim_calibrate" validate --messages "$messages" --orderbook "${messages/_message_/_orderbook_}" \
    --train 34200-36000 --test 36000-37800 --seeds "$seeds" --out "$dir/validation" --no-plots >/dev/null
}

for day in SYNTHA SYNTHB; do
  if [ "$day" = SYNTHA ]; then config="$root/configs/default.json"; seed=20120621; hidden=101
  else config="$root/configs/synthb.json"; seed=20120622; hidden=202; fi
  run_variant "$day" "$config" "$seed" "$hidden" full
  run_variant "$day" "$config" "$seed" "$hidden" no_market_maker mm
  run_variant "$day" "$config" "$seed" "$hidden" no_informed informed
  run_variant "$day" "$config" "$seed" "$hidden" noise_only mm informed
done

python3 - "$out" "$seeds" <<'PY'
import json, sys
from pathlib import Path
out, seeds = Path(sys.argv[1]), sys.argv[2]
rows = ["spread_mean", "spread_one_tick_share", "fill_le0", "fill_1_2", "executed_share", "vol_1s", "vol_60s", "sign_acf_lag1"]
variants = ["full", "no_market_maker", "no_informed", "noise_only"]
for day in ["SYNTHA", "SYNTHB"]:
    print(f"### {day} (SYNTHETIC data, NOT real market data; {seeds} seeds)\n")
    print("Each cell: data (test half) / calibrated simulation, verdict.\n")
    print("| metric | " + " | ".join(variants) + " |\n|---|" + "---|" * len(variants))
    cards = {}
    for v in variants:
        path = next((out / day / v / "validation").rglob("validation.json"))
        cards[v] = {r["name"]: r for r in json.load(open(path))["scorecard"]}
    for m in rows:
        cells = []
        for v in variants:
            r = cards[v][m]
            cells.append(f"{r['data']['value']:.4g} / {r['calibrated']['mean']:.4g}, {r['verdict']['kind']}")
        print(f"| `{m}` | " + " | ".join(cells) + " |")
    print()
PY
