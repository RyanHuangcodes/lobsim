#pragma once

#include <cstdint>

namespace lob::test {

// SplitMix64 (Steele, Lea, Flood 2014), with integer helpers built on it only: no
// std:: distributions, whose output differs between standard libraries, so a seed
// reproduces the same stream on every platform.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  // Uniform in [0, n), n > 0, by rejection (no modulo bias).
  std::uint64_t below(std::uint64_t n) noexcept {
    const std::uint64_t threshold = (0 - n) % n;  // 2^64 mod n
    for (;;) {
      const std::uint64_t r = next();
      if (r >= threshold) return r % n;
    }
  }

  // Uniform in [lo, hi], lo <= hi.
  std::int64_t range(std::int64_t lo, std::int64_t hi) noexcept {
    const std::uint64_t span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
    if (span == ~std::uint64_t{0}) return static_cast<std::int64_t>(next());
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + below(span + 1));
  }

  // True with probability percent / 100.
  bool chance(unsigned percent) noexcept { return below(100) < percent; }

 private:
  std::uint64_t state_;
};

}  // namespace lob::test
