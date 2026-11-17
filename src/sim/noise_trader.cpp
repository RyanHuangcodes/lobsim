#include "sim/agents/noise_trader.hpp"

#include <algorithm>
#include <type_traits>

namespace sim {

NoiseTrader::NoiseTrader(const NoiseTraderParams& params, Xoshiro256StarStar lifetime_rng)
    : p_(params), lifetime_rng_(lifetime_rng) {}

void NoiseTrader::on_start(Context& ctx) {
  timers_.schedule(ctx, ctx.now() + seconds_to_ns(ctx.rng().exponential(p_.lambda)), TradeTimer{});
}

void NoiseTrader::on_wakeup(Context& ctx) {
  const Timer timer = timers_.pop_due(ctx.now());
  if (std::holds_alternative<TradeTimer>(timer)) {
    trade(ctx);
    return;
  }
  // Cancel timer: the order's lifetime is over. Send the cancel only if the order is
  // still open by this trader's own records; if a fill is in flight the exchange answers
  // UnknownOrder, which on_private() ignores.
  const lob::OrderId id = std::get<CancelTimer>(timer).order_id;
  if (open_.find(id) == open_.end()) return;
  ctx.send(lob::Cancel{ctx.self(), id});
  ++cancels_sent_;
}

// Integer mid of the local BBO (floor), else the last trade seen, else the initial price.
lob::Price NoiseTrader::reference_price() const {
  if (bbo_.two_sided()) return (bbo_.bid + bbo_.ask) / 2;
  if (last_trade_) return *last_trade_;
  return p_.initial_price;
}

lob::Price NoiseTrader::buy_reference_price() const {
  if (p_.reference_price == ReferencePrice::Symmetric && bbo_.two_sided()) return (bbo_.bid + bbo_.ask + 1) / 2;
  return reference_price();
}

void NoiseTrader::trade(Context& ctx) {
  Xoshiro256StarStar& rng = ctx.rng();
  const bool is_limit = rng.uniform01() < p_.p_limit;
  const lob::Side side = rng.uniform_int(0, 1) == 0 ? lob::Side::Buy : lob::Side::Sell;
  const lob::Price d = is_limit ? rng.uniform_int(1, p_.D) : 0;
  const lob::Qty size = rng.uniform_int(1, p_.max_size);
  const double next_s = rng.exponential(p_.lambda);
  timers_.schedule(ctx, ctx.now() + seconds_to_ns(next_s), TradeTimer{});

  const std::uint64_t client_id = next_client_id_++;
  if (!is_limit) {
    ctx.send(lob::NewOrder{ctx.self(), client_id, side, lob::OrderType::Market, lob::TIF::IOC, 0, size, false,
                           lob::STP::CancelTaking});
    ++markets_sent_;
    return;
  }
  const lob::Price price = std::clamp<lob::Price>(
      side == lob::Side::Buy ? buy_reference_price() - d : reference_price() + d, 1, p_.max_price);
  awaiting_ack_[client_id] = size;
  ctx.send(lob::NewOrder{ctx.self(), client_id, side, lob::OrderType::Limit, lob::TIF::GTC, price, size, false,
                         lob::STP::CancelTaking});
  ++limits_sent_;
}

void NoiseTrader::on_market_data(Context&, const MarketData& msg) {
  if (const auto* bbo = std::get_if<BBO>(&msg)) bbo_ = *bbo;
  else last_trade_ = std::get<TradePrint>(msg).price;
}

void NoiseTrader::reduce(lob::OrderId id, lob::Qty qty) {
  const auto it = open_.find(id);
  if (it == open_.end()) return;  // a market order, or an order already finished
  it->second -= qty;
  if (it->second <= 0) open_.erase(it);
}

void NoiseTrader::on_private(Context& ctx, const lob::Event& event) {
  std::visit(
      [&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, lob::Ack>) {
          const auto it = awaiting_ack_.find(e.client_order_id);
          if (it == awaiting_ack_.end()) return;  // the Ack of a market order: nothing rests
          // A limit order was accepted. It is treated as resting from now on (any fills it
          // got on arrival are reported by the Trade events that follow and reduce it), and
          // its lifetime is drawn now from the lifetime stream.
          open_[e.order_id] = it->second;
          awaiting_ack_.erase(it);
          const double lifetime_s = lifetime_rng_.exponential(1.0 / p_.order_lifetime_mean_s);
          timers_.schedule(ctx, ctx.now() + seconds_to_ns(lifetime_s), CancelTimer{e.order_id});
        } else if constexpr (std::is_same_v<T, lob::Trade>) {
          if (e.maker_owner == ctx.self()) reduce(e.maker_order_id, e.qty);
          if (e.taker_owner == ctx.self()) reduce(e.taker_order_id, e.qty);
        } else if constexpr (std::is_same_v<T, lob::Cancelled>) {
          open_.erase(e.order_id);
        } else if constexpr (std::is_same_v<T, lob::Reject>) {
          if (e.request_kind == lob::RequestKind::New) awaiting_ack_.erase(e.client_order_id);
          else open_.erase(e.order_id);  // UnknownOrder: it was filled while the cancel travelled
          ++ignored_rejects_;
        }
      },
      event);
}

}  // namespace sim
