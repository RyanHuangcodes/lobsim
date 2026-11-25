"""Lab 7.2: a Poisson process, the way a noise trader decides when to wake up.

A noise trader wakes at rate lambda = 5 per second, but not like a clock that ticks every
0.2 s. Each wait is an exponential random number with mean 1/lambda (rng.hpp exponential()).
The result is a Poisson process: on average 5 wakeups per second, sometimes 0, sometimes 12.

This uses the Python copy of lobsim's generator (xoshiro.py), so the waits are the same
numbers the C++ code would draw from the same seed.

Run:  python3 course/labs/py/poisson.py
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from xoshiro import Xoshiro256StarStar  # noqa: E402

LAMBDA = 5.0
SECONDS = 2000

rng = Xoshiro256StarStar(42)
t = 0.0
gaps = []
per_second = [0] * SECONDS
while True:
    gap = rng.exponential(LAMBDA)
    t += gap
    if t >= SECONDS:
        break
    gaps.append(gap)
    per_second[int(t)] += 1

print(f"{len(gaps)} wakeups in {SECONDS} s: {len(gaps) / SECONDS:.3f} per second (lambda = {LAMBDA})")
print(f"mean wait {sum(gaps) / len(gaps):.4f} s (1/lambda = {1 / LAMBDA:.4f}), "
      f"shortest {min(gaps) * 1000:.3f} ms, longest {max(gaps):.3f} s")
print("\nwakeups in one second   seen     Poisson formula")
for k in range(0, 13):
    seen = per_second.count(k) / SECONDS
    formula = math.exp(-LAMBDA) * LAMBDA ** k / math.factorial(k)
    bar = "#" * round(seen * 100)
    print(f"  {k:2d}                   {seen:.3f}    {formula:.3f}   {bar}")
