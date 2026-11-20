#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "sim/agent.hpp"
#include "sim/event_queue.hpp"
#include "sim/latency.hpp"
#include "sim/timer_queue.hpp"

using namespace sim;

TEST(EventQueue, PopsInTimeThenSchedulingOrder) {
  EventQueue q;
  q.schedule(30, EventKind::Wakeup, 3);
  q.schedule(10, EventKind::Wakeup, 1);
  q.schedule(20, EventKind::Wakeup, 2);
  EXPECT_EQ(q.pop().agent, 1u);
  EXPECT_EQ(q.pop().agent, 2u);
  EXPECT_EQ(q.pop().agent, 3u);
  EXPECT_TRUE(q.empty());
}

TEST(EventQueue, EqualTimesPopInSchedulingOrderWhateverTheAgentAndKind) {
  EventQueue q;
  // Events at time 100 are scheduled with a shuffled agent id (a permutation of 0..199,
  // neither ascending nor descending) and a kind that cycles through all six in a
  // non-monotone pattern, interleaved with events at other times. A tie broken by agent
  // id or by kind would reorder them; only scheduling order gives `expected`.
  std::vector<AgentIndex> expected;
  for (AgentIndex i = 0; i < 200; ++i) {
    const AgentIndex agent = (i * 37 + 11) % 200;
    q.schedule(100, static_cast<EventKind>((i * 5) % 6), agent);
    expected.push_back(agent);
    q.schedule(50 + static_cast<TimeNs>(i % 7) * 30, EventKind::FundamentalStep, 1000 + i);
  }
  std::vector<AgentIndex> got;
  TimeNs last = 0;
  std::uint64_t last_seq = 0;
  bool first = true;
  while (!q.empty()) {
    const SimEvent ev = q.pop();
    EXPECT_GE(ev.time, last);
    // Every pair of consecutive equal-time events, at every time, is in scheduling order.
    if (!first && ev.time == last) {
      EXPECT_GT(ev.seq, last_seq);
    }
    first = false;
    last = ev.time;
    last_seq = ev.seq;
    if (ev.time == 100 && ev.agent < 1000) got.push_back(ev.agent);
  }
  EXPECT_EQ(got, expected);
  auto by_agent = expected;
  std::sort(by_agent.begin(), by_agent.end());
  EXPECT_NE(expected, by_agent) << "scheduling order must differ from agent order for this test to mean anything";
  std::reverse(by_agent.begin(), by_agent.end());
  EXPECT_NE(expected, by_agent);
}

TEST(EventQueue, SchedulingInThePastThrows) {
  EventQueue q;
  q.schedule(100, EventKind::Wakeup, 0);
  (void)q.pop();
  EXPECT_THROW(q.schedule(99, EventKind::Wakeup, 0), std::logic_error);
  EXPECT_NO_THROW(q.schedule(100, EventKind::Wakeup, 0));
}

TEST(Channel, ConstantWithoutJitter) {
  Channel c(200, 0);
  Xoshiro256StarStar rng(1);
  const auto before = rng;
  EXPECT_EQ(c.arrival(1000, rng), 1200);
  EXPECT_EQ(c.arrival(5000, rng), 5200);
  EXPECT_EQ(rng, before) << "no jitter, no draw";
}

TEST(Channel, JitterWithinBoundsOnSeparatedSends) {
  Xoshiro256StarStar rng(2);
  bool saw_low = false, saw_high = false;
  for (int i = 0; i < 2000; ++i) {
    Channel c(100, 50);  // fresh channel: no FIFO clamp
    const TimeNs a = c.arrival(1000, rng);
    ASSERT_GE(a, 1100);
    ASSERT_LE(a, 1150);
    saw_low = saw_low || a < 1110;
    saw_high = saw_high || a > 1140;
  }
  EXPECT_TRUE(saw_low && saw_high);
}

TEST(Channel, FifoUnderHugeJitter) {
  Channel c(0, 1'000'000);
  Xoshiro256StarStar rng(3);
  TimeNs last = 0;
  bool clamped = false;
  for (TimeNs t = 0; t < 5000; t += 5) {
    const TimeNs a = c.arrival(t, rng);
    ASSERT_GE(a, last);
    ASSERT_GE(a, t);
    clamped = clamped || a == last;
    last = a;
  }
  EXPECT_TRUE(clamped) << "jitter much larger than the gap must have triggered the FIFO clamp";
}

namespace {
class ScriptCtx final : public Context {
 public:
  TimeNs t{0};
  std::vector<TimeNs> wakeups;
  Xoshiro256StarStar r{1};
  TimeNs now() const override { return t; }
  void send(const lob::Request&) override {}
  void schedule_wakeup(TimeNs at) override { wakeups.push_back(at); }
  Xoshiro256StarStar& rng() override { return r; }
  lob::OwnerId self() const override { return 1; }
};
}  // namespace

TEST(TimerQueue, OnePopPerWakeupInTimeThenInsertionOrder) {
  ScriptCtx ctx;
  TimerQueue<int> q;
  q.schedule(ctx, 50, 1);
  q.schedule(ctx, 20, 2);
  q.schedule(ctx, 50, 3);
  q.schedule(ctx, 50, 4);
  EXPECT_EQ(ctx.wakeups.size(), 4u) << "one schedule_wakeup per timer";
  ctx.t = 20;
  EXPECT_EQ(q.pop_due(20), 2);
  EXPECT_EQ(q.pop_due(50), 1);
  EXPECT_EQ(q.pop_due(50), 3);
  EXPECT_EQ(q.pop_due(50), 4);
  EXPECT_EQ(q.size(), 0u);
}

TEST(TimerQueue, WakeupWithoutMatchingTimerThrows) {
  ScriptCtx ctx;
  TimerQueue<int> q;
  EXPECT_THROW(q.pop_due(10), std::logic_error);
  q.schedule(ctx, 20, 1);
  EXPECT_THROW(q.pop_due(10), std::logic_error);
  EXPECT_EQ(q.pop_due(20), 1);
  EXPECT_THROW(q.pop_due(20), std::logic_error);
}
