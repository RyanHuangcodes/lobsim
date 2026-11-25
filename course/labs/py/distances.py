"""Lab 14.2: how far apart are two distributions? Total variation and Wasserstein-1.

calib::total_variation and calib::wasserstein1 (src/calib/calibration.cpp) compare two
histograms ("PMFs") on whole numbers, for example where orders are placed (in ticks) or how
big they are (in shares):
    TV = 1/2 * sum over x of |P(x) - Q(x)|           between 0 (same shape) and 1 (no overlap)
    W1 = sum over x of |F_P(x) - F_Q(x)|              in the variable's units (ticks, shares)
F is the running total (cumulative distribution). TV only asks "how much probability is in the
wrong place"; W1 also asks "how far did it have to move".

Run:  python3 course/labs/py/distances.py
"""


def normalise(counts):
    total = sum(counts.values())
    return {x: c / total for x, c in counts.items()}


def total_variation(p_counts, q_counts):
    p, q = normalise(p_counts), normalise(q_counts)
    xs = set(p) | set(q)
    return 0.5 * sum(abs(p.get(x, 0.0) - q.get(x, 0.0)) for x in xs)


def wasserstein1(p_counts, q_counts):
    p, q = normalise(p_counts), normalise(q_counts)
    lo, hi = min(set(p) | set(q)), max(set(p) | set(q))
    fp = fq = 0.0
    w = 0.0
    for x in range(lo, hi):  # the step from x to x + 1
        fp += p.get(x, 0.0)
        fq += q.get(x, 0.0)
        w += abs(fp - fq)
    return w


if __name__ == "__main__":
    same = {0: 5, 1: 3, 2: 2}
    print("identical:          TV", total_variation(same, same), " W1", wasserstein1(same, same))
    near = ({0: 1}, {1: 1})
    far = ({0: 1}, {10: 1})
    print("all at 0 vs all at 1:  TV", total_variation(*near), " W1", wasserstein1(*near))
    print("all at 0 vs all at 10: TV", total_variation(*far), " W1", wasserstein1(*far))
    print("  (TV cannot tell 'a little off' from 'far off' once the supports do not overlap; W1 can)")
    a = {0: 50, 1: 30, 2: 20}
    b1 = {0: 30, 1: 30, 2: 20, 3: 20}  # 20% of a's mass moved from 0 to 3
    b2 = {0: 30, 1: 30, 2: 20, 9: 20}  # the same 20% moved from 0 to 9
    print(f"20% moved 3 ticks:    TV {total_variation(a, b1):.3f}  W1 {wasserstein1(a, b1):.3f}")
    print(f"20% moved 9 ticks:    TV {total_variation(a, b2):.3f}  W1 {wasserstein1(a, b2):.3f}")
