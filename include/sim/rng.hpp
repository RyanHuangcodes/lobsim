#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace sim {

// SplitMix64 (Steele, Lea, Flood 2014). Used only to seed other generators.
class SplitMix64 {
 public:
  explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  constexpr std::uint64_t next() noexcept {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  // The first output of a generator seeded with `x`: a 64-bit mixing function.
  static constexpr std::uint64_t mix(std::uint64_t x) noexcept { return SplitMix64(x).next(); }

 private:
  std::uint64_t state_;
};

// xoshiro256** (Blackman, Vigna 2018) with the distributions the simulator needs. The
// distributions are written out here because the std:: ones are implementation-defined
// and give different numbers with different standard libraries (SPEC 5.2).
class Xoshiro256StarStar {
 public:
  explicit Xoshiro256StarStar(std::uint64_t seed) noexcept {
    SplitMix64 sm(seed);
    for (std::uint64_t& s : s_) s = sm.next();
  }
  static Xoshiro256StarStar from_state(const std::array<std::uint64_t, 4>& state) noexcept {
    Xoshiro256StarStar g(0);
    g.s_ = state;
    return g;
  }

  std::uint64_t next_u64() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Uniform integer in [lo, hi] (lo <= hi), by rejection sampling: a draw below
  // 2^64 mod n is thrown away, so every value has exactly the same probability.
  std::int64_t uniform_int(std::int64_t lo, std::int64_t hi) noexcept {
    const std::uint64_t span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
    if (span == ~std::uint64_t{0}) return static_cast<std::int64_t>(next_u64());
    const std::uint64_t n = span + 1;
    const std::uint64_t threshold = (0 - n) % n;  // 2^64 mod n
    for (;;) {
      const std::uint64_t r = next_u64();
      if (r >= threshold) return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + r % n);
    }
  }

  // Uniform double in [0, 1): the top 53 bits of one draw.
  double uniform01() noexcept { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }

  // Exponential with rate `lambda` (mean 1 / lambda): -ln(1 - u) / lambda. Never negative.
  double exponential(double lambda) noexcept { return -std::log(1.0 - uniform01()) / lambda; }

  // Standard normal by Box-Muller. Each pair of draws gives two values; the second is
  // kept and returned by the next call.
  double normal() noexcept {
    if (has_spare_) {
      has_spare_ = false;
      return spare_;
    }
    const double u1 = 1.0 - uniform01();  // in (0, 1], so the logarithm is finite
    const double u2 = uniform01();
    const double radius = std::sqrt(-2.0 * std::log(u1));
    const double angle = 6.283185307179586476925286766559 * u2;
    spare_ = radius * std::sin(angle);
    has_spare_ = true;
    return radius * std::cos(angle);
  }

  bool operator==(const Xoshiro256StarStar&) const = default;

 private:
  static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }

  std::array<std::uint64_t, 4> s_{};
  bool has_spare_{false};
  double spare_{0.0};
};

// Every consumer of randomness has its own stream, derived from the one master seed and
// a stream id built from (role, index within the role, purpose). The id does not depend
// on how many agents exist, so adding an agent never changes another agent's stream.
enum class StreamRole : std::uint32_t { System = 0, NoiseTrader = 1, InformedTrader = 2, MarketMaker = 3, Test = 15 };
enum class StreamPurpose : std::uint32_t {
  Decisions = 0,  // the agent's own-clock decisions: ctx.rng(), used in on_start/on_wakeup only
  Latency = 1,    // jitter on that agent's three channels
  Lifetimes = 2,  // order lifetimes drawn when a reply arrives (noise traders)
};

constexpr std::uint64_t stream_id(StreamRole role, std::uint32_t index, StreamPurpose purpose) noexcept {
  return (static_cast<std::uint64_t>(role) << 32) | (static_cast<std::uint64_t>(index) << 8) |
         static_cast<std::uint64_t>(purpose);
}

constexpr std::uint64_t stream_seed(std::uint64_t master_seed, StreamRole role, std::uint32_t index,
                                    StreamPurpose purpose) noexcept {
  return SplitMix64::mix(master_seed ^ (0x9E3779B97F4A7C15ULL * (stream_id(role, index, purpose) + 1)));
}

inline Xoshiro256StarStar make_stream(std::uint64_t master_seed, StreamRole role, std::uint32_t index,
                                      StreamPurpose purpose) noexcept {
  return Xoshiro256StarStar(stream_seed(master_seed, role, index, purpose));
}

}  // namespace sim
