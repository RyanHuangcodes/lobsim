#pragma once

#include <cstdint>

#include "lob/types.hpp"
#include "sim/agent.hpp"
#include "sim/fundamental.hpp"
#include "sim/messages.hpp"

namespace sim {

struct InformedTraderParams {
  double lambda{2.0};       // wakeups per second (Poisson)
  double theta_ticks{2.0};  // minimum edge, in ticks, to trade
  lob::Qty size{5};         // lots per market order
};

// Informed trader (SPEC 5.6 item 2). On each wakeup it compares the current fundamental
// value V with its last received (delayed) BBO: market buy if V - ask >= theta, else
// market sell if bid - V >= theta, else nothing.
//
// Random draws, all from ctx.rng(), in this fixed order:
//   on_start:  one exponential (time to the first wakeup).
//   on_wakeup: one exponential (time to the next wakeup), drawn BEFORE the trading
//              decision, so the wakeup schedule never depends on the market.
// on_market_data and on_private draw nothing.
class InformedTrader : public Agent {
 public:
  // `fundamental` must outlive the trader; it is the only thing the trader reads besides
  // its own messages.
  InformedTrader(const InformedTraderParams& params, const Fundamental& fundamental);

  void on_start(Context& ctx) override;
  void on_wakeup(Context& ctx) override;
  void on_market_data(Context& ctx, const MarketData& msg) override;
  void on_private(Context& ctx, const lob::Event& event) override;

  // Read-only views for tests and metrics.
  const BBO& local_bbo() const noexcept { return bbo_; }
  std::uint64_t buys_sent() const noexcept { return buys_sent_; }
  std::uint64_t sells_sent() const noexcept { return sells_sent_; }
  std::uint64_t private_events() const noexcept { return private_events_; }

 private:
  InformedTraderParams p_;
  const Fundamental& fundamental_;
  BBO bbo_{};
  std::uint64_t next_client_id_{1};
  std::uint64_t buys_sent_{0}, sells_sent_{0}, private_events_{0};
};

}  // namespace sim
