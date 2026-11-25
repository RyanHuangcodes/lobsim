"""Lab 11.3: stylized facts from the one-second mids in bbo.csv.

Recomputes the "returns" block of summary.json with the same formulas as src/sim/metrics.cpp:
  log return     r_t = ln(m_t / m_{t-1})        (only when both samples have a mid)
  excess kurtosis    m4 / m2^2 - 3               (0 for a bell curve; > 0 means fat tails)
  autocorrelation    sum (x_t - m)(x_{t+k} - m) / sum (x_t - m)^2

Run:  python3 course/labs/py/returns_stats.py out/run1
"""
import csv
import json
import math
import sys


def autocorrelation(x, lag):
    present = [v for v in x if v is not None]
    m = sum(present) / len(present)
    den = sum((v - m) ** 2 for v in present)
    num = sum((x[t] - m) * (x[t + lag] - m) for t in range(len(x) - lag)
              if x[t] is not None and x[t + lag] is not None)
    return num / den


def excess_kurtosis(x):
    n = len(x)
    m = sum(x) / n
    m2 = sum((v - m) ** 2 for v in x) / n
    m4 = sum((v - m) ** 4 for v in x) / n
    return m4 / (m2 * m2) - 3.0


def main(run_dir):
    summary = json.load(open(f"{run_dir}/summary.json"))
    warmup_ns = summary["window"]["warmup_ns"]
    end_ns = summary["window"]["end_ns"]
    mids = []
    for row in csv.DictReader(open(f"{run_dir}/bbo.csv")):
        t = int(row["time_ns"])
        if warmup_ns <= t <= end_ns:
            mids.append(float(row["mid"]) if row["mid"] else None)

    rets = []
    for a, b in zip(mids, mids[1:]):
        rets.append(math.log(b / a) if a is not None and b is not None else None)
    present = [r for r in rets if r is not None]
    zeros = sum(1 for r in present if r == 0.0)
    ref = summary["returns"]

    print(f"samples {len(mids)}, returns {len(present)}   (summary: {ref['samples']}, {ref['returns']})")
    print(f"share of returns exactly 0: {zeros / len(present):.4f}   (summary: {ref['zero_fraction']})")
    print(f"excess kurtosis:            {excess_kurtosis(present):.4f}   (summary: {ref['excess_kurtosis']})")
    acf = [autocorrelation(rets, k) for k in range(1, 6)]
    print("return ACF, lags 1-5:      ", " ".join(f"{a:+.3f}" for a in acf))
    print("   summary:                ", " ".join(f"{a:+.3f}" for a in ref["acf_lags_1_to_5"]))
    abs_rets = [abs(r) if r is not None else None for r in rets]
    abs_acf = [autocorrelation(abs_rets, k) for k in range(1, 11)]
    print("|return| ACF, lags 1-10:   ", " ".join(f"{a:+.3f}" for a in abs_acf))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out/run1")
