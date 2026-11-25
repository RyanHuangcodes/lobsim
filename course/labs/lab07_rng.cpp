// Lab 7: the C++ side of the random number generator. Run it next to course/labs/py/xoshiro.py:
// both print the same numbers, because both follow the same recipe from the same seed.
//
//   course/labs/build_lab.sh course/labs/lab07_rng.cpp && build/release/labs/lab07_rng
//   python3 course/labs/py/xoshiro.py
#include <cstdio>

#include "sim/rng.hpp"

int main() {
  sim::Xoshiro256StarStar g(42);
  std::printf("next_u64     ");
  for (int i = 0; i < 3; ++i) std::printf(" %llu", static_cast<unsigned long long>(g.next_u64()));
  std::printf("\nuniform01    ");
  for (int i = 0; i < 3; ++i) std::printf(" %.12f", g.uniform01());
  std::printf("\nuniform_int  ");
  for (int i = 0; i < 5; ++i) std::printf(" %lld", static_cast<long long>(g.uniform_int(1, 10)));
  std::printf("\nexponential  ");
  for (int i = 0; i < 3; ++i) std::printf(" %.12f", g.exponential(5.0));
  std::printf("\nnormal       ");
  for (int i = 0; i < 3; ++i) std::printf(" %.12f", g.normal());
  std::printf("\nnoise trader 0's decision seed: %llu\n",
              static_cast<unsigned long long>(
                  sim::stream_seed(42, sim::StreamRole::NoiseTrader, 0, sim::StreamPurpose::Decisions)));

  // Seeds that differ by one give unrelated streams: SplitMix64 scrambles the seed first.
  sim::Xoshiro256StarStar a(1), b(2);
  std::printf("seed 1 starts %llu\n", static_cast<unsigned long long>(a.next_u64()));
  std::printf("seed 2 starts %llu\n", static_cast<unsigned long long>(b.next_u64()));

  // Puzzle (course/07-randomness.md): three draws inside ONE function call. C++ does not say in
  // which order a call's arguments are worked out, so a compiler may print these in any order.
  // Compare with the first line of this program. lobsim never does this (DECISIONS.md D-047).
  sim::Xoshiro256StarStar p(42);
  std::printf("puzzle       %llu %llu %llu\n", static_cast<unsigned long long>(p.next_u64()),
              static_cast<unsigned long long>(p.next_u64()), static_cast<unsigned long long>(p.next_u64()));
}
