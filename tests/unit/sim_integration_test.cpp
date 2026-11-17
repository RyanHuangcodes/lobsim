#include <gtest/gtest.h>

#include <cmath>

#include "support/sim_support.hpp"

using namespace sim;

TEST(Integration, FiveMinutesOfTheDefaultNoiseFlow) {
  // The default config: 10 noise traders (50 wakeups per second), the informed trader
  // and the market maker.
  SimConfig cfg = simtest::default_config();
  cfg.duration_s = 300;
  cfg.warmup_s = 0;
  finalize(cfg);

  Simulator s(cfg);
  ASSERT_NO_THROW(s.run());  // the exchange throws if the book is ever crossed
  EXPECT_EQ(s.agent_count(), 12u);
  EXPECT_GT(s.exchange().trades(), 100u);
  // About 50 wakeups/s * 300 s = 15,000 requests from wakeups, plus cancels.
  EXPECT_GT(s.exchange().requests(), 13'000u);

  // Steady state: limit orders arrive at 50 * 0.85 = 42.5 per second and each lives an
  // Exp(30 s) lifetime, so with no fills the book would settle near 42.5 * 30 = 1,275
  // orders. Market orders (7.5 per second, sized like resting orders) and crossing limits
  // remove roughly 8 to 10 orders per second more, which gives about 1,000. The run is 10
  // lifetimes long, so the book has reached its steady size; allow a wide band around it.
  const std::size_t resting = s.exchange().book().resting_orders();
  EXPECT_GT(resting, 600u);
  EXPECT_LT(resting, 1400u);

  const BBO top = s.exchange().top();
  EXPECT_TRUE(top.two_sided());
  EXPECT_LT(top.bid, top.ask);
  EXPECT_NO_THROW(s.exchange().book().validate());
}

TEST(Integration, ZeroNoiseTradersRunsEmpty) {
  Simulator s(simtest::empty_config(5.0));
  s.run();
  EXPECT_EQ(s.exchange().requests(), 0u);
}

namespace {

// Final mid minus initial price of a noise-only run.
double drift(ReferencePrice mode, std::uint64_t seed) {
  SimConfig cfg = simtest::noise_config(10, seed, 600.0);
  cfg.noise_traders.reference_price = mode;
  finalize(cfg);
  Simulator s(cfg);
  s.run();
  EXPECT_TRUE(s.exchange().last_mid().has_value());
  return s.exchange().last_mid().value_or(0.0) - static_cast<double>(cfg.initial_price);
}

}  // namespace

TEST(Integration, FloorReferenceDriftsDownAndSymmetricDoesNot) {
  double floor_sum = 0, sym_sum = 0;
  for (const std::uint64_t seed : {1, 2, 3, 4}) {
    floor_sum += drift(ReferencePrice::Floor, seed);
    sym_sum += drift(ReferencePrice::Symmetric, seed);
  }
  const double floor_mean = floor_sum / 4, sym_mean = sym_sum / 4;
  // Measured (seeds 1..4, 10 noise traders, 600 s): mean drift -18.625 ticks with Floor
  // and +3.5 ticks with Symmetric. The run is deterministic; the thresholds sit well away
  // from both values so that a change in the rounding rule shows up. Twelve one-hour
  // noise-only runs ended between -91 and -117 ticks with Floor and between -9 and +15 with
  // Symmetric (DECISIONS.md D-089), so the symmetric figure is the mid's own random walk
  // and the floor figure is a systematic bias.
  EXPECT_LT(floor_mean, -10.0) << floor_mean;
  EXPECT_LT(std::abs(sym_mean), 8.0) << sym_mean;
}
