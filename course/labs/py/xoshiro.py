"""Lab 7.1: lobsim's random number generator, rewritten in Python.

This is a line-by-line copy of include/sim/rng.hpp. If the two agree, you have seen with
your own eyes that a "random" number generator is a recipe: same recipe + same seed = same
numbers, in any language, on any computer.

Run:  python3 course/labs/py/xoshiro.py
"""
import math

MASK = (1 << 64) - 1  # Python integers never overflow, so we cut them to 64 bits by hand.


class SplitMix64:
    """Used only to turn one seed into the 4 starting numbers of xoshiro (rng.hpp, line 10)."""

    def __init__(self, seed):
        self.state = seed & MASK

    def next(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        return z ^ (z >> 31)


def rotl(x, k):
    """Rotate left: bits that fall off the left end come back on the right."""
    return ((x << k) | (x >> (64 - k))) & MASK


class Xoshiro256StarStar:
    def __init__(self, seed):
        sm = SplitMix64(seed)
        self.s = [sm.next() for _ in range(4)]
        self.has_spare = False
        self.spare = 0.0

    def next_u64(self):
        s = self.s
        result = (rotl((s[1] * 5) & MASK, 7) * 9) & MASK
        t = (s[1] << 17) & MASK
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = rotl(s[3], 45)
        return result

    def uniform_int(self, lo, hi):
        """Every value in [lo, hi] equally likely. Draws below 2^64 mod n are thrown away."""
        n = hi - lo + 1
        threshold = (1 << 64) % n
        while True:
            r = self.next_u64()
            if r >= threshold:
                return lo + r % n

    def uniform01(self):
        """A number in [0, 1): the top 53 bits of one draw, times 2^-53."""
        return (self.next_u64() >> 11) * 2.0 ** -53

    def exponential(self, lam):
        """Waiting time with rate `lam` (mean 1/lam): -ln(1 - u) / lam."""
        return -math.log(1.0 - self.uniform01()) / lam

    def normal(self):
        """Bell-curve number (Box-Muller). Each pair of uniforms gives two normals."""
        if self.has_spare:
            self.has_spare = False
            return self.spare
        u1 = 1.0 - self.uniform01()
        u2 = self.uniform01()
        radius = math.sqrt(-2.0 * math.log(u1))
        angle = 6.283185307179586476925286766559 * u2
        self.spare = radius * math.sin(angle)
        self.has_spare = True
        return radius * math.cos(angle)


def stream_seed(master_seed, role, index, purpose):
    """rng.hpp stream_seed(): each (role, index, purpose) gets its own seed."""
    stream_id = (role << 32) | (index << 8) | purpose
    return SplitMix64((master_seed ^ ((0x9E3779B97F4A7C15 * (stream_id + 1)) & MASK)) & MASK).next()


if __name__ == "__main__":
    g = Xoshiro256StarStar(42)
    print("next_u64     ", [g.next_u64() for _ in range(3)])
    print("uniform01    ", [round(g.uniform01(), 12) for _ in range(3)])
    print("uniform_int  ", [g.uniform_int(1, 10) for _ in range(5)])
    print("exponential  ", [round(g.exponential(5.0), 12) for _ in range(3)])
    print("normal       ", [round(g.normal(), 12) for _ in range(3)])
    # Role 1 = NoiseTrader, index 0, purpose 0 = Decisions (see StreamRole and StreamPurpose).
    print("noise trader 0's decision seed:", stream_seed(42, 1, 0, 0))
