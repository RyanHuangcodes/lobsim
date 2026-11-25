// Lab 8: the simulator's clock. The event queue, its one tie-break rule, and a latency channel.
//
//   course/labs/build_lab.sh course/labs/lab08_event_queue.cpp && build/release/labs/lab08_event_queue
//
// Part 1 schedules events out of order and with equal times, then pops them. Before you run it,
// write down the order you expect. The rule (include/sim/event_queue.hpp): smallest time first;
// equal times in the order they were scheduled (seq). Nothing else matters: not the kind of
// event, not the agent.
//
// Part 2 sends ten messages through one Channel (include/sim/latency.hpp) with a big random
// jitter, and shows that a message never overtakes an earlier one on the same channel.
#include <cstdio>

#include "sim/event_queue.hpp"
#include "sim/latency.hpp"
#include "sim/rng.hpp"

namespace {

const char* name(sim::EventKind k) {
  switch (k) {
    case sim::EventKind::Wakeup: return "Wakeup";
    case sim::EventKind::RequestArrival: return "RequestArrival";
    case sim::EventKind::PrivateDelivery: return "PrivateDelivery";
    case sim::EventKind::MarketDataDelivery: return "MarketDataDelivery";
    case sim::EventKind::FundamentalStep: return "FundamentalStep";
    case sim::EventKind::Sample: return "Sample";
  }
  return "?";
}

}  // namespace

int main() {
  std::printf("Part 1: the event queue\n");
  sim::EventQueue q;
  q.schedule(300, sim::EventKind::Wakeup, 2);             // seq 0
  q.schedule(100, sim::EventKind::Sample, 0);             // seq 1
  q.schedule(300, sim::EventKind::RequestArrival, 1);     // seq 2: same time as seq 0
  q.schedule(200, sim::EventKind::FundamentalStep, 0);    // seq 3
  q.schedule(300, sim::EventKind::PrivateDelivery, 0);    // seq 4: same time again
  q.schedule(100, sim::EventKind::MarketDataDelivery, 5); // seq 5: same time as seq 1
  while (!q.empty()) {
    const sim::SimEvent e = q.pop();
    std::printf("  time %3lld  seq %llu  %-19s agent %u\n", static_cast<long long>(e.time),
                static_cast<unsigned long long>(e.seq), name(e.kind), e.agent);
  }

  std::printf("\nPart 2: one latency channel, 100 us base + up to 500 us jitter\n");
  sim::Channel channel(100'000, 500'000);  // nanoseconds
  sim::Xoshiro256StarStar rng(7);
  for (int i = 0; i < 10; ++i) {
    const sim::TimeNs sent = i * 50'000;  // a message every 50 us
    const sim::TimeNs arrives = channel.arrival(sent, rng);
    std::printf("  message %d sent at %4lld us, arrives at %4lld us\n", i, static_cast<long long>(sent / 1000),
                static_cast<long long>(arrives / 1000));
  }
  std::printf("The arrival times never go down: Channel::arrival() returns max(sampled, last arrival).\n");
}
