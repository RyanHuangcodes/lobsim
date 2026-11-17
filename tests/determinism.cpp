// Same config and seed give identical outputs; the layout of the random streams does not
// depend on how many agents exist (SPEC 0 item 4, 5.2; design section 12).
#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "sim/order_log.hpp"
#include "support/sim_support.hpp"

using namespace sim;

namespace {

using Row = std::tuple<std::uint64_t, lob::OrderType, lob::Side, lob::Qty>;

// For each owner, its NewOrder lines projected to (client_order_id, type, side, qty).
std::vector<std::vector<Row>> projection(const std::string& log, std::size_t owners) {
  std::vector<std::vector<Row>> rows(owners + 1);
  std::istringstream in(log);
  std::string line;
  std::getline(in, line);  // header
  while (std::getline(in, line)) {
    const auto p = parse_request(line);
    EXPECT_TRUE(p) << line;
    if (!p) continue;
    if (const auto* o = std::get_if<lob::NewOrder>(&p->second))
      if (o->owner >= 1 && o->owner <= owners) rows[o->owner].emplace_back(o->client_order_id, o->type, o->side, o->qty);
  }
  return rows;
}

}  // namespace

TEST(Determinism, SameSeedTwiceInOneProcessIsIdentical) {
  const SimConfig cfg = simtest::noise_config(8, 2024, 60.0);
  const simtest::RunResult a = simtest::run(cfg);
  const simtest::RunResult b = simtest::run(cfg);
  EXPECT_GT(a.requests, 1000u);
  EXPECT_EQ(a.log, b.log);
  EXPECT_EQ(a.hash, b.hash);
  EXPECT_EQ(a.trades, b.trades);
  EXPECT_EQ(a.resting, b.resting);
}

TEST(Determinism, DifferentSeedGivesADifferentRun) {
  const simtest::RunResult a = simtest::run(simtest::noise_config(8, 2024, 30.0));
  const simtest::RunResult b = simtest::run(simtest::noise_config(8, 2025, 30.0));
  EXPECT_NE(a.hash, b.hash);
  EXPECT_NE(a.log, b.log);
}

TEST(Determinism, DefaultConfigWithOptionalSectionsPresentIsStillDeterministic) {
  SimConfig cfg = simtest::default_config();  // with the informed trader and the market maker
  cfg.duration_s = 20;
  cfg.warmup_s = 0;
  finalize(cfg);
  EXPECT_EQ(simtest::run(cfg).hash, simtest::run(cfg).hash);
}

namespace {

void expect_common_prefix_equal(const simtest::RunResult& small, const simtest::RunResult& big, std::uint32_t n) {
  EXPECT_NE(small.hash, big.hash) << "the extra trader does trade";
  const auto a = projection(small.log, n);
  const auto b = projection(big.log, n);
  for (std::size_t owner = 1; owner <= n; ++owner) {
    const std::size_t common = std::min(a[owner].size(), b[owner].size());
    ASSERT_GT(common, 50u) << "owner " << owner;
    for (std::size_t i = 0; i < common; ++i) ASSERT_EQ(a[owner][i], b[owner][i]) << "owner " << owner << " line " << i;
  }
}

SimConfig with_jitter(std::uint32_t count, std::uint64_t seed, double duration_s, TimeNs jitter_ns) {
  SimConfig c = simtest::noise_config(count, seed, duration_s);
  c.noise_traders.latency.jitter = jitter_ns;
  finalize(c);
  return c;
}

}  // namespace

TEST(Determinism, AddingANoiseTraderDoesNotChangeTheOthersDecisions) {
  constexpr std::uint32_t n = 5;
  expect_common_prefix_equal(simtest::run(simtest::noise_config(n, 7, 60.0)),
                             simtest::run(simtest::noise_config(n + 1, 7, 60.0)), n);
}

TEST(Determinism, AddingANoiseTraderDoesNotChangeTheOthersDecisionsUnderHugeJitter) {
  // The jitter (300 ms) is longer than the mean gap between a trader's wakeups (200 ms),
  // so an Ack often arrives on the far side of the next wakeup, and which side depends on
  // the jitter draws of every other agent. If a message-triggered draw (the order
  // lifetime) came from the decision stream, adding a trader would shift the others'
  // decisions; with the default 50 us jitter this almost never happens, so that bug would
  // go unseen. This is the layout test for decision D-072.
  constexpr std::uint32_t n = 5;
  constexpr TimeNs jitter = 300'000'000;
  expect_common_prefix_equal(simtest::run(with_jitter(n, 7, 60.0, jitter)),
                             simtest::run(with_jitter(n + 1, 7, 60.0, jitter)), n);
}

namespace {

// Runs the default config (all agents, 60 s, warm-up 0) with and without one optional section and
// checks that every noise trader's own NewOrder decisions agree on their common prefix.
void expect_noise_decisions_unchanged(bool drop_mm, bool drop_informed, TimeNs jitter_ns) {
  SimConfig with = simtest::default_config();
  with.duration_s = 60.0;
  with.warmup_s = 0;
  if (jitter_ns > 0) {
    with.noise_traders.latency.jitter = jitter_ns;
    if (with.informed) with.informed->latency.jitter = jitter_ns;
    if (with.mm) with.mm->latency.jitter = jitter_ns;
  }
  finalize(with);
  SimConfig without = with;
  if (drop_mm) without.mm.reset();
  if (drop_informed) without.informed.reset();
  finalize(without);

  const simtest::RunResult a = simtest::run(without);
  const simtest::RunResult b = simtest::run(with);
  EXPECT_NE(a.hash, b.hash) << "the extra agent does trade";
  const std::uint32_t n = with.noise_traders.count;
  const auto pa = projection(a.log, n);
  const auto pb = projection(b.log, n);
  for (std::size_t owner = 1; owner <= n; ++owner) {
    const std::size_t common = std::min(pa[owner].size(), pb[owner].size());
    ASSERT_GT(common, 50u) << "owner " << owner;
    for (std::size_t i = 0; i < common; ++i) ASSERT_EQ(pa[owner][i], pb[owner][i]) << "owner " << owner << " line " << i;
  }
}

constexpr TimeNs kHugeJitter = 300'000'000;

}  // namespace

TEST(Determinism, AddingTheMarketMakerDoesNotChangeTheNoiseTradersDecisions) {
  expect_noise_decisions_unchanged(true, false, 0);
}

TEST(Determinism, AddingTheMarketMakerDoesNotChangeTheNoiseTradersDecisionsUnderHugeJitter) {
  expect_noise_decisions_unchanged(true, false, kHugeJitter);
}

TEST(Determinism, AddingTheInformedTraderDoesNotChangeTheNoiseTradersDecisions) {
  expect_noise_decisions_unchanged(false, true, 0);
}

TEST(Determinism, AddingTheInformedTraderDoesNotChangeTheNoiseTradersDecisionsUnderHugeJitter) {
  expect_noise_decisions_unchanged(false, true, kHugeJitter);
}
