#!/usr/bin/env python3
"""Multi-run tables for the README, regenerated from scratch.

    python tools/sweep.py --bin build/release/lobsim_run --config configs/default.json --out out/sweep

Starts from the given config, writes variants of it into --out, runs each for several
seeds with lobsim_run, and prints four Markdown tables built only from the files those
runs write (summary.json, bbo.csv, trades.csv and mm_fills.csv):

  1. informed      how the market maker's results change with the informed trader
  2. counterparty  the market maker's fills split by who took them: informed trader or
                   noise trader (spread capture, mid move and markout per lot)
  3. drift         where a noise-only market ends under the two reference-price rules
  4. returns       return statistics with noise traders only, plus the informed trader,
                   plus the market maker

Use --table to print one of them. A simulation with the same config and seed is run only
once and shared between tables; the number of simulations run is printed to stderr.
Python standard library only.
"""
import argparse
import copy
import csv
import json
import statistics
import subprocess
import sys
from pathlib import Path

DEFAULT_SEEDS = [42, 1, 2, 3, 4, 5, 6, 7]
DRIFT_SEEDS = list(range(1, 13))


def vary(base, mutate):
    cfg = copy.deepcopy(base)
    mutate(cfg)
    return cfg


def noise_only(base, rule):
    def mutate(c):
        c.pop("informed", None)
        c.pop("mm", None)
        c["noise_traders"]["reference_price"] = rule

    return vary(base, mutate)


