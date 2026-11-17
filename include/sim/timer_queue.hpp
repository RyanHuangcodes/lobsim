#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "sim/agent.hpp"
#include "sim/messages.hpp"

namespace sim {

// on_wakeup() carries no payload, so an agent with several timers keeps them here.
// One schedule() is one timer entry and one Context::schedule_wakeup(); each on_wakeup()
// pops exactly one entry with pop_due(). The agent's Wakeup events and this heap are both
// ordered by (time, order of scheduling), so the entry popped is always the one the wakeup
// was scheduled for: it behaves exactly as if each timer were its own simulator event, and
// adds no second tie-break rule. Timers are never erased; an obsolete one is popped and
// ignored by the agent. A wakeup with no matching timer throws instead of being absorbed.
template <class T>
class TimerQueue {
 public:
  void schedule(Context& ctx, TimeNs at, T payload) {
    heap_.push_back(Entry{at, next_++, std::move(payload)});
    std::push_heap(heap_.begin(), heap_.end(), later);
    ctx.schedule_wakeup(at);
  }

  T pop_due(TimeNs now) {
    if (heap_.empty()) throw std::logic_error("TimerQueue: wakeup with no timer pending");
    if (heap_.front().time != now) throw std::logic_error("TimerQueue: wakeup time does not match the next timer");
    std::pop_heap(heap_.begin(), heap_.end(), later);
    T payload = std::move(heap_.back().payload);
    heap_.pop_back();
    return payload;
  }

  std::size_t size() const noexcept { return heap_.size(); }

 private:
  struct Entry {
    TimeNs time;
    std::uint64_t order;
    T payload;
  };
  static bool later(const Entry& a, const Entry& b) noexcept {
    return a.time != b.time ? a.time > b.time : a.order > b.order;
  }

  std::vector<Entry> heap_;
  std::uint64_t next_{0};
};

}  // namespace sim
