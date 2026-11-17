#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "lob/events.hpp"
#include "lob/types.hpp"
#include "sim/agent.hpp"
#include "sim/messages.hpp"

namespace sim {

// The quoting parameters of the Avellaneda-Stoikov market maker (SPEC 1, 5.6 item 3).
struct MmParams {
  double gamma{0.0001};  // risk aversion
  double kappa{1.5};     // order-book liquidity
  double sigma{1.0};     // volatility, ticks per sqrt(second)
  lob::Qty quote_size{5};
  lob::Qty q_max{50};
  lob::Price max_price{1'000'000};
};

struct Quotes {
  std::optional<lob::Price> bid, ask;
  bool operator==(const Quotes&) const = default;
};

// r = s - q * gamma * sigma^2 * tau (ticks), tau in seconds.
double reservation_price(const MmParams& p, double mid, lob::Qty inventory, double tau_s) noexcept;
// delta = gamma * sigma^2 * tau + (2 / gamma) * ln(1 + gamma / kappa) (ticks).
double total_spread(const MmParams& p, double tau_s) noexcept;

// The quotes for one moment, with no agent state (SPEC 5.6 item 3):
//   - no quotes unless the local BBO is two-sided;
//   - s = (bid + ask) / 2, tau = (end - now) in seconds (never negative);
//   - bid = floor(r - delta / 2), ask = ceil(r + delta / 2);
//   - clamp bid <= best ask - 1 and ask >= best bid + 1, then make sure bid < ask (by
//     lowering the bid; it cannot be needed while delta > 0, it guards rounding only);
//   - a side outside [1, max_price] is not quoted;
//   - no bid if inventory >= q_max, no ask if inventory <= -q_max.
Quotes compute_quotes(const MmParams& p, const BBO& local_bbo, lob::Qty inventory, TimeNs now, TimeNs end);

// Avellaneda-Stoikov market maker: one post-only GTC bid and one ask of quote_size lots.
//
// State per side: the working order it believes is resting (id, price, remaining), or
// none; and at most one pending request (New, Modify or Cancel), or none. A new request
// for a side is never sent while that side has a pending request. Every request gets
// exactly one reply, and only that reply clears the pending request:
//   New    -> Ack (by client_order_id)  | Reject(New, client_order_id)
//   Modify -> Modified(order_id)        | Reject(Modify, order_id)
//   Cancel -> Cancelled(order_id, User) | Reject(Cancel, order_id)
// Trades are not replies: they change the inventory and the working order's remaining
// quantity, and never clear a pending request. All private events for one owner arrive in
// exchange order, so a fill that beat a modify to the exchange arrives before the reply.
//
// requote() runs after every BBO update, every requote_interval wakeup and every private
// message, and acts on each side independently (see the .cpp for the rules, including
// why a reprice near the inventory limit is Cancel then New instead of Modify).
//
// No randomness: ctx.rng() is never drawn.
class AvellanedaStoikovMM : public Agent {
 public:
  struct WorkingOrder {
    lob::OrderId id{0};
    lob::Price price{0};
    lob::Qty remaining{0};
    bool operator==(const WorkingOrder&) const = default;
  };
  enum class PendingKind : std::uint8_t { None, New, Modify, Cancel };
  struct Pending {
    PendingKind kind{PendingKind::None};
    std::uint64_t client_order_id{0};  // New
    lob::OrderId order_id{0};          // Modify, Cancel
    lob::Price price{0};               // New, Modify: the price requested
    lob::Qty qty{0};                   // New, Modify: the quantity requested
    bool operator==(const Pending&) const = default;
  };

  // `requote_interval` > 0; `end` is the session end T used for tau.
  AvellanedaStoikovMM(const MmParams& params, TimeNs requote_interval, TimeNs end);

  void on_start(Context& ctx) override;
  void on_wakeup(Context& ctx) override;
  void on_market_data(Context& ctx, const MarketData& msg) override;
  void on_private(Context& ctx, const lob::Event& event) override;

  // Read-only views for tests and metrics. Everything here is the agent's belief.
  lob::Qty inventory() const noexcept { return q_; }
  const std::optional<WorkingOrder>& working(lob::Side side) const noexcept { return sides_[idx(side)].working; }
  const Pending& pending(lob::Side side) const noexcept { return sides_[idx(side)].pending; }
  PendingKind pending_kind(lob::Side side) const noexcept { return sides_[idx(side)].pending.kind; }
  const BBO& local_bbo() const noexcept { return bbo_; }
  const MmParams& params() const noexcept { return p_; }
  std::uint64_t news_sent() const noexcept { return news_sent_; }
  std::uint64_t modifies_sent() const noexcept { return modifies_sent_; }
  std::uint64_t cancels_sent() const noexcept { return cancels_sent_; }
  std::uint64_t rejects(lob::RejectReason reason) const noexcept { return rejects_[static_cast<std::size_t>(reason)]; }
  std::uint64_t total_rejects() const noexcept;
  // Messages that fit no pending request or known order. Stays 0 in a correct system.
  std::uint64_t anomalies() const noexcept { return anomalies_; }

 private:
  struct SideState {
    std::optional<WorkingOrder> working;
    Pending pending;
  };
  static constexpr std::size_t idx(lob::Side s) noexcept { return s == lob::Side::Buy ? 0 : 1; }

  void requote(Context& ctx);
  void requote_side(Context& ctx, lob::Side side, std::optional<lob::Price> desired);
  void apply(Context& ctx, const lob::Event& event);
  void on_ack(const lob::Ack& e);
  void on_reject(const lob::Reject& e);
  void on_modified(const lob::Modified& e);
  void on_cancelled(const lob::Cancelled& e);
  void on_fill(lob::Side side, lob::OrderId id, lob::Qty qty);
  SideState* side_pending(PendingKind kind, lob::OrderId order_id);

  MmParams p_;
  TimeNs requote_interval_;
  TimeNs end_;
  BBO bbo_{};
  lob::Qty q_{0};
  std::array<SideState, 2> sides_{};  // [0] bid, [1] ask
  std::uint64_t next_client_id_{1};
  std::uint64_t news_sent_{0}, modifies_sent_{0}, cancels_sent_{0};
  std::array<std::uint64_t, 7> rejects_{};  // by lob::RejectReason
  std::uint64_t anomalies_{0};
};

}  // namespace sim
