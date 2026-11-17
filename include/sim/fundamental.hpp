#pragma once

#include <cmath>
#include <cstdint>

#include "sim/rng.hpp"

namespace sim {

// The latent fundamental value (SPEC 5.5): a driftless random walk in ticks,
// V <- V + sigma * sqrt(dt) * Z with Z ~ N(0, 1), stepped by the simulator every dt.
// Only the informed trader reads it.
class Fundamental {
 public:
  Fundamental(double initial, double sigma_ticks_per_sqrt_s, double dt_s, Xoshiro256StarStar rng) noexcept
      : value_(initial), step_sd_(sigma_ticks_per_sqrt_s * std::sqrt(dt_s)), rng_(rng) {}

  void step() noexcept {
    value_ += step_sd_ * rng_.normal();
    ++steps_;
  }

  double value() const noexcept { return value_; }
  std::uint64_t steps() const noexcept { return steps_; }

 private:
  double value_;
  double step_sd_;
  Xoshiro256StarStar rng_;
  std::uint64_t steps_{0};
};

}  // namespace sim
