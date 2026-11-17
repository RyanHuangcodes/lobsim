#pragma once

#include <algorithm>

#include "sim/messages.hpp"
#include "sim/rng.hpp"

namespace sim {

// The three one-way delays of one agent (SPEC 5.3), in nanoseconds. Each message is
// delayed by the constant plus an independent uniform integer in [0, jitter].
struct LatencySpec {
  TimeNs entry{0};     // agent -> exchange
  TimeNs response{0};  // exchange -> owner: acks, fills, cancels, rejects
  TimeNs feed{0};      // exchange -> agent: public market data
  TimeNs jitter{0};
  bool operator==(const LatencySpec&) const = default;
};

// One direction for one agent. Per-channel FIFO: a message never arrives before an
// earlier message on the same channel. Two messages with the same arrival time are
// delivered in send order, because the earlier one was scheduled first.
class Channel {
 public:
  Channel() = default;
  Channel(TimeNs base, TimeNs jitter) noexcept : base_(base), jitter_(jitter) {}

  // Arrival time of a message sent at `now`. The jitter is drawn here, at send time.
  TimeNs arrival(TimeNs now, Xoshiro256StarStar& latency_rng) noexcept {
    const TimeNs sampled = now + base_ + (jitter_ > 0 ? latency_rng.uniform_int(0, jitter_) : 0);
    last_arrival_ = std::max(sampled, last_arrival_);
    return last_arrival_;
  }

 private:
  TimeNs base_{0};
  TimeNs jitter_{0};
  TimeNs last_arrival_{0};
};

}  // namespace sim
