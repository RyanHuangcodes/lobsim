#include "sim/agents/calibrated_flow.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace sim {

CumulativeTable::CumulativeTable(const calib::Pmf& pmf) {
  if (pmf.values.size() != pmf.counts.size()) throw std::invalid_argument("CumulativeTable: values and counts differ");
  values_ = pmf.values;
  cum_.reserve(pmf.counts.size());
  std::uint64_t sum = 0;
  for (std::uint64_t c : pmf.counts) {
    if (c == 0 || sum > ~std::uint64_t{0} - c) throw std::invalid_argument("CumulativeTable: bad counts");
    sum += c;
    cum_.push_back(sum);
  }
}

std::int64_t CumulativeTable::value_at(std::uint64_t r) const {
  if (r >= total()) throw std::out_of_range("CumulativeTable: draw beyond the total");
  // The first entry whose cumulative count is above r.
  const auto it = std::upper_bound(cum_.begin(), cum_.end(), r);
  return values_[static_cast<std::size_t>(it - cum_.begin())];
}

std::int64_t CumulativeTable::sample(Xoshiro256StarStar& rng) const {
  if (empty()) throw std::logic_error("CumulativeTable: sampling an empty table");
  // uniform_int takes int64 bounds; totals above INT64_MAX are handled by drawing the offset
  // as a signed value from the full range and shifting it back.
  const std::uint64_t n = total();
  if (n - 1 <= static_cast<std::uint64_t>(INT64_MAX)) return value_at(static_cast<std::uint64_t>(rng.uniform_int(0, static_cast<std::int64_t>(n - 1))));
  // Rare: total above 2^63. Rejection on the raw 64-bit output keeps it exact.
  const std::uint64_t threshold = (0 - n) % n;
  for (;;) {
    const std::uint64_t r = rng.next_u64();
    if (r >= threshold) return value_at(r % n);
  }
}

std::optional<TimeNs> lifetime_from_survival(const calib::Survival& curve, double u) {
  // survival is non-increasing, so the first index with survival[i] <= u is a partition point.
  const auto& s = curve.survival;
  const auto it = std::partition_point(s.begin(), s.end(), [u](double v) { return v > u; });
  if (it == s.end()) return std::nullopt;
  return curve.times_ns[static_cast<std::size_t>(it - s.begin())];
}

CalibratedFlowAgent::CalibratedFlowAgent(const CalibratedFlowParams& params, Xoshiro256StarStar lifetime_rng)
    : p_(params), lifetime_rng_(lifetime_rng) {
  if (!p_.calibration) throw std::invalid_argument("CalibratedFlowAgent: no calibration");
  const calib::Calibration& c = *p_.calibration;
  const double lb = c.rates.limit[calib::kBuy].per_s, ls = c.rates.limit[calib::kSell].per_s;
  const double mb = c.rates.aggressive[calib::kBuy].per_s, ms = c.rates.aggressive[calib::kSell].per_s;
  lambda_limit_ = lb + ls;
  lambda_market_ = mb + ms;
  if (lambda_limit_ > 0.0) p_limit_buy_ = lb / lambda_limit_;
  if (lambda_market_ > 0.0) p_market_buy_ = mb / lambda_market_;
  if (lambda_limit_ > 0.0) {
    if (c.limit_size.empty()) throw std::invalid_argument("CalibratedFlowAgent: limit_size is empty");
    limit_size_ = CumulativeTable(c.limit_size);
    for (int s = 0; s < 2; ++s) {
      if (c.rates.limit[s].per_s <= 0.0) continue;
      for (int b = 0; b < calib::kSpreadBuckets; ++b) {
        if (!c.placement[s][b].empty()) placement_[s][b] = CumulativeTable(c.placement[s][b]);
      }
      // A bucket with no samples draws from the nearest non-empty one of the same side (D-385).
      static constexpr int kOrder[calib::kSpreadBuckets][calib::kSpreadBuckets] = {{0, 1, 2}, {1, 0, 2}, {2, 1, 0}};
      for (int b = 0; b < calib::kSpreadBuckets; ++b) {
        placement_source_[s][b] = -1;
        for (int k = 0; k < calib::kSpreadBuckets && placement_source_[s][b] < 0; ++k)
          if (!placement_[s][kOrder[b][k]].empty()) placement_source_[s][b] = kOrder[b][k];
        if (placement_source_[s][b] < 0)
          throw std::invalid_argument("CalibratedFlowAgent: all three placement PMFs of one side are empty");
      }
    }
  }
  if (lambda_market_ > 0.0) {
    if (c.aggressive_size.empty()) throw std::invalid_argument("CalibratedFlowAgent: aggressive_size is empty");
    aggressive_size_ = CumulativeTable(c.aggressive_size);
  }
}

