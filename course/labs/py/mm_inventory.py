"""Lab 10.3: the market maker's inventory over the hour, from the one-second samples in bbo.csv.

The Avellaneda-Stoikov inventory penalty is gamma * sigma^2 * (T - t) ticks per lot: large at the
start of the session and close to zero at the end. This script shows what that does to how far the
market maker lets its inventory wander.

Run:  python3 course/labs/py/mm_inventory.py out/run1
"""
import csv
import sys

GAMMA, SIGMA, T = 0.0001, 1.0, 3600.0


def main(run_dir):
    rows = [(int(r["time_ns"]) / 1e9, int(r["mm_inventory"])) for r in csv.DictReader(open(f"{run_dir}/bbo.csv"))
            if r["mm_inventory"] != ""]
    print("window (s)      mean |q|   max |q|   penalty per lot at the window start (ticks)")
    for lo, hi in ((60, 600), (600, 1200), (1200, 1800), (1800, 2400), (2400, 3000), (3000, 3300), (3300, 3600)):
        qs = [abs(q) for t, q in rows if lo <= t < hi]
        print(f"{lo:5.0f}-{hi:<5.0f}    {sum(qs) / len(qs):7.1f}   {max(qs):7d}   {GAMMA * SIGMA ** 2 * (T - lo):.3f}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out/run1")
