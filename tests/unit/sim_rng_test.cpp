#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>

#include "sim/rng.hpp"

using namespace sim;

TEST(SplitMix64, KnownAnswersFromSeedZero) {
  SplitMix64 g(0);
  EXPECT_EQ(g.next(), 0xE220A8397B1DCDAFULL);
  EXPECT_EQ(g.next(), 0x6E789E6AA1B965F4ULL);
  EXPECT_EQ(g.next(), 0x06C45D188009454FULL);
}

TEST(Xoshiro, KnownAnswersFromState1234) {
  auto g = Xoshiro256StarStar::from_state({1, 2, 3, 4});
  EXPECT_EQ(g.next_u64(), 11520ULL);
  EXPECT_EQ(g.next_u64(), 0ULL);
  EXPECT_EQ(g.next_u64(), 1509978240ULL);
  EXPECT_EQ(g.next_u64(), 1215971899390074240ULL);
}

TEST(Xoshiro, UniformIntStaysInRangeAndCoversIt) {
  Xoshiro256StarStar g(7);
  std::set<std::int64_t> seen;
  for (int i = 0; i < 5000; ++i) {
    const std::int64_t v = g.uniform_int(-3, 4);
    ASSERT_GE(v, -3);
    ASSERT_LE(v, 4);
    seen.insert(v);
  }
  EXPECT_EQ(seen.size(), 8u);
}

TEST(Xoshiro, UniformIntSingleValueAndFullRange) {
  Xoshiro256StarStar g(8);
  for (int i = 0; i < 100; ++i) EXPECT_EQ(g.uniform_int(5, 5), 5);
  constexpr auto lo = std::numeric_limits<std::int64_t>::min();
  constexpr auto hi = std::numeric_limits<std::int64_t>::max();
  bool neg = false, pos = false;
  for (int i = 0; i < 200; ++i) {
    const std::int64_t v = g.uniform_int(lo, hi);
    neg = neg || v < 0;
    pos = pos || v > 0;
  }
  EXPECT_TRUE(neg && pos);
  for (int i = 0; i < 100; ++i) {
    const std::int64_t v = g.uniform_int(hi - 1, hi);
    EXPECT_TRUE(v == hi - 1 || v == hi);
  }
}

TEST(Xoshiro, UniformIntIsUnbiasedOnThreeOutcomes) {
  Xoshiro256StarStar g(9);
  std::array<int, 3> count{};
  constexpr int n = 300000;
  for (int i = 0; i < n; ++i) ++count[static_cast<std::size_t>(g.uniform_int(0, 2))];
  // Each cell is Binomial(n, 1/3): sd about 258. Allow 5 sd.
  for (int c : count) EXPECT_NEAR(c, n / 3.0, 1300.0);
}

TEST(Xoshiro, UniformIntIsUnbiasedWhereModuloBiasIsLarge) {
  // n = 3 * 2^62 does not divide 2^64: taking a draw modulo n would give the lowest third
  // of the range probability 1/2 instead of 1/3. Rejection sampling gives 1/3.
  Xoshiro256StarStar g(13);
  constexpr std::int64_t lo = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t third = std::int64_t{1} << 62;
  constexpr int n = 60000;
  int low = 0;
  for (int i = 0; i < n; ++i) low += g.uniform_int(lo, third - 1) < lo + third ? 1 : 0;  // range size 3 * 2^62
  EXPECT_NEAR(low / static_cast<double>(n), 1.0 / 3.0, 0.012);  // 5 sd; the biased value is 0.5
}

TEST(Xoshiro, Uniform01InHalfOpenUnitInterval) {
  Xoshiro256StarStar g(10);
  double sum = 0;
  for (int i = 0; i < 100000; ++i) {
    const double u = g.uniform01();
    ASSERT_GE(u, 0.0);
    ASSERT_LT(u, 1.0);
    sum += u;
  }
  EXPECT_NEAR(sum / 100000, 0.5, 0.01);
}

TEST(Xoshiro, ExponentialMeanAndRange) {
  Xoshiro256StarStar g(11);
  const double lambda = 4.0;
  double sum = 0;
  constexpr int n = 200000;
  for (int i = 0; i < n; ++i) {
    const double x = g.exponential(lambda);
    ASSERT_GE(x, 0.0);
    ASSERT_TRUE(std::isfinite(x));
    sum += x;
  }
  EXPECT_NEAR(sum / n, 1.0 / lambda, 0.005);
}

TEST(Xoshiro, NormalMeanAndVariance) {
  Xoshiro256StarStar g(12);
  constexpr int n = 200000;
  double sum = 0, sq = 0;
  for (int i = 0; i < n; ++i) {
    const double z = g.normal();
    ASSERT_TRUE(std::isfinite(z));
    sum += z;
    sq += z * z;
  }
  const double mean = sum / n;
  EXPECT_NEAR(mean, 0.0, 0.01);
  EXPECT_NEAR(sq / n - mean * mean, 1.0, 0.02);
}

TEST(Streams, DifferentIdsGiveDifferentSeeds) {
  std::set<std::uint64_t> seeds;
  for (StreamRole role : {StreamRole::System, StreamRole::NoiseTrader, StreamRole::InformedTrader,
                          StreamRole::MarketMaker, StreamRole::Test})
    for (std::uint32_t i = 0; i < 20; ++i)
      for (StreamPurpose p : {StreamPurpose::Decisions, StreamPurpose::Latency, StreamPurpose::Lifetimes})
        seeds.insert(stream_seed(42, role, i, p));
  EXPECT_EQ(seeds.size(), 5u * 20u * 3u);
  EXPECT_NE(stream_seed(1, StreamRole::NoiseTrader, 0, StreamPurpose::Decisions),
            stream_seed(2, StreamRole::NoiseTrader, 0, StreamPurpose::Decisions));
}

TEST(Streams, EveryRoleIndexPurposeGivesADistinctSeed) {
  // Index 0..999 within each role: the seed is a function of (role, index, purpose) only,
  // so these seeds are the same whatever the number of agents; they must not collide.
  std::set<std::uint64_t> seeds;
  std::size_t n = 0;
  for (StreamRole role : {StreamRole::System, StreamRole::NoiseTrader, StreamRole::InformedTrader,
                          StreamRole::MarketMaker, StreamRole::Test})
    for (std::uint32_t i = 0; i < 1000; ++i)
      for (StreamPurpose p : {StreamPurpose::Decisions, StreamPurpose::Latency, StreamPurpose::Lifetimes}) {
        seeds.insert(stream_seed(42, role, i, p));
        ++n;
      }
  EXPECT_EQ(seeds.size(), n);
}

TEST(Xoshiro, NormalIsBoxMullerAndCachesTheSecondValue) {
  Xoshiro256StarStar by_hand(99), g(99);
  const double u1 = 1.0 - by_hand.uniform01();
  const double u2 = by_hand.uniform01();
  const double radius = std::sqrt(-2.0 * std::log(u1));
  const double angle = 6.283185307179586476925286766559 * u2;
  EXPECT_EQ(g.normal(), radius * std::cos(angle));
  EXPECT_EQ(g.normal(), radius * std::sin(angle)) << "the second value comes from the cache";
  // The pair is used up: the third call consumes the next two uniforms.
  const double v1 = 1.0 - by_hand.uniform01();
  const double v2 = by_hand.uniform01();
  EXPECT_EQ(g.normal(), std::sqrt(-2.0 * std::log(v1)) * std::cos(6.283185307179586476925286766559 * v2));
  EXPECT_EQ(g.next_u64(), by_hand.next_u64()) << "exactly four uniforms were consumed in three calls";
}
