#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"
#include "sim/config.hpp"
#include "sim/messages.hpp"
#include "sim/observer.hpp"

namespace sim {

// ---------------------------------------------------------------------------------------
// Pure statistics helpers. Each takes hand-made data and has a worked example in
// tests/unit/sim_metrics_test.cpp.
// ---------------------------------------------------------------------------------------
namespace stats {

// A series with holes: a missing observation is std::nullopt.
using Series = std::vector<std::optional<double>>;

std::optional<double> mean(std::span<const double> x);  // nullopt when empty
// Sample standard deviation (divisor n - 1); nullopt when n < 2.
std::optional<double> sample_sd(std::span<const double> x);
// m4 / m2^2 - 3 with central moments divided by n; nullopt when n < 4 or m2 == 0.
std::optional<double> excess_kurtosis(std::span<const double> x);
// sum over pairs (t, t+lag) with both present of (x_t - mean)(x_{t+lag} - mean), divided by
// sum over all present values of (x_t - mean)^2; mean over all present values. nullopt when
// no pair exists or the denominator is 0.
std::optional<double> autocorrelation(const Series& x, std::size_t lag);

struct Interval {
  double lo{0};
  double hi{0};
};
// mean +- 1.96 * sd / sqrt(n); nullopt when n < 2.
std::optional<Interval> confidence_interval_95(std::size_t n, double mean, double sd);

// Per-lot markout in ticks: m - p for a buy, p - m for a sell.
double markout_per_lot(lob::Side side, lob::Price price, double mid_later);

// log(m_k / m_{k-1}) where both consecutive samples are present, nullopt otherwise. The
// result has one entry per sample after the first.
Series log_returns(const Series& mids);

// A step function summarised by how long it spent at each value: value -> weight.
using Weighted = std::map<std::int64_t, std::int64_t>;
std::optional<double> weighted_mean(const Weighted& w);
// Smallest value v whose cumulative weight reaches p * total weight (0 < p <= 1); nullopt if
// the total weight is 0.
std::optional<std::int64_t> weighted_quantile(const Weighted& w, double p);

}  // namespace stats

// ---------------------------------------------------------------------------------------
// The metrics observer.
// ---------------------------------------------------------------------------------------
enum class Role : std::uint8_t { Noise, Informed, MarketMaker, Other };
inline constexpr std::size_t kRoleCount = 3;  // the roles reported: Noise, Informed, MarketMaker

inline constexpr std::int64_t kSpreadHistogramMax = 40;  // bins for spreads 1..40, then overflow

struct MetricsOptions {
  TimeNs warmup_ns{0};
  TimeNs end_ns{0};
  TimeNs sample_interval_ns{0};
  std::vector<TimeNs> markout_horizons_ns;
  std::vector<double> markout_horizons_s;  // same length, for the output
  double initial_fundamental{0};
  std::optional<lob::OwnerId> mm_owner;
  double maker_fee_ticks{0};
  double taker_fee_ticks{0};
  std::vector<Role> roles;  // by agent index; an agent beyond the list is Role::Other
};

// Options for a Simulator built from `cfg`, using its agent creation order (noise traders,
// the informed trader if configured, the market maker if configured).
MetricsOptions make_metrics_options(const SimConfig& cfg);

// markout(h) = spread_capture + mid_move(h), over the window fills where the markout and
// the pre-trade mid are both defined.
struct Decomposition {
  std::uint64_t n{0};
  std::optional<double> markout_mean, spread_capture_mean, mid_move_mean, mid_move_sd, mid_move_ci_lo,
      mid_move_ci_hi, mid_move_mean_qty_weighted;
};

struct MarkoutStats {
  double horizon_s{0};
  std::uint64_t n{0};
  std::uint64_t skipped{0};
  std::optional<double> mean, sd, ci_lo, ci_hi, mean_qty_weighted;
  Decomposition decomposition;
};

// What the fills earned against the pre-trade mid (window fills with a defined mid_before).
struct SpreadCaptureStats {
  std::uint64_t n{0};
  std::optional<double> mean, sd, ci_lo, ci_hi, mean_qty_weighted;
};

struct MmWindowStats {
  std::uint64_t fills{0}, buy_fills{0}, sell_fills{0};
  std::int64_t volume_buy{0}, volume_sell{0};
  double pnl_after_warmup{0};
  std::int64_t max_abs_inventory{0};
  std::optional<double> quoting_fraction;      // null when the window is empty
  std::optional<double> avg_quoted_spread;     // null when it never quoted both sides
};

struct MmTotalStats {
  std::uint64_t fills{0}, buy_fills{0}, sell_fills{0};
  std::int64_t volume_buy{0}, volume_sell{0};
  double pnl_total{0};
  std::int64_t cash{0};
  double inventory_value{0};
  double fees{0};
  std::int64_t final_inventory{0};
  std::int64_t max_abs_inventory{0};
};

struct SpreadStats {
  std::optional<double> two_sided_fraction;
  std::optional<double> mean;
  std::optional<std::int64_t> median, p90, p99;
  std::vector<double> histogram;  // fraction of two-sided time at spread 1..kSpreadHistogramMax
  double overflow{0};
};

struct ReturnStats {
  std::uint64_t samples{0}, missing{0}, returns{0};
  std::optional<double> mean, sd, zero_fraction, excess_kurtosis;
  std::vector<std::optional<double>> acf;      // lags 1..5
  std::vector<std::optional<double>> abs_acf;  // lags 1..10
};

struct RoleCounts {
  std::uint64_t orders_sent{0};
  std::int64_t taker_volume{0}, maker_volume{0};
};

struct RunCounts {
  std::uint64_t requests_new{0}, requests_cancel{0}, requests_modify{0};
  std::uint64_t ev_ack{0}, ev_reject{0}, ev_trade{0}, ev_cancelled{0}, ev_modified{0};
  std::uint64_t trades{0};
  std::int64_t traded_volume{0};
  std::array<std::uint64_t, 7> rejects{};  // by lob::RejectReason
  std::array<RoleCounts, kRoleCount> roles{};
};

struct MetricsSummary {
  bool has_mm{false};
  MmWindowStats mm_window;
  MmTotalStats mm_total;
  std::vector<MarkoutStats> markouts;
  SpreadCaptureStats spread_capture;
  SpreadStats spread;
  ReturnStats returns;
  RunCounts counts;
};

// Computes every metric of SPEC 6 from ground truth (engine events and the true top of
// book). Read-only: it never schedules, sends or draws, so attaching it changes nothing.
class MetricsObserver final : public Observer {
 public:
  explicit MetricsObserver(MetricsOptions options);

