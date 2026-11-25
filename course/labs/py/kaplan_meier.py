"""Lab 14.1: Kaplan-Meier survival, the way the estimator measures order lifetimes.

An order's lifetime ends with a cancel (an "event"). If it was filled first, or we stopped
watching it, we only know it lived AT LEAST that long: it is "censored". Throwing censored
orders away, or counting them as cancels, both give the wrong answer. Kaplan-Meier uses them
correctly:
    S(t) = product over event times t_i <= t of (1 - d_i / n_i)
where d_i orders were cancelled at t_i and n_i orders were still "at risk" just before t_i.

The first example is the worked example of CALIBRATION.md section 2 (A cancelled at 1 s,
B executed at 2 s, C cancelled at 3 s, D still alive at 5 s). The unit test
KaplanMeier.WorkedExampleOfSection13 checks the same numbers in C++.

Run:  python3 course/labs/py/kaplan_meier.py
"""


def kaplan_meier(events, censored):
    """events: lifetimes that ended in a cancel; censored: lifetimes we stopped seeing.
    Returns [(t_i, n_i, d_i, S(t_i))]. At a time with both, censored orders are still at risk."""
    times = sorted(set(events))
    curve = []
    s = 1.0
    for t in times:
        n = sum(1 for x in events if x >= t) + sum(1 for x in censored if x >= t)
        d = sum(1 for x in events if x == t)
        s *= 1.0 - d / n
        curve.append((t, n, d, s))
    return curve


def median(curve):
    """The first time S(t) <= 0.5 (None if the curve never gets that low)."""
    for t, _, _, s in curve:
        if s <= 0.5:
            return t
    return None


if __name__ == "__main__":
    curve = kaplan_meier(events=[1.0, 3.0], censored=[2.0, 5.0])
    print("worked example:")
    for t, n, d, s in curve:
        print(f"  t = {t} s: at risk {n}, cancelled {d}, S = {s}")
    print("  median lifetime:", median(curve), "s")

    # What goes wrong if you ignore censoring: pretend every order ended in a cancel.
    wrong = kaplan_meier(events=[1.0, 2.0, 3.0, 5.0], censored=[])
    print("\nif B and D were (wrongly) counted as cancels:")
    for t, n, d, s in wrong:
        print(f"  t = {t} s: S = {s}")
    print("  median:", median(wrong), "s  (too short: orders look less patient than they are)")
