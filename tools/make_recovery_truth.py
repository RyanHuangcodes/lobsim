#!/usr/bin/env python3
"""Writes calibration/recovery_truth.json: the hand-made calibration that the parameter-recovery
runs simulate and then estimate back (spec upgrade 3, section 4.1). Standard library only and
deterministic: the same script always writes the same bytes.

    tools/make_recovery_truth.py [--out calibration/recovery_truth.json]
    tools/make_recovery_truth.py --check [--out ...]   # exit 1 if the file is not up to date

The truth, in the schema of calibration/SCHEMA.md (source "hand-made"):
  * placement, both sides alike: P(Delta) proportional to (1 + Delta)^-1.6 on Delta = 0..30, plus 10%
    of the mass on improving placements where the bucket allows them: none in bucket "1" (s = 1
    allows no Delta < 0), Delta = -1 in bucket "2", uniform on -1..-4 in bucket "3+";
  * lifetimes, every distance bucket alike: S(t) = 0.3 * 1[t < 0.5 s] + 0.7 * exp(-t / 20 s), on the
    grid of SURVIVAL_GRID, ending with S = 0 at 600 s so that every order gets a cancel;
  * sizes: round-lot heavy, LIMIT_SIZES and AGGRESSIVE_SIZES;
  * rates: 40 limit and 4 aggressive orders per second, half on each side;
  * tick_units 1, median_mid_ticks 10000.
Probabilities become integer counts at SCALE (10^9), rounded half up; the file's report records it.
"""
import argparse
import math
import os
import sys

SCALE = 10**9
NS = 10**9

# Placement.
POWER_EXPONENT = 1.6
POWER_MAX = 30
IMPROVING_SHARE = 0.10
IMPROVING = {0: [], 1: [-1], 2: [-1, -2, -3, -4]}  # by spread bucket "1", "2", "3+"

# Sizes (shares): probabilities. Round lots carry 85%, odd lots 15%. Aggressive orders are larger
# than limit orders (mean about 970 shares against 217): with sizes alike, the 20 limit orders per
# second and side, half of them at the touch, keep the best level so deep that 2 market orders per
# second never empty it, the spread stays at 1 tick and the "2" and "3+" placement buckets get no
# samples at all (measured: 20 minutes, 0 samples outside bucket "1").
ODD_LOTS = {1: 0.01, 10: 0.02, 20: 0.02, 25: 0.02, 50: 0.04, 75: 0.02, 99: 0.02}  # 15% in all
LIMIT_SIZES = {**ODD_LOTS, 100: 0.40, 200: 0.20, 300: 0.10, 500: 0.10, 1000: 0.05}
AGGRESSIVE_SIZES = {**ODD_LOTS, 100: 0.15, 500: 0.20, 1000: 0.30, 2000: 0.15, 5000: 0.05}

