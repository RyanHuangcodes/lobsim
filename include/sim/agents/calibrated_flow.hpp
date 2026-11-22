#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "calib/calibration.hpp"
#include "lob/events.hpp"
#include "lob/types.hpp"
#include "sim/agent.hpp"
#include "sim/messages.hpp"
#include "sim/rng.hpp"
#include "sim/timer_queue.hpp"

namespace sim {

// An integer distribution as a cumulative table: sampling is one uniform_int draw over the
// total and a binary search, so it is exact and the same on every platform. Built from a
// non-empty calib::Pmf.
class CumulativeTable {
 public:
  CumulativeTable() = default;
  explicit CumulativeTable(const calib::Pmf& pmf);
  bool empty() const noexcept { return values_.empty(); }
  std::uint64_t total() const noexcept { return cum_.empty() ? 0 : cum_.back(); }
  // The value whose cumulative range holds r, r in [0, total()).
  std::int64_t value_at(std::uint64_t r) const;
  // One draw from `rng`.
  std::int64_t sample(Xoshiro256StarStar& rng) const;

 private:
  std::vector<std::int64_t> values_;
  std::vector<std::uint64_t> cum_;  // cum_[i] = counts[0] + ... + counts[i]
};

// Lifetime by inverse CDF: the smallest times_ns[i] with survival[i] <= u, or nullopt when
// u < the curve's last value (the curve never gets that low: no cancel is ever scheduled).
// Only comparisons, no log or exp. A curve with no points never gives a lifetime.
std::optional<TimeNs> lifetime_from_survival(const calib::Survival& curve, double u);

struct CalibratedFlowParams {
  std::shared_ptr<const calib::Calibration> calibration;
  lob::Price start_price{10'000};  // the reference of an empty local book
  lob::Price max_price{1'000'000};
};

struct CalibratedFlowCounters {
  std::uint64_t limits_sent{0};
  std::uint64_t markets_sent{0};
  std::uint64_t placement_redraws{0};   // extra draws because a placement would have crossed
  std::uint64_t placement_clamped{0};   // 100 redraws all crossed: the bound was used
  std::uint64_t fallback_one_sided{0};  // limit orders placed with only one side in the local book
  std::uint64_t fallback_empty{0};      // limit orders placed with an empty local book
  std::uint64_t price_clamped{0};       // a price outside [1, max_price] was moved to it
  std::uint64_t cancels_scheduled{0};
  std::uint64_t no_cancel_draws{0};     // lifetime draws that gave no cancel
  std::uint64_t cancels_sent{0};
  std::uint64_t ignored_rejects{0};
};

// The whole background flow as one agent that samples a calibration file (SPEC upgrade 3, 3).
//
// Two Poisson clocks: limit orders at lambda_L = limit buy + sell per_s, and aggressive orders
// (market, IOC) at lambda_M. Orders cycle through the agent's owner ids.
//
// Random draws, in this fixed order, all from ctx.rng():
//   on_start: the first gap of the limit clock, then of the market clock (a clock with rate 0
//     draws nothing and never fires).
//   limit wakeup: 1. side uniform01() < p_buy; 2. the placement Delta, then one more draw per
//     redraw; 3. the size; 4. the next gap exponential(lambda_L).
//   market wakeup: 1. side; 2. size; 3. the next gap exponential(lambda_M).
// Ack of a limit order: one uniform01() from the agent's own lifetime stream (see Context::rng()).
//
// The local view is the BBO from on_market_data. With a two-sided book the spread s picks the
// placement bucket and Delta must be >= -(s - 1). Otherwise (recorded in DECISIONS.md):
//   own side present, other side absent: reference own best, bucket "3+", nothing to cross;
//   own side absent, other side present: reference other best -/+ 1 tick, bucket "1", bound 0;
//   empty book: reference the start price, bucket "1", bound 0.
class CalibratedFlowAgent : public Agent {
 public:
  CalibratedFlowAgent(const CalibratedFlowParams& params, Xoshiro256StarStar lifetime_rng);

  void on_start(Context& ctx) override;
  void on_wakeup(Context& ctx) override;
  void on_market_data(Context& ctx, const MarketData& msg) override;
  void on_private(Context& ctx, const lob::Event& event) override;

  const BBO& local_bbo() const noexcept { return bbo_; }
  const CalibratedFlowCounters& counters() const noexcept { return counters_; }
  std::size_t open_orders() const noexcept { return open_.size(); }
  double limit_rate() const noexcept { return lambda_limit_; }
  double aggressive_rate() const noexcept { return lambda_market_; }

 private:
  struct LimitClock {};
  struct MarketClock {};
  struct CancelTimer {
    lob::OrderId order_id;
    lob::OwnerId owner;
  };
  using Timer = std::variant<LimitClock, MarketClock, CancelTimer>;
  struct Pending {
    lob::Qty qty;
    int bucket;  // distance bucket of the placement: picks the lifetime curve
    lob::OwnerId owner;
  };
  struct Open {
    lob::Qty qty;
    lob::OwnerId owner;
  };

  void send_limit(Context& ctx);
  void send_market(Context& ctx);
  lob::OwnerId next_owner(Context& ctx);
  void reduce(lob::OrderId id, lob::Qty qty);

  CalibratedFlowParams p_;
  Xoshiro256StarStar lifetime_rng_;
  TimerQueue<Timer> timers_;
  BBO bbo_{};
  double lambda_limit_{0.0}, lambda_market_{0.0};
  double p_limit_buy_{0.5}, p_market_buy_{0.5};
  std::array<std::array<CumulativeTable, calib::kSpreadBuckets>, 2> placement_;
  CumulativeTable limit_size_, aggressive_size_;
  std::uint64_t next_client_id_{1};
  std::uint64_t next_owner_{0};
  std::map<std::uint64_t, Pending> awaiting_ack_;
  std::map<lob::OrderId, Open> open_;
  CalibratedFlowCounters counters_;
};

}  // namespace sim
