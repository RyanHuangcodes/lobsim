#include "sim/agents/as_market_maker.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <variant>

namespace sim {

double reservation_price(const MmParams& p, double mid, lob::Qty inventory, double tau_s) noexcept {
  return mid - static_cast<double>(inventory) * p.gamma * p.sigma * p.sigma * tau_s;
}

double total_spread(const MmParams& p, double tau_s) noexcept {
  return p.gamma * p.sigma * p.sigma * tau_s + (2.0 / p.gamma) * std::log(1.0 + p.gamma / p.kappa);
}

Quotes compute_quotes(const MmParams& p, const BBO& b, lob::Qty inventory, TimeNs now, TimeNs end) {
  Quotes out;
  if (!b.two_sided()) return out;
  const double s = static_cast<double>(b.bid + b.ask) / 2.0;
  const double tau = now < end ? static_cast<double>(end - now) / 1e9 : 0.0;
  const double r = reservation_price(p, s, inventory, tau);
  const double half = total_spread(p, tau) / 2.0;

  // Worked in doubles until the range check, so that an extreme r can never overflow the
  // conversion to an integer price.
  double bid = std::min(std::floor(r - half), static_cast<double>(b.ask - 1));
  const double ask = std::max(std::ceil(r + half), static_cast<double>(b.bid + 1));
  if (!(bid < ask)) bid = ask - 1.0;

  const double max_price = static_cast<double>(p.max_price);
  if (inventory < p.q_max && bid >= 1.0 && bid <= max_price) out.bid = static_cast<lob::Price>(bid);
  if (inventory > -p.q_max && ask >= 1.0 && ask <= max_price) out.ask = static_cast<lob::Price>(ask);
  return out;
}

AvellanedaStoikovMM::AvellanedaStoikovMM(const MmParams& params, TimeNs requote_interval, TimeNs end)
    : p_(params), requote_interval_(requote_interval), end_(end) {}

std::uint64_t AvellanedaStoikovMM::total_rejects() const noexcept {
  std::uint64_t n = 0;
  for (const std::uint64_t r : rejects_) n += r;
  return n;
}

void AvellanedaStoikovMM::on_start(Context& ctx) { ctx.schedule_wakeup(ctx.now() + requote_interval_); }

void AvellanedaStoikovMM::on_wakeup(Context& ctx) {
  ctx.schedule_wakeup(ctx.now() + requote_interval_);
  requote(ctx);
}

void AvellanedaStoikovMM::on_market_data(Context& ctx, const MarketData& msg) {
  const auto* bbo = std::get_if<BBO>(&msg);
  if (bbo == nullptr) return;  // trade prints carry nothing the quotes use
  bbo_ = *bbo;
  requote(ctx);
}

void AvellanedaStoikovMM::on_private(Context& ctx, const lob::Event& event) {
  apply(ctx, event);
  // Act at once on a cleared pending request or a changed inventory.
  requote(ctx);
}

void AvellanedaStoikovMM::requote(Context& ctx) {
  const Quotes quotes = compute_quotes(p_, bbo_, q_, ctx.now(), end_);
  requote_side(ctx, lob::Side::Buy, quotes.bid);
  requote_side(ctx, lob::Side::Sell, quotes.ask);
}

void AvellanedaStoikovMM::requote_side(Context& ctx, lob::Side side, std::optional<lob::Price> desired) {
  SideState& st = sides_[idx(side)];
  if (st.pending.kind != PendingKind::None) return;  // never two requests in flight on one side

  if (!desired) {
    if (st.working) {
      ctx.send(lob::Cancel{ctx.self(), st.working->id});
      st.pending = Pending{PendingKind::Cancel, 0, st.working->id, 0, 0};
      ++cancels_sent_;
    }
    return;
  }
  if (!st.working) {
    const std::uint64_t cid = next_client_id_++;
    ctx.send(lob::NewOrder{ctx.self(), cid, side, lob::OrderType::Limit, lob::TIF::GTC, *desired, p_.quote_size,
                           true, lob::STP::CancelTaking});
    st.pending = Pending{PendingKind::New, cid, 0, *desired, p_.quote_size};
    ++news_sent_;
    return;
  }
  if (st.working->price == *desired) return;  // also when partially filled

  // Reprice. A Modify sets the order's remaining quantity to quote_size, and it may reach
  // the exchange after fills this agent has not seen yet; the order is then refilled.
  // Let e be the believed exposure on this side (q for the bid, -q for the ask) when the
  // Modify is sent, and R <= quote_size the believed remaining quantity. Every earlier
  // order on this side is gone and all its fills have been seen (a side gets a new order
  // only after the old one's last message), so the true exposure can grow by at most:
  //   R - 1 lots of unseen fills before the Modify arrives (if all R fill, the order is
  //   gone and the Modify is rejected with UnknownOrder: nothing is refilled), plus
  //   quote_size lots from the refilled order,
  // i.e. it can reach e + R - 1 + quote_size <= e + 2 * quote_size - 1. Modify is used
  // only when e + quote_size <= q_max, which bounds this by q_max + quote_size - 1.
  // Otherwise the reprice is Cancel now and, once the cancel's reply has arrived, an
  // ordinary NewOrder from the next requote: a NewOrder is sent only when e < q_max
  // (compute_quotes), with no order resting on that side and every fill seen, so it too
  // gives at most q_max - 1 + quote_size. (SPEC 9 item 6 asks for q_max + quote_size.)
  const lob::Qty exposure = side == lob::Side::Buy ? q_ : -q_;
  if (exposure + p_.quote_size <= p_.q_max) {
    ctx.send(lob::Modify{ctx.self(), st.working->id, *desired, p_.quote_size});
    st.pending = Pending{PendingKind::Modify, 0, st.working->id, *desired, p_.quote_size};
    ++modifies_sent_;
  } else {
    ctx.send(lob::Cancel{ctx.self(), st.working->id});
    st.pending = Pending{PendingKind::Cancel, 0, st.working->id, 0, 0};
    ++cancels_sent_;
  }
}

AvellanedaStoikovMM::SideState* AvellanedaStoikovMM::side_pending(PendingKind kind, lob::OrderId order_id) {
  for (SideState& st : sides_)
    if (st.pending.kind == kind && st.pending.order_id == order_id) return &st;
  return nullptr;
}

void AvellanedaStoikovMM::apply(Context& ctx, const lob::Event& event) {
  std::visit(
      [&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, lob::Ack>) {
          on_ack(e);
        } else if constexpr (std::is_same_v<T, lob::Reject>) {
          on_reject(e);
        } else if constexpr (std::is_same_v<T, lob::Modified>) {
          on_modified(e);
        } else if constexpr (std::is_same_v<T, lob::Cancelled>) {
          on_cancelled(e);
        } else {  // lob::Trade: either role, though post-only orders are always the maker
          if (e.maker_owner == ctx.self()) on_fill(lob::opposite(e.aggressor_side), e.maker_order_id, e.qty);
          if (e.taker_owner == ctx.self()) on_fill(e.aggressor_side, e.taker_order_id, e.qty);
        }
      },
      event);
}

