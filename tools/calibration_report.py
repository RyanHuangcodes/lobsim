#!/usr/bin/env python3
"""Helpers of scripts/run_real_data.sh --calibrate (upgrade 3, C6). Python 3, standard library only.

    calibration_report.py windows --messages FILE [--train A-B] [--test C-D]
        prints "A-B C-D <note>": the windows to validate on. Given windows are used as they are. Without
        them the default is train 36000-45900, test 45900-55800 (10:00-12:45 and 12:45-15:30) when the data
        covers them, otherwise the data's span split in half, and the note says so.
    calibration_report.py pending --report FILE --data-dir DIR
        writes the "Calibration pending data" block between the CALIBRATION markers.
    calibration_report.py report --report FILE --manifest FILE --data-dir DIR --seeds N
        writes the results between the CALIBRATION markers. The manifest has one tab-separated line per run:
        kind (same|cross), stem, output directory, train, test, window note, exit code, calibration ticker.
    calibration_report.py table --validation FILE [--title TEXT]
        prints one validation's scorecard table as Markdown (what the report embeds).

The markers are <!-- CALIBRATION:BEGIN --> and <!-- CALIBRATION:END -->; they are added at the end of the
report if missing. Nothing outside them is touched.
"""
import argparse
import json
import math
import os
import re
import sys

BEGIN, END = "<!-- CALIBRATION:BEGIN -->", "<!-- CALIBRATION:END -->"
DEFAULT_TRAIN, DEFAULT_TEST = (36000, 45900), (45900, 55800)


def fail(message):
    print(f"calibration_report.py: {message}", file=sys.stderr)
    sys.exit(1)


# ---- windows ---------------------------------------------------------------------------------------

def message_span(path):
    """Seconds of the first and the last message line of a LOBSTER message file."""
    with open(path, "rb") as f:
        first = f.readline().split(b",")[0]
        f.seek(0, os.SEEK_END)
        size = f.tell()
        back = min(size, 4096)
        f.seek(size - back)
        lines = [ln for ln in f.read().splitlines() if ln.strip()]
    if not first or not lines:
        fail(f"{path} has no message lines")
    return float(first), float(lines[-1].split(b",")[0])


def parse_window(text, what):
    m = re.fullmatch(r"(\d+)-(\d+)", text or "")
    if not m or int(m.group(1)) >= int(m.group(2)):
        fail(f"{what} must be A-B in whole seconds, A < B (got '{text}')")
    return int(m.group(1)), int(m.group(2))


def windows(messages, train, test):
    if train or test:
        if not (train and test):
            fail("--train and --test must be given together")
        a, b = parse_window(train, "--train")
        c, d = parse_window(test, "--test")
        return (a, b), (c, d), "windows given on the command line"
    first, last = message_span(messages)
    (a, b), (c, d) = DEFAULT_TRAIN, DEFAULT_TEST
    if first <= a and last >= d - 1:
        return (a, b), (c, d), "default windows (train 10:00-12:45, test 12:45-15:30)"
    lo, hi = int(math.floor(first)), int(math.floor(last)) + 1
    mid = lo + (hi - lo) // 2
    return (lo, mid), (mid, hi), (
        "the data does not cover the default windows (%d-%d and %d-%d s; its messages run from %.2f to %.2f s), "
        "so its span was split in half" % (a, b, c, d, first, last))


# ---- tables ----------------------------------------------------------------------------------------

def fmt(x):
    if x is None or (isinstance(x, float) and not math.isfinite(x)):
        return "n/a"
    x = float(x)
    return "%.0f" % x if abs(x) >= 1e4 else "%.4g" % x


def interval(value, lo, hi):
    if lo is None or hi is None:
        return fmt(value) + " [n/a]"
    return "%s [%s, %s]" % (fmt(value), fmt(lo), fmt(hi))


def verdict(v):
    if v["kind"] == "miss":
        r = v["ratio"]
        return "miss (sim/data = %s)" % fmt(r) if r is not None else "miss (data value 0)"
    return v["kind"]


def scorecard_table(v):
    lines = ["| metric | data (test) [95% interval] | calibrated sim [95% CI of mean] | baseline sim [95% CI of mean]; its verdict | calibrated verdict |",
             "|---|---|---|---|---|"]
    for r in v["scorecard"]:
        d, c, b = r["data"], r["calibrated"], r["baseline"]
        lines.append("| `%s` | %s | %s | %s; %s | %s |" % (
            r["name"], interval(d["value"], d["lo"], d["hi"]), interval(c["mean"], c["ci_lo"], c["ci_hi"]),
            interval(b["mean"], b["ci_lo"], b["ci_hi"]), verdict(r["baseline_verdict"]), verdict(r["verdict"])))
    return "\n".join(lines)


def counts_line(v):
    c = v["verdict_counts"]
    def one(k):
        x = c[k]
        return "%d match, %d close, %d miss, %d undefined" % (x["match"], x["close"], x["miss"], x["n/a"])
    return "Calibrated: %s. Baseline: %s." % (one("calibrated"), one("baseline"))


def tag(v):
    return {"SYNTHETIC": "SYNTHETIC data, NOT real market data", "REAL": "real data"}.get(v["label"], "hand-made calibration")


# ---- the report block -----------------------------------------------------------------------------------

def shown(path, root):
    rel = os.path.relpath(os.path.abspath(path), root)
    return (path if rel.startswith("..") else rel)


def write_block(report, body):
    block = BEGIN + "\n" + body + "\n" + END
    text = open(report).read() if os.path.exists(report) else ""
    pattern = re.compile(re.escape(BEGIN) + r".*?" + re.escape(END), re.S)
    if pattern.search(text):
        text = pattern.sub(lambda _: block, text, count=1)
    else:
        text = text.rstrip("\n") + ("\n\n" if text else "") + block + "\n"
    with open(report, "w") as f:
        f.write(text)


