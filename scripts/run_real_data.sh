#!/usr/bin/env bash
# Run the LOBSTER replay on every message/orderbook pair in a data folder and write the results into
# a report file, between <!-- REAL-DATA:BEGIN --> and <!-- REAL-DATA:END -->.
#
#   scripts/run_real_data.sh [--data-dir DIR] [--report FILE] [--out DIR] [--bin PATH]
#
#   --data-dir  folder holding the LOBSTER files            (default data/lobster/)
#   --report    file whose marked block is replaced         (default REAL_DATA.md; markers added if missing)
#   --out       where each run's summary.json is kept       (default build/real_data/)
#   --bin       the lobsim_lobster binary                   (default: first of build/release, build/debug)
#
# Per pair it runs strict, report and resync mode (default compare and trust depths) and keeps
# <out>/<pair>/<mode>/summary.json and divergences.csv. Exit codes: 0 all runs done (a strict-mode
# failure is a result, not an error), 1 bad arguments or a run that failed, 3 no data in the folder.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
data_dir="$root/data/lobster"
report="$root/REAL_DATA.md"
out="$root/build/real_data"
bin=""

die() { echo "run_real_data.sh: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || die "$1 needs a value"
  case "$1" in
    --data-dir) data_dir="$2" ;;
    --report) report="$2" ;;
    --out) out="$2" ;;
    --bin) bin="$2" ;;
    *) die "unknown argument $1 (usage: run_real_data.sh [--data-dir DIR] [--report FILE] [--out DIR] [--bin PATH])" ;;
  esac
  shift 2
done

if [ -z "$bin" ]; then
  for candidate in "$root/build/release/lobsim_lobster" "$root/build/debug/lobsim_lobster"; do
    if [ -x "$candidate" ]; then bin="$candidate"; break; fi
  done
fi
[ -n "$bin" ] && [ -x "$bin" ] || die "no lobsim_lobster binary: build one (cmake --preset release && cmake --build --preset release) or pass --bin PATH"
command -v python3 >/dev/null || die "python3 is needed to write the report"

# Every <stem>_message_<L>.csv with a <stem>_orderbook_<L>.csv next to it.
pairs=()
if [ -d "$data_dir" ]; then
  while IFS= read -r m; do
    ob="${m/_message_/_orderbook_}"
    [ "$ob" != "$m" ] && [ -f "$ob" ] && pairs+=("$m")
  done < <(find "$data_dir" -maxdepth 1 -type f -name '*_message_*.csv' | sort)
fi

