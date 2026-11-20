#!/usr/bin/env bash
# Regenerate the two synthetic LOBSTER-like days (SYNTHA, SYNTHB) into data/synthetic/.
#   scripts/make_fake_day.sh [--bin-dir DIR] [--out DIR]
# SYNTHETIC DATA, NOT MARKET DATA. Deterministic: same binaries and seeds, same bytes. Each day is a
# 300 s warm-up plus 3600 s of window, L = 10. Never writes into data/lobster/ (the exporter refuses).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin_dir=""
out="$root/data/synthetic"
while [ $# -gt 0 ]; do
  case "$1" in
    --bin-dir) bin_dir="${2:?--bin-dir needs a value}"; shift 2 ;;
    --out) out="${2:?--out needs a value}"; shift 2 ;;
    *) echo "usage: make_fake_day.sh [--bin-dir DIR] [--out DIR]" >&2; exit 1 ;;
  esac
done
if [ -z "$bin_dir" ]; then
  for d in "$root/build/release" "$root/build/debug"; do
    if [ -x "$d/lobsim_run" ] && [ -x "$d/lobsim_export_lobster" ]; then bin_dir="$d"; break; fi
  done
fi
if [ -z "$bin_dir" ] || [ ! -x "$bin_dir/lobsim_run" ] || [ ! -x "$bin_dir/lobsim_export_lobster" ]; then
  echo "make_fake_day.sh: lobsim_run / lobsim_export_lobster not found; build first or pass --bin-dir" >&2
  exit 1
fi

warmup=300
window=3600
make_day() {  # ticker config sim-seed hidden-seed
  local ticker="$1" config="$2" seed="$3" hidden_seed="$4"
  local work="$out/.work_$ticker"
  rm -rf "$work"
  "$bin_dir/lobsim_run" --config "$config" --seed "$seed" --duration-s $((warmup + window)) --out "$work" >/dev/null
  "$bin_dir/lobsim_export_lobster" --run "$work" --levels 10 --lobster-like --window-start-s $warmup \
    --seed "$hidden_seed" --ticker "$ticker" --out "$out/$ticker"
  rm -rf "$work"
}

mkdir -p "$out"
make_day SYNTHA "$root/configs/default.json" 20120621 101
make_day SYNTHB "$root/configs/synthb.json" 20120622 202
echo "wrote synthetic days under $out (SYNTHETIC, not market data)"
