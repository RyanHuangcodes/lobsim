"""Lab 11.1: the market maker's money, recomputed from the list of trades.

The simulator's MetricsObserver keeps cash, inventory and fees for the market maker (MM).
This script redoes that bookkeeping from trades.csv alone and compares the result with
summary.json. If they agree, you understand mark-to-market PnL.

    PnL = cash + inventory * last_mid - fees

Run (after `lobsim_run --config configs/default.json --out out/run1`):
    python3 course/labs/py/mm_pnl.py out/run1
"""
import csv
import json
import sys


def main(run_dir):
    summary = json.load(open(f"{run_dir}/summary.json"))
    cfg = summary["config"]
    # Owner ids are agent index + 1, agents in the order noise traders, informed trader, MM.
    mm_owner = cfg["noise_traders"]["count"] + (1 if "informed" in cfg else 0) + 1
    maker_fee = cfg["mm"]["maker_fee_ticks"]
    taker_fee = cfg["mm"]["taker_fee_ticks"]

    cash = 0        # ticks x lots: selling 1 lot at 10000 gives +10000
    inventory = 0   # lots: + means the MM owns them (long), - means it owes them (short)
    fees = 0.0
    lots = 0
    for row in csv.DictReader(open(f"{run_dir}/trades.csv")):
        price, qty = int(row["price"]), int(row["qty"])
        aggressor = row["aggressor_side"]
        for role, owner in (("maker", int(row["maker_owner"])), ("taker", int(row["taker_owner"]))):
            if owner != mm_owner:
                continue
            # The taker's side is the aggressor side; the maker is on the other side.
            mm_buys = (aggressor == "buy") == (role == "taker")
            if mm_buys:
                cash -= price * qty
                inventory += qty
            else:
                cash += price * qty
                inventory -= qty
            fees += qty * (maker_fee if role == "maker" else taker_fee)
            lots += qty

    # The mark: the last mid of the run (the last row of bbo.csv with a mid).
    last_mid = None
    for row in csv.DictReader(open(f"{run_dir}/bbo.csv")):
        if row["mid"]:
            last_mid = float(row["mid"])

    pnl = cash + inventory * last_mid - fees
    whole = summary["market_maker"]["whole_run"]
    print(f"MM owner id          {mm_owner}")
    print(f"lots traded          {lots}")
    print(f"cash                 {cash}   (summary: {whole['cash_ticks']})")
    print(f"inventory            {inventory}   (summary: {whole['final_inventory']})")
    print(f"last mid             {last_mid}")
    print(f"fees                 {fees:.4f}   (summary: {whole['fees_ticks']})")
    print(f"PnL                  {pnl:.4f}   (summary: {whole['pnl_total']})")
    print(f"PnL per lot traded   {pnl / lots:.4f} ticks")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out/run1")
