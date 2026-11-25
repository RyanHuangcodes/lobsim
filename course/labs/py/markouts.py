"""Lab 11.2: markouts, spread capture and adverse selection, from mm_fills.csv.

For a market-maker fill at price p:
    markout(h)     = mid(t + h) - p   for a buy,   p - mid(t + h)   for a sell
    spread capture = mid_before - p   for a buy,   p - mid_before   for a sell
    mid move(h)    = markout(h) - spread capture     (negative: the price moved against the MM)

Only fills after the warm-up count (in_window = 1). The script prints the mean of each, the 95%
interval of the mean markout, and compares with summary.json. Then it splits the fills by who
was on the other side (the informed trader or a noise trader), using trades.csv.

Run:  python3 course/labs/py/markouts.py out/run1
"""
import csv
import json
import math
import statistics
import sys


def side_sign(side):
    return 1.0 if side == "buy" else -1.0


def main(run_dir):
    summary = json.load(open(f"{run_dir}/summary.json"))
    cfg = summary["config"]
    informed_owner = cfg["noise_traders"]["count"] + 1 if "informed" in cfg else None

    # Who took the market maker's quote in each trade: trade_id -> taker owner.
    taker_of = {}
    for row in csv.DictReader(open(f"{run_dir}/trades.csv")):
        taker_of[row["trade_id"]] = int(row["taker_owner"])

    rows = [r for r in csv.DictReader(open(f"{run_dir}/mm_fills.csv")) if r["in_window"] == "1"]
    horizons = [k for k in rows[0].keys() if k.startswith("markout_")]
    print(f"{len(rows)} market-maker fills after the warm-up\n")

    for h in horizons:
        marks = [float(r[h]) for r in rows if r[h] != ""]
        mean = statistics.fmean(marks)
        sd = statistics.stdev(marks)
        half = 1.96 * sd / math.sqrt(len(marks))
        both = [r for r in rows if r[h] != "" and r["mid_before"] != ""]
        capture = [side_sign(r["side"]) * (float(r["mid_before"]) - float(r["price"])) for r in both]
        move = [float(r[h]) - c for r, c in zip(both, capture)]
        ref = next(m for m in summary["markouts"] if f"markout_{m['horizon_s']:g}s" == h)
        print(f"{h:12s} n {len(marks)}  mean {mean:+.4f}  95% interval [{mean - half:+.4f}, {mean + half:+.4f}]"
              f"   (summary: {ref['mean']:+.4f})")
        print(f"{'':12s} = spread capture {statistics.fmean(capture):+.4f} + mid move {statistics.fmean(move):+.4f}")

    print("\nSplit by the taker, 30 s horizon, weighted by lots:")
    h = horizons[-1]
    for name, keep in (("informed", lambda o: o == informed_owner), ("noise", lambda o: o != informed_owner)):
        group = [r for r in rows if r[h] != "" and keep(taker_of[r["trade_id"]])]
        lots = sum(int(r["qty"]) for r in group)
        if lots == 0:
            print(f"  taker {name:8s}: no fills")
            continue
        weighted = sum(float(r[h]) * int(r["qty"]) for r in group) / lots
        print(f"  taker {name:8s}: {lots:6d} lots, markout {weighted:+.3f} ticks per lot")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out/run1")
