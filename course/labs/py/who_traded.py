"""Lab 9.1: what each kind of trader did, read straight from orders.log and trades.csv.

For the noise traders: how many orders were limit or market, buy or sell, and how big. Compare
with their rules (85% limit, 50/50 side, size uniform 1..10). For every role: how many lots it
bought and sold, and its profit or loss marked at the last mid of the run.

Run:  python3 course/labs/py/who_traded.py out/run1
"""
import csv
import json
import sys
from collections import Counter


def roles(cfg):
    """Owner id -> role name, in the simulator's creation order (owner = agent index + 1)."""
    out = {}
    owner = 1
    for _ in range(cfg["noise_traders"]["count"]):
        out[owner] = "noise"
        owner += 1
    if "informed" in cfg:
        out[owner] = "informed"
        owner += 1
    if "mm" in cfg:
        out[owner] = "market maker"
    return out


def main(run_dir):
    summary = json.load(open(f"{run_dir}/summary.json"))
    role_of = roles(summary["config"])

    kinds, sides, sizes = Counter(), Counter(), Counter()
    with open(f"{run_dir}/orders.log") as f:
        next(f)
        for line in f:
            t = line.split()
            if t[1] != "N" or role_of.get(int(t[2])) != "noise":
                continue
            kinds["market" if t[5] == "1" else "limit"] += 1
            sides["buy" if t[4] == "0" else "sell"] += 1
            sizes[int(t[8])] += 1
    total = sum(kinds.values())
    print(f"noise traders sent {total} new orders")
    print(f"  limit {kinds['limit'] / total:.3f}, market {kinds['market'] / total:.3f}   (rule: 0.85 / 0.15)")
    print(f"  buy {sides['buy'] / total:.3f}, sell {sides['sell'] / total:.3f}     (rule: 0.5 / 0.5)")
    print("  size shares:", " ".join(f"{s}:{sizes[s] / total:.3f}" for s in sorted(sizes)), "  (rule: 0.1 each)")

    last_mid = None
    for row in csv.DictReader(open(f"{run_dir}/bbo.csv")):
        if row["mid"]:
            last_mid = float(row["mid"])

    book = {}  # role -> [cash, inventory, lots bought, lots sold]
    for row in csv.DictReader(open(f"{run_dir}/trades.csv")):
        price, qty = int(row["price"]), int(row["qty"])
        buyer, seller = (row["taker_owner"], row["maker_owner"]) if row["aggressor_side"] == "buy" else \
                        (row["maker_owner"], row["taker_owner"])
        for owner, sign in ((int(buyer), 1), (int(seller), -1)):
            b = book.setdefault(role_of.get(owner, "other"), [0, 0, 0, 0])
            b[0] -= sign * price * qty
            b[1] += sign * qty
            b[2 if sign == 1 else 3] += qty
    print(f"\nmarked at the last mid {last_mid} (fees left out):")
    for role, (cash, inv, bought, sold) in sorted(book.items()):
        pnl = cash + inv * last_mid
        print(f"  {role:13s} bought {bought:7d} sold {sold:7d} lots, ends with {inv:+5d}, PnL {pnl:+10.1f} ticks"
              f" ({pnl / (bought + sold):+.3f} per lot)")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out/run1")
