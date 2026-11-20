#!/usr/bin/env bash
# build_site.sh — build the static GitHub Pages site for the lobsim replay dashboard.
#
# Local equivalent of the Pages build job in .github/workflows/pages.yml: the workflow calls this
# script so the two never drift. It
#   1. configures + builds Release (lobsim_run + lobsim_viz_export),
#   2. runs the default config with fixed seed 42 for the configured duration,
#   3. exports a viz bundle into site/runs/demo/{meta.json,data.bin},
#   4. copies web/ (index.html, css/, js/, vendor/) into site/, and
#   5. rewrites site/index.html so the dashboard loads run=demo when no ?run is given.
#
# The default run matches the committed demo: seed 42, the default config's duration_s (1 hour).
# For a quick local proof you may shorten the run with DURATION_S, e.g.
#   DURATION_S=30 scripts/build_site.sh
# The committed workflow leaves DURATION_S unset so CI always publishes the full 1-hour run.
#
# Everything this produces lives under site/ (and the temp run dir), both gitignored; nothing here
# is ever committed.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
cd "${root}"

build="${BUILD_DIR:-${root}/build}"
site="${root}/site"
config="${root}/configs/default.json"
seed="${SEED:-42}"
run_name="demo"

# Pick a CMake generator: prefer Ninja when present (matches CI and the project default), otherwise
# fall back to the platform default generator so a clean runner without ninja still works.
generator_args=()
if command -v ninja >/dev/null 2>&1; then
  generator_args=(-G Ninja)
  echo "build_site: using Ninja generator"
else
  echo "build_site: ninja not found, using the default CMake generator"
fi

echo "build_site: configuring + building Release in ${build}"
cmake -S "${root}" -B "${build}" "${generator_args[@]}" \
  -DCMAKE_BUILD_TYPE=Release -DLOBSIM_WERROR=ON
cmake --build "${build}" --config Release --target lobsim_run lobsim_viz_export

run_bin="${build}/lobsim_run"
export_bin="${build}/viz/lobsim_viz_export"
[ -x "${run_bin}" ] || run_bin="${build}/bin/lobsim_run"
[ -x "${export_bin}" ] || export_bin="${build}/bin/lobsim_viz_export"
if [ ! -x "${run_bin}" ] || [ ! -x "${export_bin}" ]; then
  echo "build_site: ERROR could not find lobsim_run / lobsim_viz_export under ${build}" >&2
  exit 1
fi

# Run the default sim. With DURATION_S unset we let lobsim_run use the config's duration_s (1 hour);
# otherwise we pass the override through for a faster local build.
run_args=(--config "${config}" --seed "${seed}")
if [ -n "${DURATION_S:-}" ]; then
  echo "build_site: DURATION_S override = ${DURATION_S}s (not the full 1-hour default)"
  run_args+=(--duration-s "${DURATION_S}")
else
  echo "build_site: running the full default duration (seed ${seed})"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

echo "build_site: running the simulator"
"${run_bin}" "${run_args[@]}" --out "${tmp}/run"

echo "build_site: exporting the viz bundle into site/runs/${run_name}"
rm -rf "${site}"
mkdir -p "${site}/runs/${run_name}"
"${export_bin}" --run "${tmp}/run" --out "${site}/runs/${run_name}"

echo "build_site: copying web/ into site/"
# Copy every web asset (index.html, css/, js/, vendor/uplot/, README.md) but not the test fixtures.
cp -R "${root}/web/." "${site}/"
rm -rf "${site}/test"

# Default to run=demo: a tiny inline script sets ?run=demo before app.js reads location.search, so
# the published root URL loads the demo run while explicit ?run=<name> links still override it. We
# edit only the generated site copy; the committed web/index.html is untouched.
python3 - "${site}/index.html" <<'PY'
import sys
path = sys.argv[1]
with open(path, encoding="utf-8") as f:
    html = f.read()
needle = '  <script type="module" src="js/app.js"></script>'
snippet = (
    '  <script>\n'
    '    // GitHub Pages default: load the committed demo run when no ?run is supplied.\n'
    '    (function () {\n'
    '      var p = new URLSearchParams(location.search);\n'
    '      if (!p.get("run")) {\n'
    '        p.set("run", "demo");\n'
    '        history.replaceState(null, "", location.pathname + "?" + p.toString() + location.hash);\n'
    '      }\n'
    '    })();\n'
    '  </script>\n'
    '  <script type="module" src="js/app.js"></script>'
)
if needle not in html:
    sys.stderr.write("build_site: ERROR could not find the app.js module tag to inject the default run\n")
    sys.exit(1)
html = html.replace(needle, snippet, 1)
with open(path, "w", encoding="utf-8") as f:
    f.write(html)
PY

# Sanity checks: the assets the dashboard needs must all be present.
missing=0
for p in \
  "index.html" \
  "css/style.css" \
  "js/app.js" \
  "vendor/uplot/uPlot.iife.min.js" \
  "runs/${run_name}/meta.json" \
  "runs/${run_name}/data.bin"; do
  if [ ! -e "${site}/${p}" ]; then
    echo "build_site: ERROR missing site/${p}" >&2
    missing=1
  fi
done
if ! grep -q 'run.*demo' "${site}/index.html"; then
  echo "build_site: ERROR site/index.html does not default to run=demo" >&2
  missing=1
fi
[ "${missing}" -eq 0 ] || exit 1

echo "build_site: site/ contents:"
find "${site}" -maxdepth 3 | sort
echo "build_site: OK — site/ ready to serve (defaults to run=${run_name})"
