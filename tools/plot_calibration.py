#!/usr/bin/env python3
"""Plots for one out-of-sample validation (lobsim_calibrate validate).

    python tools/plot_calibration.py --validation <dir>/validation.json [--out <dir>/plots]

Reads validation.json and writes four PNGs (matplotlib, Agg backend), each showing the test-period
data, the calibrated simulation and the baseline simulation (the old hand-picked noise traders):
spread_pmf, depth_profile, survival, volatility_vs_interval.

Every figure says in its title whether the numbers are real or synthetic (the "label" of the file).
The data survival curve is the training period's Kaplan-Meier curve (what was fitted): the
estimator never reads the test period.
"""
import argparse
import json
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

# Okabe-Ito colours: distinguishable with the common colour-vision deficiencies.
COLOURS = {"data": "#222222", "calibrated": "#0072B2", "baseline": "#D55E00"}
NAMES = {"data": "data (test period)", "calibrated": "calibrated sim", "baseline": "baseline sim (old noise traders)"}


def fail(message):
    print(f"plot_calibration.py: {message}", file=sys.stderr)
    sys.exit(1)


def num(x):
    return float("nan") if x is None else float(x)


def heading(v):
    label = v["label"]
    suffix = "SYNTHETIC data, NOT real market data" if label == "SYNTHETIC" else label.lower()
    return f'{v["ticker"]} {v["date"]} ({suffix})'


def footer(fig, v):
    if v["label"] == "SYNTHETIC":
        fig.text(0.5, 0.005, "SYNTHETIC DATA - NOT REAL MARKET DATA", ha="center", va="bottom", fontsize=8, color="#8b0000")


def rows_by_name(v):
    return {r["name"]: r for r in v["scorecard"]}


def save(fig, v, out, name):
    footer(fig, v)
    fig.savefig(out / name, dpi=130)
    plt.close(fig)


def plot_spread(v, out):
    pm = v["distributions"]["spread_pmf"]
    kinds = ["data", "calibrated", "baseline"]
    xs = sorted({x for k in kinds for x in pm[k]["values"]})
    mass = {k: dict(zip(pm[k]["values"], pm[k]["probs"])) for k in kinds}
    keep = [x for x in xs if any(mass[k].get(x, 0.0) >= 0.001 for k in kinds)] or xs
    if not keep:
        return
    fig, ax = plt.subplots(figsize=(8, 4.5))
    width = 0.27
    for i, k in enumerate(kinds):
        ax.bar(np.arange(len(keep)) + (i - 1) * width, [mass[k].get(x, 0.0) for x in keep], width, color=COLOURS[k], label=NAMES[k])
    ax.set_xticks(np.arange(len(keep)))
    ax.set_xticklabels([str(x) for x in keep])
    ax.set_xlabel("spread (ticks)")
    ax.set_ylabel("share of two-sided time")
    ax.set_title("Spread distribution\n" + heading(v), fontsize=10)
    ax.legend(frameon=False)
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    save(fig, v, out, "spread_pmf.png")


def plot_depth(v, out):
    rows = rows_by_name(v)
    levels = v["levels"]
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.2), sharey=True)
    for ax, side in zip(axes, ["buy", "sell"]):
        names = [f"depth_{side}_L{k}" for k in range(1, levels + 1)]
        x = np.arange(1, levels + 1)
        d = [rows[n]["data"] for n in names]
        ax.fill_between(x, [num(r["lo"]) for r in d], [num(r["hi"]) for r in d], color=COLOURS["data"], alpha=0.18, lw=0)
        ax.plot(x, [num(r["value"]) for r in d], color=COLOURS["data"], marker="o", ms=3, label=NAMES["data"] + ", 95% bootstrap band")
        for k, shift in (("calibrated", -0.06), ("baseline", 0.06)):
            s = [rows[n][k] for n in names]
            mean = np.array([num(r["mean"]) for r in s])
            lo = np.array([num(r["ci_lo"]) for r in s])
            hi = np.array([num(r["ci_hi"]) for r in s])
            ax.errorbar(x + shift, mean, yerr=[np.maximum(mean - lo, 0), np.maximum(hi - mean, 0)], color=COLOURS[k], marker="s", ms=3,
                        lw=1, capsize=2, label=NAMES[k] + ", 95% CI of mean")
        ax.set_title(f"{side} side")
        ax.set_xlabel("level")
        ax.set_xticks(x)
    axes[0].set_ylabel("mean size at the level (shares)")
    axes[0].legend(frameon=False, fontsize=7)
    fig.suptitle("Average depth profile\n" + heading(v), fontsize=10)
    fig.tight_layout(rect=(0, 0.03, 1, 0.93))
    save(fig, v, out, "depth_profile.png")


