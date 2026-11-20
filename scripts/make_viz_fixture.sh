#!/usr/bin/env bash
# make_viz_fixture.sh — regenerate the tiny committed viz bundle used by the node --test suite.
#
# This is the single source of truth for the fixture at web/test/fixtures/{meta.json,data.bin}.
# Re-run it whenever viz_core.cpp's kSchemaVersion changes (the C++ end-to-end test fails if the
# committed fixture's schema_version is stale). The bundle must stay under 50 KB so it is cheap to
# commit; the short duration + coarse sampling flags below keep it there.
#
# Usage: scripts/make_viz_fixture.sh [build-dir]
#   build-dir defaults to ./build. The script builds lobsim_run and lobsim_viz_export there if the
#   binaries are missing, runs a short default sim, exports a bundle, and copies meta.json + data.bin
#   into web/test/fixtures/. Idempotent: it overwrites the fixture in place.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
build="${1:-${root}/build}"

config="${root}/configs/default.json"
fixtures="${root}/web/test/fixtures"

# Short sim + coarse sampling keeps the bundle tiny. 20 simulated seconds with 500 ms ladder
# sampling and 1 s heatmap columns, padded narrowly, lands well under 50 KB.
duration_s=20
ladder_dt_ms=500
heatmap_dt_ms=1000
heatmap_pad_ticks=8
seed=42

run_bin="${build}/bin/lobsim_run"
export_bin="${build}/viz/lobsim_viz_export"
# Fall back to the layouts CMake may use if bin/ is not where the tools landed.
[ -x "${run_bin}" ] || run_bin="${build}/lobsim_run"
[ -x "${export_bin}" ] || export_bin="${build}/lobsim_viz_export"

if [ ! -x "${run_bin}" ] || [ ! -x "${export_bin}" ]; then
  echo "make_viz_fixture: building lobsim_run and lobsim_viz_export in ${build}"
  cmake -S "${root}" -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "${build}" --target lobsim_run lobsim_viz_export >/dev/null
  run_bin="${build}/bin/lobsim_run"
  export_bin="${build}/viz/lobsim_viz_export"
  [ -x "${run_bin}" ] || run_bin="${build}/lobsim_run"
  [ -x "${export_bin}" ] || export_bin="${build}/lobsim_viz_export"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

echo "make_viz_fixture: running ${duration_s}s sim (seed ${seed})"
"${run_bin}" --config "${config}" --seed "${seed}" --duration-s "${duration_s}" --out "${tmp}/run" >/dev/null

echo "make_viz_fixture: exporting bundle"
"${export_bin}" --run "${tmp}/run" --out "${tmp}/bundle" \
  --ladder-dt-ms "${ladder_dt_ms}" --heatmap-dt-ms "${heatmap_dt_ms}" \
  --heatmap-pad-ticks "${heatmap_pad_ticks}" >/dev/null

mkdir -p "${fixtures}"
cp "${tmp}/bundle/meta.json" "${fixtures}/meta.json"
cp "${tmp}/bundle/data.bin" "${fixtures}/data.bin"

meta_bytes=$(wc -c < "${fixtures}/meta.json")
data_bytes=$(wc -c < "${fixtures}/data.bin")
total=$((meta_bytes + data_bytes))
echo "make_viz_fixture: wrote web/test/fixtures/ meta.json=${meta_bytes}B data.bin=${data_bytes}B total=${total}B"
if [ "${total}" -ge 51200 ]; then
  echo "make_viz_fixture: ERROR fixture is ${total} bytes, must stay under 50 KB; lower the duration or coarsen sampling" >&2
  exit 1
fi
echo "make_viz_fixture: OK (under 50 KB)"