render() { RUN_REAL_DATA_ROOT="$root" RUN_REAL_DATA_DIR="$data_dir" python3 - "$report" "$@" <<'PY'
import json, os, re, sys

report, kind = sys.argv[1], sys.argv[2]
ROOT, DATA_DIR = os.environ["RUN_REAL_DATA_ROOT"], os.environ["RUN_REAL_DATA_DIR"]
BEGIN, END = "<!-- REAL-DATA:BEGIN -->", "<!-- REAL-DATA:END -->"
MODES = ["strict", "report", "resync"]


def missing_block():
    return "\n".join([
        "### Data not provided yet",
        "",
        "No LOBSTER files were found, so **no real-data results exist**. Nothing is reported here on purpose.",
        "",
        "- Expected file names, one pair per ticker and day (L is the number of levels):",
        "  `<TICKER>_<DATE>_34200000_57600000_message_<L>.csv` and",
        "  `<TICKER>_<DATE>_34200000_57600000_orderbook_<L>.csv`, for example",
        "  `INTC_2012-06-21_34200000_57600000_message_10.csv` and",
        "  `INTC_2012-06-21_34200000_57600000_orderbook_10.csv`.",
        "- Put them in the folder `data/lobster/` (it is not tracked by git).",
        "- Run one command, which replays every pair in all three modes and replaces this block with the results:",
        "",
        "```",
        "scripts/run_real_data.sh",
        "```",
        "",
        "LOBSTER data needs a LOBSTER account, a university subscription, an approved trial, or the sample that comes with the book *Trades, Quotes and Prices*.",
    ])


def pct(n, d):
    return "n/a" if d == 0 else "%d.%02d" % divmod(n * 10000 // d, 100)


def shown(path):  # a folder as the report names it: relative to the repository when inside it
    rel = os.path.relpath(os.path.abspath(path), ROOT)
    return (path if rel.startswith("..") else rel).rstrip("/") + "/"


def results_block(outdir, stems):
    lines = ["### Results", "",
             "Written by `scripts/run_real_data.sh` from the files in `" + shown(DATA_DIR) + "`; each run's `summary.json` is under `" + shown(outdir) + "`.",
             "Strict, report and resync use the default compare depth and trust depth. A strict run that stops is a result: it names the first row that is not exact.",
             ""]
    for stem in stems:
        runs = {}
        for mode in MODES:
            path = os.path.join(outdir, stem, mode, "summary.json")
            if os.path.exists(path):
                with open(path) as f:
                    runs[mode] = json.load(f)
        lines += ["#### %s" % stem, ""]
        if not runs:
            lines += ["No summary was produced for this pair (see the script's output).", ""]
            continue
        levels = next(iter(runs.values()))["levels"]
        depths = sorted({d for d in (1, 2, 5, levels) if d <= levels})
        head = "| mode | completed | lines | " + " | ".join("exact rows at depth %d" % d for d in depths) + " | crossed state | messages/s |"
        lines += [head, "|" + "---|" * (5 + len(depths))]
        for mode, s in runs.items():
            ex = {e["depth"]: e for e in s["exact_rows"]}
            done = "yes" if s["completed"] else "stopped at line %s" % s["strict_failure_line"]
            lines.append("| %s | %s | %d | %s | %d | %s |" % (
                mode, done, s["lines"],
                " | ".join("%s%% (%d of %d)" % (ex[d]["percent"], ex[d]["exact"], ex[d]["rows"]) for d in depths),
                s["counters"]["crossed_state"], s.get("messages_per_second", "n/a")))
        lines.append("")
        if "resync" in runs:
            r = runs["resync"]["resync"]
            lines += ["Resync corrections: %d rows corrected, by level %s, removed by rank %s, removed beyond level L %d, failed %d."
                      % (r["rows"], r["corrected_by_level"], r["removed_by_rank"], r["removed_beyond_level_l"], r["failed"]), ""]
        lines.append("Counters (resync run, else the first run):")
        lines.append("")
        c = (runs.get("resync") or next(iter(runs.values())))["counters"]
        lines.append("`" + ", ".join("%s=%d" % kv for kv in c.items()) + "`")
        lines.append("")
        lines += ["Matching agreement (agree / scored, then every class):", "",
                  "| mode | trust depth | agree / scored | % | exact | exact via synthetic | queue order | price skip (better/worse) | size split | length | excluded unknown | excluded untrusted |",
                  "|" + "---|" * 12]
        for mode, s in runs.items():
            for t in s["matching"]["by_trust_depth"]:
                lines.append("| %s | %d | %d / %d | %s | %d | %d | %d | %d (%d/%d) | %d | %d | %d | %d |" % (
                    mode, t["trust_depth"], t["numerator"], t["denominator"], t["percent"], t["exact"],
                    t["exact_via_synthetic"], t["queue_order"], t["price_skip"], t["price_skip_better"],
                    t["price_skip_worse"], t["size_split"], t["length"], t["excluded_unknown"], t["excluded_untrusted"]))
        lines.append("")
        ex = (runs.get("resync") or next(iter(runs.values())))["matching"]["examples"]
        for cls, items in ex.items():
            for x in items[:3]:
                lines += ["- %s at line %d: messages %s; data %s; predicted %s" % (
                    cls, x["line"], x["messages"], x["data"], x["predicted"])]
        lines.append("")
    return "\n".join(lines).rstrip()


if kind == "missing":
    body = missing_block()
else:
    body = results_block(sys.argv[3], sys.argv[4:])

block = BEGIN + "\n" + body + "\n" + END
text = open(report).read() if os.path.exists(report) else ""
pattern = re.compile(re.escape(BEGIN) + r".*?" + re.escape(END), re.S)
if pattern.search(text):
    text = pattern.sub(lambda _: block, text, count=1)
else:
    text = text.rstrip("\n") + ("\n\n" if text else "") + block + "\n"
with open(report, "w") as f:
    f.write(text)
PY
}

if [ "${#pairs[@]}" -eq 0 ]; then
  render missing || die "could not write $report"
  echo "run_real_data.sh: no LOBSTER message/orderbook pairs in $data_dir; wrote 'Data not provided yet' to $report" >&2
  exit 3
fi

status=0
stems=()
for m in "${pairs[@]}"; do
  ob="${m/_message_/_orderbook_}"
  stem="$(basename "$m" .csv)"
  stem="${stem/_message_/_}"
  stems+=("$stem")
  for mode in strict report resync; do
    dir="$out/$stem/$mode"
    echo "== $stem: $mode"
    "$bin" --messages "$m" --orderbook "$ob" --mode "$mode" --out "$dir"
    rc=$?
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 2 ]; then
      echo "run_real_data.sh: lobsim_lobster exited with $rc on $stem ($mode)" >&2
      status=1
    fi
  done
done

render results "$out" "${stems[@]}" || die "could not write $report"
echo "run_real_data.sh: wrote the results of ${#stems[@]} pair(s) to $report"
exit $status