def curve(c):
    t = [x / 1e9 for x in c["times_ns"]]
    s = list(c["survival"])
    return t, s


def plot_survival(v, out):
    surv = v["survival"]
    buckets = surv["bucket_names"]
    fig, axes = plt.subplots(1, len(buckets), figsize=(3.6 * len(buckets), 4), sharey=True)
    axes = np.atleast_1d(axes)
    sources = [("data", surv["data_train"], NAMES["data"].replace("test period", "training period, as fitted")),
               ("calibrated", surv.get("calibrated"), NAMES["calibrated"] + " (first seed)"),
               ("baseline", surv.get("baseline"), NAMES["baseline"] + " (first seed)")]
    all_t = [x for _, curves, _ in sources if curves for c in curves for x in curve(c)[0]]
    t_min = min(all_t) / 2 if all_t else 0.01
    t_max = max(all_t) * 1.5 if all_t else 100.0
    for b, ax in enumerate(axes):
        for kind, curves, label in sources:
            if not curves or b >= len(curves) or not curves[b]["times_ns"]:
                continue
            t, s = curve(curves[b])
            ax.step([t_min] + t, [1.0] + s, where="post", color=COLOURS[kind], label=label, lw=1.6)
        ax.set_xscale("log")
        ax.set_xlim(t_min, t_max)
        ax.set_ylim(0, 1.02)
        ax.set_title(f"distance {buckets[b]} ticks", fontsize=9)
        ax.set_xlabel("lifetime (s)")
        ax.grid(alpha=0.25)
    axes[0].set_ylabel("survival S(t) (Kaplan-Meier)")
    axes[0].legend(frameon=False, fontsize=7, loc="lower left")
    fig.suptitle("Limit-order lifetime survival\n" + heading(v), fontsize=10)
    fig.tight_layout(rect=(0, 0.03, 1, 0.9))
    save(fig, v, out, "survival.png")


def plot_vol(v, out):
    rows = rows_by_name(v)
    clocks = [1, 10, 60]
    names = [f"vol_{c}s" for c in clocks]
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.2))
    for ax, scaled in zip(axes, [False, True]):
        div = np.sqrt(np.array(clocks, dtype=float)) if scaled else np.ones(len(clocks))
        d = [rows[n]["data"] for n in names]
        x = np.array(clocks, dtype=float)
        ax.fill_between(x, np.array([num(r["lo"]) for r in d]) / div, np.array([num(r["hi"]) for r in d]) / div, color=COLOURS["data"], alpha=0.18, lw=0)
        ax.plot(x, np.array([num(r["value"]) for r in d]) / div, color=COLOURS["data"], marker="o", label=NAMES["data"] + ", 95% bootstrap band")
        for k, shift in (("calibrated", 0.95), ("baseline", 1.05)):
            s = [rows[n][k] for n in names]
            mean = np.array([num(r["mean"]) for r in s]) / div
            lo = np.array([num(r["ci_lo"]) for r in s]) / div
            hi = np.array([num(r["ci_hi"]) for r in s]) / div
            ax.errorbar(x * shift, mean, yerr=[np.maximum(mean - lo, 0), np.maximum(hi - mean, 0)], color=COLOURS[k], marker="s", ms=4,
                        lw=1, capsize=2, label=NAMES[k] + ", 95% CI of mean")
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xticks(clocks)
        ax.set_xticklabels([f"{c} s" for c in clocks])
        ax.set_xlabel("sampling interval")
        ax.set_ylabel("std of log mid returns / sqrt(interval in s)" if scaled else "std of log mid returns")
        ax.set_title("scaled by sqrt(interval): flat for a random walk" if scaled else "raw", fontsize=9)
        ax.grid(alpha=0.25, which="both")
    axes[0].legend(frameon=False, fontsize=7, loc="upper left")
    fig.suptitle("Volatility against sampling interval\n" + heading(v), fontsize=10)
    fig.tight_layout(rect=(0, 0.03, 1, 0.9))
    save(fig, v, out, "volatility_vs_interval.png")


def main():
    parser = argparse.ArgumentParser(description="Plot one lobsim validation.")
    parser.add_argument("--validation", required=True, help="validation.json written by lobsim_calibrate validate")
    parser.add_argument("--out", help="output directory (default: plots/ beside the json)")
    args = parser.parse_args()
    path = Path(args.validation)
    if not path.is_file():
        fail(f"file not found: {path}")
    with open(path) as f:
        v = json.load(f)
    out = Path(args.out) if args.out else path.parent / "plots"
    out.mkdir(parents=True, exist_ok=True)
    plot_spread(v, out)
    plot_depth(v, out)
    plot_survival(v, out)
    plot_vol(v, out)
    print(f"plot_calibration.py: wrote 4 plots into {out}")


if __name__ == "__main__":
    main()