class Runner:
    def __init__(self, binary, out):
        self.binary = binary
        self.out = out
        self.out.mkdir(parents=True, exist_ok=True)
        self.done = {}  # (config text, seed) -> output directory of a run made by this process
        self.runs = 0

    def run(self, name, cfg, seed):
        """Runs one simulation (or reuses an identical one) and returns its output directory."""
        text = json.dumps(cfg, indent=1, sort_keys=True) + "\n"
        key = (text, seed)
        if key in self.done:
            return self.done[key]
        config_path = self.out / f"{name}.json"
        config_path.write_text(text)
        run_dir = self.out / f"{name}_seed_{seed}"
        result = subprocess.run([self.binary, "--config", str(config_path), "--seed", str(seed), "--out", str(run_dir)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode != 0:
            sys.exit(f"sweep.py: {self.binary} failed for {name} seed {seed}:\n{result.stderr}")
        self.done[key] = run_dir
        self.runs += 1
        return run_dir


def summary(run_dir):
    with open(run_dir / "summary.json") as f:
        return json.load(f)


def final_mid(run_dir):
    """The mid in the last row of bbo.csv (the sample at the end of the run)."""
    with open(run_dir / "bbo.csv", newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows or rows[-1]["mid"] == "":
        sys.exit(f"sweep.py: no final mid in {run_dir / 'bbo.csv'}")
    return float(rows[-1]["mid"])


def mean(values):
    return statistics.mean(list(values))


def informed_variants(base):
    return [
        ("default", "none", vary(base, lambda c: None)),
        ("no informed trader", "`informed` section removed", vary(base, lambda c: c.pop("informed"))),
        ("theta 1", "`informed.theta_ticks` 2 to 1", vary(base, lambda c: c["informed"].update(theta_ticks=1))),
        ("size 20", "`informed.size` 5 to 20", vary(base, lambda c: c["informed"].update(size=20))),
        ("lambda 10", "`informed.lambda` 2 to 10", vary(base, lambda c: c["informed"].update({"lambda": 10.0}))),
        ("size 20, lambda 10", "both of the above",
         vary(base, lambda c: c["informed"].update({"size": 20, "lambda": 10.0}))),
        ("floor reference price", '`noise_traders.reference_price` "symmetric" to "floor"',
         vary(base, lambda c: c["noise_traders"].update(reference_price="floor"))),
    ]


def table_informed(runner, base, seeds):
    variants = informed_variants(base)
    lines = []
    horizon = None
    for index, (name, change, cfg) in enumerate(variants):
        rows = []
        for seed in seeds:
            s = summary(runner.run(f"informed_{index}", cfg, seed))
            window = s["market_maker"]["window"]
            by_horizon = {m["horizon_s"]: m for m in s["markouts"]}
            longest = by_horizon[max(by_horizon)]
            horizon = longest["horizon_s"]
            rows.append({
                "informed_lots": s["counts"]["roles"].get("informed", {}).get("taker_volume_lots", 0),
                "pnl": window["pnl_after_warmup"],
                "volume": window["volume_buy_lots"] + window["volume_sell_lots"],
                "spread_capture": s["spread_capture"]["mean"],
                "mid_move": longest["decomposition"]["mid_move_mean"],
                "markout": longest["mean"],
                "max_inventory": window["max_abs_inventory"],
            })
        col = lambda key: [r[key] for r in rows]  # noqa: E731
        lines.append(
            f"| {name} | {change} | {mean(col('informed_lots')):,.0f} | {mean(col('pnl')):,.0f} | "
            f"{min(col('pnl')):,.0f} to {max(col('pnl')):,.0f} | {mean(col('pnl')) / mean(col('volume')):.3f} | "
            f"{mean(col('spread_capture')):+.3f} | {mean(col('mid_move')):+.3f} | {mean(col('markout')):+.3f} | "
            f"{max(col('max_inventory'))} |")
    print(f"### informed: market maker against the informed trader\n")
    print(f"Means over {len(seeds)} seeds ({', '.join(map(str, seeds))}), after the warm-up. Ticks unless stated.\n")
    print(f"| Variant | Change from the default | Informed volume (lots) | MM PnL | PnL range over seeds | PnL per lot | "
          f"Spread capture per lot | Mid move per lot, {horizon:g} s | Markout per lot, {horizon:g} s | "
          f"Largest \\|inventory\\| |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    print("\n".join(lines) + "\n")


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def table_counterparty(runner, base, seeds):
    """Market-maker fills in the window, split by the owner of the taking order."""
    wanted = {"default", "no informed trader", "lambda 10", "size 20, lambda 10"}
    horizons = sorted(base["markout_horizons_s"])
    short, long_ = horizons[0], horizons[-1]
    print("### counterparty: the market maker's fills by who took them\n")
    print(f"Fills after the warm-up whose pre-trade mid and markouts are all defined. Quantity-weighted means per lot, "
          f"ticks, averaged over {len(seeds)} seeds ({', '.join(map(str, seeds))}). Mid move = markout minus spread "
          f"capture.\n")
    print(f"| Variant | Taker | Share of the market maker's lots | Spread capture | Mid move, {short:g} s | "
          f"Mid move, {long_:g} s | Markout, {long_:g} s |")
    print("|---|---|---|---|---|---|---|")
    for index, (name, _change, cfg) in enumerate(informed_variants(base)):
        if name not in wanted:
            continue
        informed_owner = cfg["noise_traders"]["count"] + 1 if "informed" in cfg else None
        per_seed = {}
        for seed in seeds:
            run_dir = runner.run(f"informed_{index}", cfg, seed)
            takers = {row["trade_id"]: int(row["taker_owner"]) for row in read_csv(run_dir / "trades.csv")}
            groups = {}
            for row in read_csv(run_dir / "mm_fills.csv"):
                needed = [row["mid_before"], row[f"markout_{short:g}s"], row[f"markout_{long_:g}s"]]
                if row["in_window"] != "1" or "" in needed:
                    continue
                qty = int(row["qty"])
                sign = 1.0 if row["side"] == "buy" else -1.0
                capture = sign * (float(row["mid_before"]) - float(row["price"]))
                who = "informed" if takers[row["trade_id"]] == informed_owner else "noise"
                g = groups.setdefault(who, {"qty": 0, "cap": 0.0, "mv_s": 0.0, "mv_l": 0.0, "mo_l": 0.0})
                g["qty"] += qty
                g["cap"] += qty * capture
                g["mv_s"] += qty * (float(row[f"markout_{short:g}s"]) - capture)
                g["mv_l"] += qty * (float(row[f"markout_{long_:g}s"]) - capture)
                g["mo_l"] += qty * float(row[f"markout_{long_:g}s"])
            total = sum(g["qty"] for g in groups.values())
            for who, g in groups.items():
                per_seed.setdefault(who, []).append({
                    "share": g["qty"] / total, "cap": g["cap"] / g["qty"], "mv_s": g["mv_s"] / g["qty"],
                    "mv_l": g["mv_l"] / g["qty"], "mo_l": g["mo_l"] / g["qty"]})
        for who in sorted(per_seed):
            rows = per_seed[who]
            col = lambda key: mean(r[key] for r in rows)  # noqa: E731
            print(f"| {name} | {who} | {col('share'):.3f} | {col('cap'):+.3f} | {col('mv_s'):+.3f} | {col('mv_l'):+.3f} | "
                  f"{col('mo_l'):+.3f} |")
    print()


def table_drift(runner, base, seeds):
    initial = base["initial_price"]
    print("### drift: final mid minus initial price, noise traders only\n")
    print(f"One run per seed ({', '.join(map(str, seeds))}), `informed` and `mm` sections removed. Ticks.\n")
    print("| Reference-price rule | Mean | Lowest | Highest | Every seed |")
    print("|---|---|---|---|---|")
    for rule in ["floor", "symmetric"]:
        drifts = [final_mid(runner.run(f"drift_{rule}", noise_only(base, rule), seed)) - initial for seed in seeds]
        each = ", ".join(f"{d:+g}" for d in drifts)
        print(f"| {rule} | {mean(drifts):+.1f} | {min(drifts):+g} | {max(drifts):+g} | {each} |")
    print()


def table_returns(runner, base, seeds):
    populations = [
        ("noise traders only", noise_only(base, base["noise_traders"].get("reference_price", "floor"))),
        ("noise + informed", vary(base, lambda c: c.pop("mm"))),
        ("noise + informed + market maker (default)", vary(base, lambda c: None)),
    ]
    print("### returns: statistics of 1-sample log returns of the mid\n")
    print(f"Means over {len(seeds)} seeds ({', '.join(map(str, seeds))}), after the warm-up, with the range over "
          f"seeds in brackets.\n")
    print("| Agents | Excess kurtosis | Return ACF lag 1 | lag 2 | lag 3 | \\|return\\| ACF lag 1 | lag 5 | lag 10 | "
          "Zero returns | Mean spread (ticks) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for index, (name, cfg) in enumerate(populations):
        stats = []
        for seed in seeds:
            s = summary(runner.run(f"returns_{index}", cfg, seed))
            r = s["returns"]
            stats.append({
                "kurtosis": r["excess_kurtosis"],
                "acf1": r["acf_lags_1_to_5"][0], "acf2": r["acf_lags_1_to_5"][1], "acf3": r["acf_lags_1_to_5"][2],
                "abs1": r["abs_acf_lags_1_to_10"][0], "abs5": r["abs_acf_lags_1_to_10"][4],
                "abs10": r["abs_acf_lags_1_to_10"][9],
                "zero": r["zero_fraction"], "spread": s["spread"]["mean_ticks"],
            })
        col = lambda key: [x[key] for x in stats]  # noqa: E731
        rng = lambda key, fmt: f"{mean(col(key)):{fmt}} [{min(col(key)):{fmt}}, {max(col(key)):{fmt}}]"  # noqa: E731
        print(f"| {name} | {rng('kurtosis', '.2f')} | {rng('acf1', '+.3f')} | {mean(col('acf2')):+.3f} | "
              f"{mean(col('acf3')):+.3f} | {rng('abs1', '+.3f')} | {mean(col('abs5')):+.3f} | {mean(col('abs10')):+.3f} | "
              f"{mean(col('zero')):.2f} | {mean(col('spread')):.2f} |")
    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin", required=True, help="path to lobsim_run")
    parser.add_argument("--config", required=True, help="the base config (configs/default.json)")
    parser.add_argument("--out", required=True, help="directory for the variant configs and the run outputs")
    parser.add_argument("--table", choices=["informed", "counterparty", "drift", "returns", "all"], default="all")
    parser.add_argument("--seeds", type=int, nargs="+", default=None,
                        help=f"seeds for every table (default: {DEFAULT_SEEDS}, and 1..12 for drift)")
    args = parser.parse_args()

    with open(args.config) as f:
        base = json.load(f)
    if "informed" not in base or "mm" not in base:
        sys.exit("sweep.py: the base config needs both an `informed` and an `mm` section")
    runner = Runner(args.bin, Path(args.out))

    if args.table in ("informed", "all"):
        table_informed(runner, base, args.seeds or DEFAULT_SEEDS)
    if args.table in ("counterparty", "all"):
        table_counterparty(runner, base, args.seeds or DEFAULT_SEEDS)
    if args.table in ("drift", "all"):
        table_drift(runner, base, args.seeds or DRIFT_SEEDS)
    if args.table in ("returns", "all"):
        table_returns(runner, base, args.seeds or DEFAULT_SEEDS)
    print(f"sweep.py: {runner.runs} simulations run", file=sys.stderr)


if __name__ == "__main__":
    main()