  void on_agent_send(TimeNs now, AgentIndex agent, const lob::Request& request) override;
  void on_request(TimeNs now, const lob::Request& request) override;
  void on_engine_event(TimeNs now, const lob::Event& event) override;
  void on_book(TimeNs now, const BBO& top, bool changed) override;
  void on_fundamental(TimeNs now, double value) override;
  void on_sample(TimeNs now) override;
  void on_end(TimeNs end) override;

  // Valid after the run (on_end).
  MetricsSummary summary() const;

  // The three CSV files and the summary (text, trailing newline).
  void write_trades_csv(std::ostream& out) const;
  void write_bbo_csv(std::ostream& out) const;
  void write_mm_fills_csv(std::ostream& out) const;
  std::string summary_json(const SimConfig& cfg, std::uint64_t engine_event_hash) const;

  std::int64_t mm_inventory() const noexcept { return inventory_; }

 private:
  struct TradeRow {
    TimeNs time;
    lob::Trade trade;
  };
  struct Fill {
    TimeNs time;
    std::uint64_t trade_id;
    lob::Side side;
    lob::Price price;
    lob::Qty qty;
    bool maker;
    std::int64_t inventory_after;
    bool in_window;
    std::optional<double> mid_before;  // mid in force just before the request that caused the trade
    std::vector<std::optional<double>> markouts;  // by horizon
  };
  struct BboRow {
    TimeNs time;
    BBO top;
    std::optional<double> mid;
    double fundamental;
    std::int64_t mm_inventory;
    double mm_pnl;
  };
  struct QuoteOrder {
    lob::OrderId id;
    lob::Side side;
    lob::Price price;
    lob::Qty remaining;
  };

  Role role_of_owner(lob::OwnerId owner) const noexcept;
  void advance(TimeNs now);
  void ensure_started();
  void take_sample(TimeNs now);
  double pnl_now() const;
  void record_fill(TimeNs now, const lob::Trade& t, lob::Side side, bool maker);
  void reduce_quote(lob::OrderId id, lob::Qty qty);

  MetricsOptions opt_;
  bool started_{false};
  TimeNs last_time_{0};

  // Current ground truth.
  BBO top_{};
  std::optional<double> mid_;       // defined only while both sides exist
  std::optional<double> last_mid_;  // last defined mid
  double fundamental_{0};

  // Market-maker position.
  std::int64_t cash_{0};
  std::int64_t inventory_{0};
  double fees_{0};
  std::vector<QuoteOrder> quotes_;
  std::optional<lob::NewOrder> pending_new_;
  std::optional<double> mid_before_request_;
  bool quoting_both_{false};
  lob::Price quoted_spread_{0};

  // Window bookkeeping.
  bool warm_captured_{false};
  double pnl_at_warmup_{0};
  std::int64_t window_max_inventory_{0};
  std::int64_t total_max_inventory_{0};
  stats::Weighted spread_time_;         // market spread (ticks) -> ns, while two-sided, in the window
  stats::Weighted quoted_spread_time_;  // market maker's quoted spread -> ns, while quoting both sides, in the window

  std::vector<TradeRow> trades_;
  std::vector<Fill> fills_;
  std::vector<std::vector<std::size_t>> pending_;  // by horizon: fill indices awaiting resolution
  std::vector<std::size_t> pending_head_;
  std::vector<BboRow> bbo_rows_;
  stats::Series sampled_mids_;  // samples inside the window
  RunCounts counts_;
  double final_pnl_{0};
  double final_inventory_value_{0};
};

}  // namespace sim
