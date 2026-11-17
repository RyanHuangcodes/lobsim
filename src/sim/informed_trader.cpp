#include "sim/agents/informed_trader.hpp"

namespace sim {

InformedTrader::InformedTrader(const InformedTraderParams& params, const Fundamental& fundamental)
    : p_(params), fundamental_(fundamental) {}

void InformedTrader::on_start(Context& ctx) {
  ctx.schedule_wakeup(ctx.now() + seconds_to_ns(ctx.rng().exponential(p_.lambda)));
}

void InformedTrader::on_wakeup(Context& ctx) {
  // Draw and schedule first: the wakeup schedule must not depend on the market.
  ctx.schedule_wakeup(ctx.now() + seconds_to_ns(ctx.rng().exponential(p_.lambda)));

  const double v = fundamental_.value();
  lob::Side side;
  if (bbo_.has_ask() && v - static_cast<double>(bbo_.ask) >= p_.theta_ticks) side = lob::Side::Buy;
  else if (bbo_.has_bid() && static_cast<double>(bbo_.bid) - v >= p_.theta_ticks) side = lob::Side::Sell;
  else return;

  ctx.send(lob::NewOrder{ctx.self(), next_client_id_++, side, lob::OrderType::Market, lob::TIF::IOC, 0, p_.size,
                         false, lob::STP::CancelTaking});
  if (side == lob::Side::Buy) ++buys_sent_;
  else ++sells_sent_;
}

void InformedTrader::on_market_data(Context&, const MarketData& msg) {
  if (const auto* bbo = std::get_if<BBO>(&msg)) bbo_ = *bbo;  // trade prints are ignored
}

void InformedTrader::on_private(Context&, const lob::Event&) { ++private_events_; }

}  // namespace sim
