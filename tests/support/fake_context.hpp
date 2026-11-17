#pragma once

// A scripted Context: records what an agent does and lets a test fire its wakeups.
#include <algorithm>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "lob/events.hpp"
#include "sim/agent.hpp"

namespace simtest {

class FakeCtx final : public sim::Context {
 public:
  explicit FakeCtx(std::uint64_t seed) : r(seed) {}
  sim::TimeNs t{0};
  sim::Xoshiro256StarStar r;
  std::vector<lob::Request> sent;
  std::vector<sim::TimeNs> pending;  // wakeups not yet fired

  sim::TimeNs now() const override { return t; }
  void send(const lob::Request& q) override { sent.push_back(q); }
  void schedule_wakeup(sim::TimeNs at) override {
    EXPECT_GE(at, t);
    pending.push_back(at);
  }
  sim::Xoshiro256StarStar& rng() override { return r; }
  lob::OwnerId self() const override { return 7; }

  sim::TimeNs earliest() const { return *std::min_element(pending.begin(), pending.end()); }
  // Fires the earliest pending wakeup (the kernel would: time, then scheduling order).
  void fire(sim::Agent& a) {
    const auto it = std::min_element(pending.begin(), pending.end());
    t = *it;
    pending.erase(it);
    a.on_wakeup(*this);
  }
};

}  // namespace simtest
