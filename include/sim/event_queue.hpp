#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"
#include "sim/messages.hpp"

namespace sim {

enum class EventKind : std::uint8_t {
  Wakeup,              // an agent's timer
  RequestArrival,      // a request reaches the exchange (agent = sender)
  PrivateDelivery,     // an engine event reaches its owner (agent = receiver)
  MarketDataDelivery,  // public data reaches a subscriber (agent = receiver)
  FundamentalStep,
  Sample,
};

using Payload = std::variant<std::monostate, lob::Request, lob::Event, MarketData>;

struct SimEvent {
  TimeNs time{0};
  std::uint64_t seq{0};  // global counter, incremented on every schedule()
  EventKind kind{EventKind::Wakeup};
  AgentIndex agent{0};
  Payload payload;
};

// Min-heap on (time, seq). `seq` is unique, so this is a strict total order and
// THE tie-break rule of the simulator: events with equal time are processed in the order
// they were scheduled. Nothing else (kind, agent) takes part in the ordering.
class EventQueue {
 public:
  void schedule(TimeNs time, EventKind kind, AgentIndex agent, Payload payload = {}) {
    if (time < now_) throw std::logic_error("EventQueue: scheduling in the past");
    heap_.push_back(SimEvent{time, next_seq_++, kind, agent, std::move(payload)});
    std::push_heap(heap_.begin(), heap_.end(), later);
  }

  bool empty() const noexcept { return heap_.empty(); }
  std::size_t size() const noexcept { return heap_.size(); }
  const SimEvent& top() const { return heap_.front(); }

  // Removes and returns the earliest event; the queue's clock moves to its time.
  SimEvent pop() {
    std::pop_heap(heap_.begin(), heap_.end(), later);
    SimEvent ev = std::move(heap_.back());
    heap_.pop_back();
    now_ = ev.time;
    return ev;
  }

  TimeNs now() const noexcept { return now_; }
  std::uint64_t scheduled() const noexcept { return next_seq_; }

 private:
  static bool later(const SimEvent& a, const SimEvent& b) noexcept {
    return a.time != b.time ? a.time > b.time : a.seq > b.seq;
  }

  std::vector<SimEvent> heap_;
  std::uint64_t next_seq_{0};
  TimeNs now_{0};
};

}  // namespace sim
