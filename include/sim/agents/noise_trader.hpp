#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <variant>

#include "lob/events.hpp"
#include "lob/types.hpp"
#include "sim/agent.hpp"
#include "sim/messages.hpp"
#include "sim/reference_price.hpp"
#include "sim/rng.hpp"
#include "sim/timer_queue.hpp"

namespace sim {

struct NoiseTraderParams {
  double lambda{5.0};                 // wakeups per second (Poisson)
  double p_limit{0.85};               // probability a wakeup sends a limit order, else a market order
  double order_lifetime_mean_s{30.0}; // mean of the exponential lifetime of a resting order
  lob::Price D{10};                   // limit price offset is uniform in [1, D] ticks
  lob::Qty max_size{10};              // size is uniform in [1, max_size] lots
  lob::Price initial_price{10'000};
  lob::Price max_price{1'000'000};
  ReferencePrice reference_price{ReferencePrice::Floor};
};

// Zero-intelligence trader (SPEC 5.6 item 1, after Smith, Farmer et al.).
//
// Random draws, in this fixed order:
//   on_start:  one exponential (time to the first trading wakeup), from ctx.rng().
//   trading wakeup, all from ctx.rng():
//     1. limit or market   uniform01() < p_limit
//     2. side              uniform_int(0, 1): 0 = buy, 1 = sell
//     3. limit only: d     uniform_int(1, D)
//     4. size              uniform_int(1, max_size)
//     5. time to the next trading wakeup: exponential(lambda)
//   Ack of a limit order: one exponential (the order's lifetime) from the trader's own
//     lifetime stream, never from ctx.rng() (see Context::rng()).
class NoiseTrader : public Agent {
 public:
  NoiseTrader(const NoiseTraderParams& params, Xoshiro256StarStar lifetime_rng);

  void on_start(Context& ctx) override;
  void on_wakeup(Context& ctx) override;
  void on_market_data(Context& ctx, const MarketData& msg) override;
  void on_private(Context& ctx, const lob::Event& event) override;

  // Read-only views for tests and metrics.
  lob::Price reference_price() const;
  // Reference for a buy limit order: ceil(mid) in Symmetric mode with a two-sided BBO,
  // otherwise the same as reference_price().
  lob::Price buy_reference_price() const;
  const BBO& local_bbo() const noexcept { return bbo_; }
  std::size_t open_orders() const noexcept { return open_.size(); }
  std::uint64_t limit_orders_sent() const noexcept { return limits_sent_; }
  std::uint64_t market_orders_sent() const noexcept { return markets_sent_; }
  std::uint64_t cancels_sent() const noexcept { return cancels_sent_; }
  std::uint64_t ignored_rejects() const noexcept { return ignored_rejects_; }

 private:
  struct TradeTimer {};
  struct CancelTimer {
    lob::OrderId order_id;
  };
  using Timer = std::variant<TradeTimer, CancelTimer>;

  void trade(Context& ctx);
  void reduce(lob::OrderId id, lob::Qty qty);

  NoiseTraderParams p_;
  Xoshiro256StarStar lifetime_rng_;
  TimerQueue<Timer> timers_;
  BBO bbo_{};
  std::optional<lob::Price> last_trade_;
  std::uint64_t next_client_id_{1};
  std::map<std::uint64_t, lob::Qty> awaiting_ack_;  // limit orders sent: client_order_id -> qty
  std::map<lob::OrderId, lob::Qty> open_;           // its resting orders, by its own records
  std::uint64_t limits_sent_{0}, markets_sent_{0}, cancels_sent_{0}, ignored_rejects_{0};
};

}  // namespace sim