void AvellanedaStoikovMM::on_ack(const lob::Ack& e) {
  for (SideState& st : sides_) {
    if (st.pending.kind != PendingKind::New || st.pending.client_order_id != e.client_order_id) continue;
    if (st.working) ++anomalies_;  // a NewOrder is only sent with no working order
    st.working = WorkingOrder{e.order_id, st.pending.price, st.pending.qty};
    st.pending = Pending{};
    return;
  }
  ++anomalies_;
}

void AvellanedaStoikovMM::on_reject(const lob::Reject& e) {
  SideState* st = nullptr;
  if (e.request_kind == lob::RequestKind::New) {
    for (SideState& s : sides_)
      if (s.pending.kind == PendingKind::New && s.pending.client_order_id == e.client_order_id) st = &s;
  } else {
    st = side_pending(e.request_kind == lob::RequestKind::Modify ? PendingKind::Modify : PendingKind::Cancel,
                      e.order_id);
  }
  if (st == nullptr) {
    ++anomalies_;
    return;
  }
  ++rejects_[static_cast<std::size_t>(e.reason)];
  st->pending = Pending{};
  // A rejected New never rested, so there is still no working order (PostOnlyWouldCross
  // means the local view was stale; the next requote uses the BBO it has by then). A
  // Modify or Cancel rejected with UnknownOrder found the order gone: it was filled before
  // the request arrived, and those fills were delivered first and have already removed
  // the working order. If it is somehow still here, the belief was wrong: drop it and
  // count the anomaly. Any other reject (PostOnlyWouldCross on a Modify, for one) leaves
  // the order resting exactly as it was.
  if (e.request_kind != lob::RequestKind::New && e.reason == lob::RejectReason::UnknownOrder && st->working) {
    ++anomalies_;
    st->working.reset();
  }
}

void AvellanedaStoikovMM::on_modified(const lob::Modified& e) {
  SideState* st = side_pending(PendingKind::Modify, e.order_id);
  if (st == nullptr) {
    ++anomalies_;
    return;
  }
  st->pending = Pending{};
  if (!st->working || st->working->id != e.order_id) ++anomalies_;  // believed gone: trust the exchange
  // The exchange set the remaining quantity to new_qty, whatever filled before.
  st->working = WorkingOrder{e.order_id, e.new_price, e.new_qty};
}

void AvellanedaStoikovMM::on_cancelled(const lob::Cancelled& e) {
  SideState* st = e.reason == lob::CancelReason::User ? side_pending(PendingKind::Cancel, e.order_id) : nullptr;
  if (st != nullptr) {
    st->pending = Pending{};
    if (!st->working || st->working->id != e.order_id) ++anomalies_;
    st->working.reset();
    return;
  }
  // Not the reply to a pending cancel (a post-only maker is never cancelled by
  // self-trade prevention). The order is gone whatever the reason.
  ++anomalies_;
  for (SideState& s : sides_)
    if (s.working && s.working->id == e.order_id) s.working.reset();
}

void AvellanedaStoikovMM::on_fill(lob::Side side, lob::OrderId id, lob::Qty qty) {
  q_ += side == lob::Side::Buy ? qty : -qty;  // the inventory follows every fill, known order or not
  SideState& st = sides_[idx(side)];
  if (!st.working || st.working->id != id) {
    ++anomalies_;
    return;
  }
  st.working->remaining -= qty;
  if (st.working->remaining < 0) ++anomalies_;
  if (st.working->remaining <= 0) st.working.reset();
}

}  // namespace sim