void CalibratedFlowAgent::on_start(Context& ctx) {
  if (lambda_limit_ > 0.0)
    timers_.schedule(ctx, ctx.now() + seconds_to_ns(ctx.rng().exponential(lambda_limit_)), LimitClock{});
  if (lambda_market_ > 0.0)
    timers_.schedule(ctx, ctx.now() + seconds_to_ns(ctx.rng().exponential(lambda_market_)), MarketClock{});
}

void CalibratedFlowAgent::on_wakeup(Context& ctx) {
  const Timer timer = timers_.pop_due(ctx.now());
  if (std::holds_alternative<LimitClock>(timer)) {
    send_limit(ctx);
  } else if (std::holds_alternative<MarketClock>(timer)) {
    send_market(ctx);
  } else {
    const CancelTimer& t = std::get<CancelTimer>(timer);
    // Cancel only if the order is still open by this agent's own records; if a fill is in
    // flight the exchange answers UnknownOrder, which on_private() ignores. The cancel carries
    // the owner id that placed the order.
    if (open_.find(t.order_id) == open_.end()) return;
    ctx.send(lob::Cancel{t.owner, t.order_id});
    ++counters_.cancels_sent;
  }
}

lob::OwnerId CalibratedFlowAgent::next_owner(Context& ctx) {
  return ctx.owner_id(static_cast<std::uint32_t>(next_owner_++ % ctx.owner_count()));
}

void CalibratedFlowAgent::send_limit(Context& ctx) {
  Xoshiro256StarStar& rng = ctx.rng();
  const int side_idx = rng.uniform01() < p_limit_buy_ ? calib::kBuy : calib::kSell;
  const lob::Side side = side_idx == calib::kBuy ? lob::Side::Buy : lob::Side::Sell;

  // Reference price, spread bucket and the lowest allowed Delta, from the local view.
  lob::Price ref = 0;
  int bucket = 0;
  std::optional<lob::Price> bound;  // Delta must be >= this; none: nothing can be crossed
  const bool own_present = side == lob::Side::Buy ? bbo_.has_bid() : bbo_.has_ask();
  if (bbo_.two_sided()) {
    const lob::Price s = bbo_.ask - bbo_.bid;
    bucket = calib::spread_bucket(s);
    ref = side == lob::Side::Buy ? bbo_.bid : bbo_.ask;
    bound = -(s - 1);
  } else if (own_present) {
    ++counters_.fallback_one_sided;
    bucket = calib::kSpreadBuckets - 1;
    ref = side == lob::Side::Buy ? bbo_.bid : bbo_.ask;
  } else if (bbo_.has_bid() || bbo_.has_ask()) {
    ++counters_.fallback_one_sided;
    ref = side == lob::Side::Buy ? bbo_.ask - 1 : bbo_.bid + 1;
    bucket = 0;
    bound = 0;
  } else {
    ++counters_.fallback_empty;
    ref = p_.start_price;
    bucket = 0;
    bound = 0;
  }

  const int source = placement_source_[side_idx][bucket];
  if (source != bucket) ++counters_.placement_bucket_fallback;
  if (source < bucket && bbo_.two_sided()) {
    // The PMF was observed at a narrower spread than this one (source + 1 ticks), where its Delta was measured from the
    // own best: here that is the opposite best -+ that spread. Measured from the own best instead, an order of bucket
    // "1" (every Delta >= 0) could never narrow a spread that has widened (D-399).
    const lob::Price observed = source + 1;
    ref = side == lob::Side::Buy ? bbo_.ask - observed : bbo_.bid + observed;
    bound = -(observed - 1);
  }
  const CumulativeTable& table = placement_[side_idx][source];
  std::int64_t delta = table.sample(rng);
  if (bound && delta < *bound) {
    int tries = 0;
    while (delta < *bound && tries < 100) {
      delta = table.sample(rng);
      ++tries;
      ++counters_.placement_redraws;
    }
    if (delta < *bound) {
      delta = *bound;
      ++counters_.placement_clamped;
    }
  }
  const lob::Qty size = limit_size_.sample(rng);
  const double gap_s = rng.exponential(lambda_limit_);
  timers_.schedule(ctx, ctx.now() + seconds_to_ns(gap_s), LimitClock{});

  // The price is clamped to [1, max_price] below, so a huge Delta is limited first (no overflow).
  const std::int64_t reach = 2 * p_.max_price;
  const std::int64_t d = std::clamp<std::int64_t>(delta, -reach, reach);
  lob::Price price = side == lob::Side::Buy ? ref - d : ref + d;
  const lob::Price clamped = std::clamp<lob::Price>(price, 1, p_.max_price);
  if (clamped != price) ++counters_.price_clamped;
  price = clamped;

  const std::uint64_t client_id = next_client_id_++;
  const lob::OwnerId owner = next_owner(ctx);
  awaiting_ack_[client_id] = Pending{size, calib::distance_bucket(delta), owner};
  ctx.send(lob::NewOrder{owner, client_id, side, lob::OrderType::Limit, lob::TIF::GTC, price, size, false,
                         lob::STP::CancelTaking});
  ++counters_.limits_sent;
}

