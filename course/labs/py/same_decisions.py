"""Lab 7.4: adding a trader does not change anybody else's dice.

Each agent draws its random numbers from its own stream, seeded from (master seed, role, index,
purpose) and never from "how many agents are there" (include/sim/rng.hpp, DECISIONS.md D-071).
So a noise trader makes the same decisions (limit or market, buy or sell, size) whether or not
the informed trader exists. Its PRICES can differ, because a limit price is measured from the
market it sees, and the market changes when another trader joins.

Run two simulations that differ only by the informed trader, then this script:
    build/release/lobsim_run --config configs/default.json --out out/run1
    build/release/lobsim_run --config course/labs/configs/no_informed.json --out out/no_informed
    python3 course/labs/py/same_decisions.py out/run1 out/no_informed
"""
import sys


def new_orders(path, owner):
    rows = []
    with open(path) as f:
        next(f)  # the header line
        for line in f:
            t = line.split()
            # <time> N <owner> <client_order_id> <side> <type> <tif> <price> <qty> <post_only> <stp>
            if t[1] == "N" and int(t[2]) == owner:
                rows.append({"client_id": int(t[3]), "side": int(t[4]), "type": int(t[5]),
                             "price": int(t[7]), "qty": int(t[8])})
    return rows


def main(run_a, run_b):
    for owner in range(1, 11):  # the ten noise traders are owners 1 to 10
        a = new_orders(f"{run_a}/orders.log", owner)
        b = new_orders(f"{run_b}/orders.log", owner)
        n = min(len(a), len(b))
        same_decision = sum(1 for x, y in zip(a, b) if (x["client_id"], x["side"], x["type"], x["qty"])
                            == (y["client_id"], y["side"], y["type"], y["qty"]))
        same_price = sum(1 for x, y in zip(a, b) if x["price"] == y["price"])
        print(f"noise trader {owner:2d}: {n} orders compared, same decision {same_decision}, same price {same_price}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
