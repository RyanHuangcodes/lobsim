#include <gtest/gtest.h>

#include "support/sim_support.hpp"

using namespace sim;

TEST(Integration, FiveMinutesOfTheDefaultNoiseFlow) {
  // The default config with the informed trader and market maker sections present (no
  // agent is created for them yet): 10 noise traders, 50 wakeups per second.
  SimConfig cfg = simtest::default_config();
  cfg.duration_s = 300;
  cfg.warmup_s = 0;
  finalize(cfg);

  Simulator s(cfg);
  ASSERT_NO_THROW(s.run());  // the exchange throws if the book is ever crossed
  EXPECT_EQ(s.agent_count(), 10u);
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