void CalibratedFlowAgent::send_market(Context& ctx) {
  Xoshiro256StarStar& rng = ctx.rng();
  const lob::Side side = rng.uniform01() < p_market_buy_ ? lob::Side::Buy : lob::Side::Sell;
  const lob::Qty size = aggressive_size_.sample(rng);
  const double gap_s = rng.exponential(lambda_market_);
  timers_.schedule(ctx, ctx.now() + seconds_to_ns(gap_s), MarketClock{});

  const std::uint64_t client_id = next_client_id_++;
  ctx.send(lob::NewOrder{next_owner(ctx), client_id, side, lob::OrderType::Market, lob::TIF::IOC, 0, size, false,
                         lob::STP::CancelTaking});
  ++counters_.markets_sent;
}

void CalibratedFlowAgent::on_market_data(Context&, const MarketData& msg) {
  if (const auto* bbo = std::get_if<BBO>(&msg)) bbo_ = *bbo;
}

void CalibratedFlowAgent::reduce(lob::OrderId id, lob::Qty qty) {
  const auto it = open_.find(id);
  if (it == open_.end()) return;  // a market order, or an order already finished
  it->second.qty -= qty;
  if (it->second.qty <= 0) open_.erase(it);
}

void CalibratedFlowAgent::on_private(Context& ctx, const lob::Event& event) {
  std::visit(
      [&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, lob::Ack>) {
          const auto it = awaiting_ack_.find(e.client_order_id);
          if (it == awaiting_ack_.end()) return;  // the Ack of a market order: nothing rests
          const Pending pending = it->second;
          awaiting_ack_.erase(it);
          open_[e.order_id] = Open{pending.qty, pending.owner};
          // The lifetime is drawn now, from the lifetime stream: one draw per limit order.
          const double u = lifetime_rng_.uniform01();
          const auto life = lifetime_from_survival(p_.calibration->lifetime[pending.bucket], u);
          if (!life) {
            ++counters_.no_cancel_draws;
            return;
          }
          ++counters_.cancels_scheduled;
          timers_.schedule(ctx, ctx.now() + std::min(*life, kFarFutureNs), CancelTimer{e.order_id, pending.owner});
        } else if constexpr (std::is_same_v<T, lob::Trade>) {
          if (ctx.owns(e.maker_owner)) reduce(e.maker_order_id, e.qty);
          if (ctx.owns(e.taker_owner)) reduce(e.taker_order_id, e.qty);
        } else if constexpr (std::is_same_v<T, lob::Cancelled>) {
          open_.erase(e.order_id);
        } else if constexpr (std::is_same_v<T, lob::Reject>) {
          if (e.request_kind == lob::RequestKind::New) awaiting_ack_.erase(e.client_order_id);
          else open_.erase(e.order_id);  // UnknownOrder: it was filled while the cancel travelled
          ++counters_.ignored_rejects;
        }
      },
      event);
}

}  // namespace sim
