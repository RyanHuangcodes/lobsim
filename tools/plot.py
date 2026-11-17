#!/usr/bin/env python3
"""Plots for one lobsim run.

    python tools/plot.py --run out/<run> [--out out/<run>/plots]

Reads bbo.csv and summary.json from the run directory (pandas) and writes five PNGs
(matplotlib, Agg backend): mid_vs_fundamental, mm_inventory, mm_pnl, spread_hist, return_acf.
The warm-up (config.warmup_s) is shaded grey; statistics use only the time after it.
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
import pandas as pd  # noqa: E402

REQUIRED = ["bbo.csv", "summary.json"]
MAX_ACF_LAG = 20


def fail(message):
    print(f"plot.py: {message}", file=sys.stderr)
    sys.exit(1)


def shade_warmup(ax, warmup_s):
    if warmup_s > 0:
        ax.axvspan(0, warmup_s, color="grey", alpha=0.2, label="warm-up")


def autocorrelation(x, lag):
    """sum((x_t - m)(x_{t+lag} - m)) over pairs where both exist / sum((x_t - m)^2)."""
    present = x[~np.isnan(x)]
    if len(present) == 0:
        return float("nan")
    m = present.mean()
    den = float(((present - m) ** 2).sum())
    if den == 0 or lag >= len(x):
        return float("nan")
    a, b = x[:-lag], x[lag:]
    ok = ~np.isnan(a) & ~np.isnan(b)
    if not ok.any():
        return float("nan")
    return float(((a[ok] - m) * (b[ok] - m)).sum() / den)


def empty_note(ax, text):
    ax.text(0.5, 0.5, text, ha="center", va="center", transform=ax.transAxes)


def main():
    parser = argparse.ArgumentParser(description="Plot one lobsim run.")
    parser.add_argument("--run", required=True, help="run directory written by lobsim_run")
    parser.add_argument("--out", help="output directory (default: <run>/plots)")
    args = parser.parse_args()

    run = Path(args.run)
    if not run.is_dir():
        fail(f"run directory not found: {run}")
    for name in REQUIRED:
        if not (run / name).is_file():
            fail(f"missing file {run / name} (is this a lobsim_run output directory?)")
    out = Path(args.out) if args.out else run / "plots"
    out.mkdir(parents=True, exist_ok=True)

    summary = json.loads((run / "summary.json").read_text())
    warmup_s = float(summary["config"]["warmup_s"])
    seed = summary["seed"]
    bbo = pd.read_csv(run / "bbo.csv")
    if bbo.empty:
        fail("bbo.csv has no rows")
    t = bbo["time_ns"] / 1e9
    title = f"seed {seed}"
    window = t >= warmup_s

    # 1. mid vs fundamental
    fig, ax = plt.subplots(figsize=(10, 4))
    ax.plot(t, bbo["fundamental"], lw=0.8, label="fundamental value")
    ax.plot(t, bbo["mid"], lw=0.8, label="exchange mid")
    shade_warmup(ax, warmup_s)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("price (ticks)")
    ax.set_title(f"Mid vs fundamental ({title})")
    ax.legend(loc="best")
    fig.tight_layout()
    fig.savefig(out / "mid_vs_fundamental.png", dpi=120)
    plt.close(fig)

    has_mm = bbo["mm_inventory"].notna().any()

    # 2. market maker inventory
    fig, ax = plt.subplots(figsize=(10, 4))
    if has_mm:
        ax.step(t, bbo["mm_inventory"], where="post", lw=0.8)
        ax.axhline(0, color="black", lw=0.5)
        shade_warmup(ax, warmup_s)
        ax.set_xlabel("time (s)")
        ax.set_ylabel("inventory (lots)")
    else:
        empty_note(ax, "no market maker in this run")
    ax.set_title(f"Market-maker inventory, sampled ({title})")
    fig.tight_layout()
    fig.savefig(out / "mm_inventory.png", dpi=120)
    plt.close(fig)

    # 3. market maker mark-to-market PnL
    fig, ax = plt.subplots(figsize=(10, 4))
    if has_mm:
        ax.plot(t, bbo["mm_pnl"], lw=0.8)
        ax.axhline(0, color="black", lw=0.5)
        shade_warmup(ax, warmup_s)
        ax.set_xlabel("time (s)")
        ax.set_ylabel("mark-to-market PnL (ticks)")
    else:
        empty_note(ax, "no market maker in this run")
    ax.set_title(f"Market-maker PnL, sampled ({title})")
    fig.tight_layout()
    fig.savefig(out / "mm_pnl.png", dpi=120)
    plt.close(fig)

    # 4. spread histogram (samples after the warm-up, both sides present)
    spread = (bbo["ask"] - bbo["bid"])[window].dropna()
    fig, ax = plt.subplots(figsize=(6, 4))
    if len(spread) > 0:
        top = int(spread.max())
        bins = np.arange(0.5, top + 1.5, 1.0)
        ax.hist(spread, bins=bins, weights=np.full(len(spread), 1.0 / len(spread)), edgecolor="black")
        ax.set_xticks(range(1, top + 1))
        ax.set_xlabel("spread (ticks)")
        ax.set_ylabel("fraction of samples")
    else:
        empty_note(ax, "no two-sided samples after the warm-up")
    ax.set_title(f"Spread at the sample times, after warm-up ({title})")
    fig.tight_layout()
    fig.savefig(out / "spread_hist.png", dpi=120)
    plt.close(fig)

    # 5. autocorrelation of returns and of absolute returns, from the sampled mids
    mids = bbo["mid"][window].to_numpy(dtype=float)
    rets = np.full(max(len(mids) - 1, 0), np.nan)
    for k in range(1, len(mids)):
        if not (math.isnan(mids[k]) or math.isnan(mids[k - 1])):
            rets[k - 1] = math.log(mids[k] / mids[k - 1])
    n = int((~np.isnan(rets)).sum())
    fig, axes = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
    for ax, series, label in ((axes[0], rets, "returns"), (axes[1], np.abs(rets), "absolute returns")):
        if n >= 2:
            lags = np.arange(1, MAX_ACF_LAG + 1)
            acf = [autocorrelation(series, int(k)) for k in lags]
            ax.bar(lags, acf, width=0.6)
            band = 1.96 / math.sqrt(n)
            ax.axhline(band, color="red", ls="--", lw=0.8, label="±1.96/√n")
            ax.axhline(-band, color="red", ls="--", lw=0.8)
            ax.axhline(0, color="black", lw=0.5)
            ax.set_xlabel("lag (samples)")
            ax.legend(loc="best")
        else:
            empty_note(ax, "too few returns")
        ax.set_title(f"ACF of {label} (n = {n})")
    axes[0].set_ylabel("autocorrelation")
    fig.suptitle(f"Return autocorrelation, after warm-up ({title})")
    fig.tight_layout()
    fig.savefig(out / "return_acf.png", dpi=120)
    plt.close(fig)

    print(f"wrote 5 plots to {out}")


if __name__ == "__main__":
    main()