def pending_body(data_dir, root):
    return "\n".join([
        "### Calibration pending data",
        "",
        "No LOBSTER files were found, so **no calibration was run and no real-data calibration result exists**. Nothing is reported here on purpose.",
        "",
        "- Expected file names, one pair per ticker and day (L is the number of levels):",
        "  `<TICKER>_<DATE>_34200000_57600000_message_<L>.csv` and",
        "  `<TICKER>_<DATE>_34200000_57600000_orderbook_<L>.csv`, for example",
        "  `INTC_2012-06-21_34200000_57600000_message_10.csv` and",
        "  `INTC_2012-06-21_34200000_57600000_orderbook_10.csv`.",
        "- Put them in the folder `data/lobster/` (it is not tracked by git).",
        "- Run one command. For every ticker it estimates the order flow on 10:00-12:45, simulates it (and the old noise traders as a baseline) "
        "for 20 seeds, and compares both with 12:45-15:30 of the data, which the estimator never reads; with two or more tickers it also calibrates "
        "on the first and validates on each of the others. It replaces this block with the scorecards:",
        "",
        "```",
        "scripts/run_real_data.sh --calibrate",
        "```",
        "",
        "The synthetic dress rehearsal of the same code path (not real data) is in `CALIBRATION.md`.",
    ])


def results_body(manifest, data_dir, seeds, root):
    runs = []
    with open(manifest) as f:
        for line in f:
            line = line.rstrip("\n")
            if line:
                runs.append(line.split("\t"))
    same = [r for r in runs if r[0] == "same"]
    cross = [r for r in runs if r[0] == "cross"]
    out = ["### Calibration results", "",
           "Written by `scripts/run_real_data.sh --calibrate` from the files in `%s/`. For each ticker the estimator reads only the training period; "
           "the calibrated flow and the old noise traders (baseline) are each simulated for %s seeds and compared with the test period of the data "
           "(`lobsim_calibrate validate`, definitions and verdict rule in the `validation.md` of each run). Informed trader and market maker are off in both simulations." % (shown(data_dir, root).rstrip("/"), seeds), ""]

    def section(r, level):
        kind, stem, outdir, train, test, note, rc, cal_ticker = (r + [""] * 8)[:8]
        path = os.path.join(outdir, "validation.json")
        head = "#" * level
        if rc != "0" or not os.path.exists(path):
            return ["%s %s" % (head, stem), "", "The validation did not finish (exit code %s); see the script's output. Nothing is reported for it." % rc, ""]
        v = json.load(open(path))
        t = tag(v)
        lines = ["%s %s (%s)" % (head, stem if kind == "same" else stem, t), ""]
        if v["label"] == "SYNTHETIC":
            lines += ["> **SYNTHETIC.** Every number in this table is a statement about a synthetic market, not a real one.", ""]
        lines += ["- Training period %s s, test period %s s. %s" % (train, test, note[0].upper() + note[1:] + "." if note else ""),
                  "- Seeds: %d per model; start price %d ticks; L = %d; tick = %d file price unit(s)." % (
                      v["simulation"]["seeds"], v["simulation"]["start_price_ticks"], v["levels"], v["tick_units"]),
                  "- Leakage check: %s" % ("passed (the estimator read only the training period)." if v["leakage"]["checked"] else "not applicable (calibration file given)."),
                  "- %s" % counts_line(v),
                  "- Full report and plots: `%s/validation.md`, `%s/plots/`." % (shown(outdir, root), shown(outdir, root)), "",
                  "**%s: scorecard (%s)**" % (stem, t), "", scorecard_table(v), ""]
        return lines

    for r in same:
        out += section(r, 4)
    if cross:
        out += ["#### Cross-ticker check", "",
                "The calibration of one ticker, estimated on its own training period, is used to simulate another ticker, whose test period is the "
                "yardstick. The simulation starts at the validated ticker's own median mid-price (log-return volatility depends on how big a tick is "
                "relative to the price). It tests how general the measured flow is, not only whether it fits one stock.", ""]
        for r in cross:
            out += section(r, 5)
    return "\n".join(out).rstrip()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("windows")
    w.add_argument("--messages", required=True)
    w.add_argument("--train")
    w.add_argument("--test")
    pe = sub.add_parser("pending")
    pe.add_argument("--report", required=True)
    pe.add_argument("--data-dir", required=True)
    pe.add_argument("--root", default=os.getcwd())
    r = sub.add_parser("report")
    r.add_argument("--report", required=True)
    r.add_argument("--manifest", required=True)
    r.add_argument("--data-dir", required=True)
    r.add_argument("--seeds", default="20")
    r.add_argument("--root", default=os.getcwd())
    t = sub.add_parser("table")
    t.add_argument("--validation", required=True)
    t.add_argument("--title")
    a = p.parse_args()
    if a.cmd == "windows":
        (x, y), (c, d), note = windows(a.messages, a.train, a.test)
        print("%d-%d %d-%d %s" % (x, y, c, d, note))
    elif a.cmd == "pending":
        write_block(a.report, pending_body(a.data_dir, a.root))
    elif a.cmd == "report":
        write_block(a.report, results_body(a.manifest, a.data_dir, a.seeds, a.root))
    else:
        v = json.load(open(a.validation))
        if a.title:
            print("**%s**\n" % a.title)
        print(counts_line(v) + "\n")
        print(scorecard_table(v))


if __name__ == "__main__":
    main()