# Lifetimes: S(t) = 0.3 * 1[t < 0.5 s] + 0.7 * exp(-t / 20 s).
FAST_SHARE, FAST_NS, SLOW_MEAN_NS = 0.3, NS // 2, 20 * NS
# (end of segment, step), both in ns; each segment starts where the previous one ended. Fine
# (10 ms) around the median 20 ln(1.4) s = 6.729 s, coarse in the far tail.
SURVIVAL_GRID = [
    (NS // 2, 50_000_000),   # (0, 0.5 s]: the last point is the jump at exactly 0.5 s
    (5 * NS, 50_000_000),    # (0.5 s, 5 s]
    (9 * NS, 10_000_000),    # (5 s, 9 s]: the median
    (20 * NS, 50_000_000),   # (9 s, 20 s]
    (100 * NS, 500_000_000),
    (400 * NS, 5 * NS),
]
END_NS = 600 * NS  # S = 0 here: no order outlives it

RATES = {"limit": 40.0, "aggressive": 4.0}


def counts_of(probabilities):
    """{value: probability} -> sorted values and counts at SCALE, rounded half up."""
    values = sorted(probabilities)
    return values, [int(math.floor(probabilities[v] * SCALE + 0.5)) for v in values]


def placement_bucket(bucket):
    weights = {d: (1.0 + d) ** -POWER_EXPONENT for d in range(POWER_MAX + 1)}
    total = sum(weights.values())
    improving = IMPROVING[bucket]
    power_share = 1.0 - IMPROVING_SHARE if improving else 1.0
    p = {d: power_share * w / total for d, w in weights.items()}
    for d in improving:
        p[d] = IMPROVING_SHARE / len(improving)
    values, counts = counts_of(p)
    return {"values": values, "counts": counts}


def survival(t_ns):
    fast = FAST_SHARE if t_ns < FAST_NS else 0.0
    return fast + (1.0 - FAST_SHARE) * math.exp(-t_ns / SLOW_MEAN_NS)


def lifetime_curve():
    times, start = [], 0
    for end, step in SURVIVAL_GRID:
        assert (end - start) % step == 0
        times.extend(range(start + step, end + 1, step))
        start = end
    surv = [survival(t) for t in times]
    times.append(END_NS)
    surv.append(0.0)
    return {
        "times_ns": times, "survival": surv, "at_risk": [], "events": [], "orders": 0, "cancelled": 0,
        "censored_executed": 0, "censored_window_end": 0, "censored_left_window": 0,
    }


def grid_median_ns(curve):
    return next(t for t, s in zip(curve["times_ns"], curve["survival"]) if s <= 0.5)


def truth():
    curve = lifetime_curve()
    exact_median_s = SLOW_MEAN_NS / NS * math.log((1.0 - FAST_SHARE) / 0.5)
    grid_median = grid_median_ns(curve)
    rate = lambda per_s: {"count": 0, "per_s": per_s / 2.0}  # noqa: E731 - half on each side
    side = lambda f: {"buy": f(), "sell": f()}  # noqa: E731
    placement = {"1": placement_bucket(0), "2": placement_bucket(1), "3+": placement_bucket(2)}
    fit = {"delta0": 1.0, "mu": POWER_EXPONENT - 1.0, "samples": 0,
           "method": "hand-made: the truth's own law (1 + Delta)^-1.6 on Delta = 0..30"}
    limit_values, limit_counts = counts_of(LIMIT_SIZES)
    aggr_values, aggr_counts = counts_of(AGGRESSIVE_SIZES)
    return {
        "schema_version": 1,
        "source": "hand-made",
        "ticker": "TRUTH",
        "date": "2012-06-21",
        "window_from_ns": 0,
        "window_to_ns": 0,
        "observed_ns": 0,
        "window_note": "hand-made recovery truth (tools/make_recovery_truth.py); rates are stated, not measured",
        "inputs": [],
        "code_commit": "unknown",
        "levels": 0,
        "tick_units": 1,
        "median_mid_ticks": 10000,
        "rates": {"limit": side(lambda: rate(RATES["limit"])), "aggressive": side(lambda: rate(RATES["aggressive"]))},
        "placement": {"buy": placement, "sell": placement},
        "placement_fit": {"buy": fit, "sell": fit},
        "limit_size": {"values": limit_values, "counts": limit_counts},
        "aggressive_size": {"values": aggr_values, "counts": aggr_counts},
        "pooled_limit_size": 0,
        "pooled_aggressive_size": 0,
        "lifetime": {b: curve for b in ("<=0", "1-2", "3-9", ">=10")},
        "report": {
            "description": "Parameter-recovery truth (spec upgrade 3, section 4.1). Hand-made, not measured.",
            "placement": "both sides alike: (1 + Delta)^-1.6 on Delta = 0..30; 10% of the mass on improving "
                         "placements where the bucket allows them: none in bucket 1, Delta = -1 in bucket 2, "
                         "uniform on -1..-4 in bucket 3+",
            "lifetime": "S(t) = 0.3 * 1[t < 0.5 s] + 0.7 * exp(-t / 20 s) in every distance bucket, on a grid "
                        "(50 ms to 5 s, 10 ms to 9 s, 50 ms to 20 s, 0.5 s to 100 s, 5 s to 400 s), "
                        "then S = 0 at 600 s",
            "lifetime_median_exact_ns": round(exact_median_s * NS),
            "lifetime_median_grid_ns": grid_median,
            "sizes": "round-lot heavy, 15% odd lots on 1, 10, 20, 25, 50, 75, 99; limit orders 100 (40%), 200 (20%), "
                     "300, 500 (10% each), 1000 (5%); aggressive orders 100 (15%), 500 (20%), 1000 (30%), "
                     "2000 (15%), 5000 (5%)",
            "rounding": "every probability p is stored as the count floor(p * 10^9 + 0.5)",
        },
    }


def emit(value, indent=0):
    """JSON with sorted keys and 2-space indentation; arrays of numbers stay on one line."""
    pad = " " * indent
    if isinstance(value, dict):
        if not value:
            return "{}"
        items = [f'{pad}  "{k}": {emit(value[k], indent + 2)}' for k in sorted(value)]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if isinstance(value, list):
        if all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in value):
            return "[" + ", ".join(emit(v) for v in value) + "]"
        return "[\n" + ",\n".join(pad + "  " + emit(v, indent + 2) for v in value) + "\n" + pad + "]"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        assert math.isfinite(value)
        return repr(value)
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        import json
        return json.dumps(value)
    raise TypeError(type(value))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.join(here, "..", "calibration", "recovery_truth.json")
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=default_out)
    ap.add_argument("--check", action="store_true", help="exit 1 unless --out holds exactly this output")
    args = ap.parse_args()
    text = emit(truth()) + "\n"
    if args.check:
        try:
            with open(args.out, encoding="utf-8") as f:
                current = f.read()
        except OSError as e:
            print(f"make_recovery_truth: cannot read {args.out}: {e}", file=sys.stderr)
            return 1
        if current != text:
            print(f"make_recovery_truth: {args.out} is not up to date; run tools/make_recovery_truth.py",
                  file=sys.stderr)
            return 1
        print(f"make_recovery_truth: {args.out} is up to date")
        return 0
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"make_recovery_truth: wrote {args.out} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
