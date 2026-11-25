"""Lab 7.2: why uniform_int() throws some draws away.

A generator gives whole numbers that are all equally likely, say 0..7 (3 random bits). To get a
die with 3 faces you might take r % 3. Count what that does:
    r     : 0 1 2 3 4 5 6 7
    r % 3 : 0 1 2 0 1 2 0 1        -> 0 three times, 1 three times, 2 only twice
Faces 0 and 1 come up more often than face 2: "modulo bias". lobsim's uniform_int() fixes it by
rejection sampling: it throws away the few smallest draws (those below 2^64 mod n) so that what is
left divides evenly into n groups (include/sim/rng.hpp, uniform_int).

Run:  python3 course/labs/py/modulo_bias.py
"""
from collections import Counter


def every_outcome(bits, n, reject):
    """Counts of r % n over every possible r with `bits` random bits, equally likely."""
    size = 1 << bits
    threshold = size % n if reject else 0  # the same formula as rng.hpp: 2^bits mod n
    kept = [r % n for r in range(size) if r >= threshold]
    return Counter(kept), size - len(kept)


for bits, n in ((3, 3), (8, 3), (8, 10)):
    plain, _ = every_outcome(bits, n, reject=False)
    fixed, thrown = every_outcome(bits, n, reject=True)
    print(f"{bits} random bits ({1 << bits} values), a die with {n} faces")
    print("   r % n            :", dict(sorted(plain.items())))
    print(f"   with rejection   : {dict(sorted(fixed.items()))}   "
          f"({thrown} value{'' if thrown == 1 else 's'} thrown away)")

print("\nWith 64-bit draws and n = 10, 2^64 mod 10 =", (1 << 64) % 10,
      "values out of 18,446,744,073,709,551,616 are thrown away: almost never, but exactly fair.")
