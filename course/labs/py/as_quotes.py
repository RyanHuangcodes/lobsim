"""Lab 10.1: the Avellaneda-Stoikov market maker's quotes, by hand.

A copy of compute_quotes() in src/sim/as_market_maker.cpp. The first check reproduces the market
maker's very first two orders in the default run (orders.log lines 4 and 5: a bid at 9995 and an
ask at 9998). Then it shows what inventory and the clock do to the quotes.

Run:  python3 course/labs/py/as_quotes.py
"""
import math

GAMMA = 0.0001   # risk aversion
KAPPA = 1.5      # how quickly fill chances fall as you quote further away
SIGMA = 1.0      # volatility, ticks per square-root second
Q_MAX = 50
T_END = 3600.0   # the session lasts one hour


def reservation_price(mid, q, tau):
    return mid - q * GAMMA * SIGMA ** 2 * tau


def total_spread(tau):
    return GAMMA * SIGMA ** 2 * tau + (2.0 / GAMMA) * math.log(1.0 + GAMMA / KAPPA)


def quotes(bid, ask, q, now_s):
    """The market maker's (bid, ask) given ITS view of the best bid and ask."""
    mid = (bid + ask) / 2.0
    tau = max(0.0, T_END - now_s)
    r = reservation_price(mid, q, tau)
    half = total_spread(tau) / 2.0
    my_bid = min(math.floor(r - half), ask - 1)  # never at or above the best ask
    my_ask = max(math.ceil(r + half), bid + 1)   # never at or below the best bid
    if not my_bid < my_ask:
        my_bid = my_ask - 1
    return (my_bid if q < Q_MAX else None, my_ask if q > -Q_MAX else None), r, half


if __name__ == "__main__":
    # In the default run the market maker first sees bid 9990 (orders.log line 2) and ask 10003
    # (line 3), about 0.021 s after the start, with no inventory.
    (b, a), r, half = quotes(9990, 10003, 0, 0.021)
    print(f"first quote: mid 9996.5, r {r:.4f}, half spread {half:.4f} -> bid {b}, ask {a}")
    print("orders.log says: 21120731 N 12 1 0 0 0 9995 5 1 0  and  21120731 N 12 2 1 0 0 9998 5 1 0\n")

    print("Same book (bid 9999, ask 10001), inventory q changes, start of the hour:")
    for q in (-20, -5, 0, 5, 20, 50):
        (b, a), r, _ = quotes(9999, 10001, q, 0.0)
        print(f"  q = {q:4d}: r = {r:9.2f}  bid {b}  ask {a}")

    print("\nq = 10 lots long, as the hour runs out (the inventory penalty shrinks):")
    for t in (0, 1800, 3300, 3590):
        (b, a), r, half = quotes(9999, 10001, 10, t)
        print(f"  t = {t:4d} s: r = {r:9.3f}  total spread {2 * half:.3f}  bid {b}  ask {a}")
