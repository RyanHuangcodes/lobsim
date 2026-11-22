#include "sim/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ostream>
#include <variant>

#include <nlohmann/json.hpp>

namespace sim {

// ---------------------------------------------------------------------------------------
// stats
// ---------------------------------------------------------------------------------------
namespace stats {

std::optional<double> mean(std::span<const double> x) {
  if (x.empty()) return std::nullopt;
  double s = 0;
  for (double v : x) s += v;
  return s / static_cast<double>(x.size());
}

std::optional<double> sample_sd(std::span<const double> x) {
  if (x.size() < 2) return std::nullopt;
  const double m = *mean(x);
  double s = 0;
  for (double v : x) s += (v - m) * (v - m);
  return std::sqrt(s / static_cast<double>(x.size() - 1));
}

std::optional<double> excess_kurtosis(std::span<const double> x) {
  if (x.size() < 4) return std::nullopt;
  const double n = static_cast<double>(x.size());
  const double m = *mean(x);
  double m2 = 0, m4 = 0;
  for (double v : x) {
    const double d = v - m;
    m2 += d * d;
    m4 += d * d * d * d;
  }
  m2 /= n;
  m4 /= n;
  if (!(m2 > 0)) return std::nullopt;
  return m4 / (m2 * m2) - 3.0;
}

std::optional<double> autocorrelation(const Series& x, std::size_t lag) {
  double sum = 0;
  std::size_t count = 0;
  for (const auto& v : x)
    if (v) {
      sum += *v;
      ++count;
    }
  if (count == 0) return std::nullopt;
  const double m = sum / static_cast<double>(count);
  double den = 0;
  for (const auto& v : x)
    if (v) den += (*v - m) * (*v - m);
  if (!(den > 0)) return std::nullopt;
  double num = 0;
  std::size_t pairs = 0;
  for (std::size_t t = 0; t + lag < x.size(); ++t) {
    if (x[t] && x[t + lag]) {
      num += (*x[t] - m) * (*x[t + lag] - m);
      ++pairs;
    }
  }
  if (pairs == 0) return std::nullopt;
  return num / den;
}

std::optional<Interval> confidence_interval_95(std::size_t n, double mean, double sd) {
  if (n < 2) return std::nullopt;
  const double half = 1.96 * sd / std::sqrt(static_cast<double>(n));
  return Interval{mean - half, mean + half};
}

double markout_per_lot(lob::Side side, lob::Price price, double mid_later) {
  const double p = static_cast<double>(price);
  return side == lob::Side::Buy ? mid_later - p : p - mid_later;
}

Series log_returns(const Series& mids) {
  Series r;
  if (mids.size() < 2) return r;
  r.reserve(mids.size() - 1);
  for (std::size_t k = 1; k < mids.size(); ++k) {
    if (mids[k] && mids[k - 1]) r.emplace_back(std::log(*mids[k] / *mids[k - 1]));
    else r.emplace_back(std::nullopt);
  }
  return r;
}

std::optional<double> weighted_mean(const Weighted& w) {
  long double sum = 0, total = 0;
  for (const auto& [v, wt] : w) {
    sum += static_cast<long double>(v) * static_cast<long double>(wt);
    total += static_cast<long double>(wt);
  }
  if (!(total > 0)) return std::nullopt;
  return static_cast<double>(sum / total);
}

std::optional<std::int64_t> weighted_quantile(const Weighted& w, double p) {
  long double total = 0;
  for (const auto& kv : w) total += static_cast<long double>(kv.second);
  if (!(total > 0)) return std::nullopt;
  const long double target = static_cast<long double>(p) * total;
  long double cum = 0;
  for (const auto& [v, wt] : w) {
    cum += static_cast<long double>(wt);
    if (cum >= target) return v;
  }
  return w.rbegin()->first;
}

}  // namespace stats

// ---------------------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------------------
MetricsOptions make_metrics_options(const SimConfig& cfg) {
  MetricsOptions o;
  o.warmup_ns = cfg.warmup_ns;
  o.end_ns = cfg.duration_ns;
  o.sample_interval_ns = cfg.sample_interval_ns;
  o.markout_horizons_ns = cfg.markout_horizons_ns;
  o.markout_horizons_s = cfg.markout_horizons_s;
  o.initial_fundamental = static_cast<double>(cfg.initial_price);
  for (std::uint32_t i = 0; i < cfg.noise_traders.count; ++i) o.roles.push_back(Role::Noise);
  if (cfg.informed) o.roles.push_back(Role::Informed);
  if (cfg.mm) {
    o.roles.push_back(Role::MarketMaker);
    o.mm_owner = static_cast<lob::OwnerId>(o.roles.size());  // owner id = agent index + 1
    o.maker_fee_ticks = cfg.mm->maker_fee_ticks;
    o.taker_fee_ticks = cfg.mm->taker_fee_ticks;
  }
  if (cfg.background) o.roles.push_back(Role::Noise);  // the calibrated flow is the background
  return o;
}

// ---------------------------------------------------------------------------------------
// observer
// ---------------------------------------------------------------------------------------
namespace {

constexpr std::size_t role_slot(Role r) { return static_cast<std::size_t>(r); }

std::string fmt(const char* format, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, format, v);
  return buf;
}

// What the fill earned against the pre-trade mid, per lot (mid_before must be defined).
double spread_capture(const auto& f) {
  const double p = static_cast<double>(f.price);
  return f.side == lob::Side::Buy ? *f.mid_before - p : p - *f.mid_before;
}

void append_ll(std::string& s, long long v) { s += std::to_string(v); }

}  // namespace

MetricsObserver::MetricsObserver(MetricsOptions options) : opt_(std::move(options)) {
  fundamental_ = opt_.initial_fundamental;
  const std::size_t h = opt_.markout_horizons_ns.size();
  pending_.assign(h, {});
  pending_head_.assign(h, 0);
}

Role MetricsObserver::role_of_owner(lob::OwnerId owner) const noexcept {
  if (owner >= kExtraOwnerBase) return Role::Noise;  // the extra owner ids of the background flow
  if (owner == 0 || owner > opt_.roles.size()) return Role::Other;
  return opt_.roles[owner - 1];
}

double MetricsObserver::pnl_now() const {
  return static_cast<double>(cash_) + static_cast<double>(inventory_) * last_mid_.value_or(0.0) - fees_;
}

void MetricsObserver::ensure_started() {
  if (started_) return;
  started_ = true;
  take_sample(0);
}

void MetricsObserver::take_sample(TimeNs now) {
  BboRow row{now, top_, mid_, fundamental_, inventory_, opt_.mm_owner ? pnl_now() : 0.0};
  bbo_rows_.push_back(row);
  if (now >= opt_.warmup_ns && now <= opt_.end_ns) sampled_mids_.push_back(mid_);
}

void MetricsObserver::advance(TimeNs now) {
  ensure_started();
  if (!warm_captured_ && now >= opt_.warmup_ns) {
    warm_captured_ = true;
    pnl_at_warmup_ = pnl_now();
    window_max_inventory_ = inventory_ < 0 ? -inventory_ : inventory_;
  }
  const TimeNs lo = std::max(last_time_, opt_.warmup_ns);
  const TimeNs hi = std::min(now, opt_.end_ns);
  if (hi > lo) {
    const TimeNs dt = hi - lo;
    if (top_.two_sided()) spread_time_[top_.ask - top_.bid] += dt;
    if (quoting_both_) quoted_spread_time_[quoted_spread_] += dt;
  }
  // A markout due strictly before `now` sees the state after every event with timestamp
  // <= its due time: all of those precede `now`, and nothing changed since the last event.
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    auto& queue = pending_[i];
    auto& head = pending_head_[i];
    while (head < queue.size() && fills_[queue[head]].time + opt_.markout_horizons_ns[i] < now) {
      Fill& f = fills_[queue[head]];
      if (mid_) f.markouts[i] = stats::markout_per_lot(f.side, f.price, *mid_);
      ++head;
    }
  }
  last_time_ = now;
}

void MetricsObserver::on_agent_send(TimeNs, AgentIndex agent, const lob::Request& request) {
  if (!std::holds_alternative<lob::NewOrder>(request)) return;
  const Role r = agent < opt_.roles.size() ? opt_.roles[agent] : Role::Other;
  if (r != Role::Other) ++counts_.roles[role_slot(r)].orders_sent;
}

void MetricsObserver::on_request(TimeNs now, const lob::Request& request) {
  advance(now);
  mid_before_request_ = mid_;  // the mid in force; empty while the book is one-sided
  pending_new_.reset();
  if (const auto* n = std::get_if<lob::NewOrder>(&request)) {
    ++counts_.requests_new;
    if (opt_.mm_owner && n->owner == *opt_.mm_owner) pending_new_ = *n;
  } else if (std::holds_alternative<lob::Cancel>(request)) {
    ++counts_.requests_cancel;
  } else {
    ++counts_.requests_modify;
  }
}

void MetricsObserver::reduce_quote(lob::OrderId id, lob::Qty qty) {
  for (auto it = quotes_.begin(); it != quotes_.end(); ++it) {
    if (it->id != id) continue;
    it->remaining -= qty;
    if (it->remaining <= 0) quotes_.erase(it);
    return;
  }
}

void MetricsObserver::record_fill(TimeNs now, const lob::Trade& t, lob::Side side, bool maker) {
  if (side == lob::Side::Buy) {
    cash_ -= t.price * t.qty;
    inventory_ += t.qty;
  } else {
    cash_ += t.price * t.qty;
    inventory_ -= t.qty;
  }
  fees_ += static_cast<double>(t.qty) * (maker ? opt_.maker_fee_ticks : opt_.taker_fee_ticks);
  const std::int64_t abs_inv = inventory_ < 0 ? -inventory_ : inventory_;
  total_max_inventory_ = std::max(total_max_inventory_, abs_inv);
  const bool in_window = now >= opt_.warmup_ns;
  if (in_window) window_max_inventory_ = std::max(window_max_inventory_, abs_inv);

  Fill f{now, t.trade_id, side, t.price, t.qty, maker, inventory_, in_window, mid_before_request_,
         std::vector<std::optional<double>>(opt_.markout_horizons_ns.size())};
  fills_.push_back(std::move(f));
  for (std::size_t i = 0; i < opt_.markout_horizons_ns.size(); ++i)
    if (now + opt_.markout_horizons_ns[i] <= opt_.end_ns) pending_[i].push_back(fills_.size() - 1);
}

void MetricsObserver::on_engine_event(TimeNs now, const lob::Event& event) {
  const bool have_mm = opt_.mm_owner.has_value();
  const lob::OwnerId mm = have_mm ? *opt_.mm_owner : 0;
  if (const auto* a = std::get_if<lob::Ack>(&event)) {
    ++counts_.ev_ack;
    if (have_mm && a->owner == mm && pending_new_)
      quotes_.push_back({a->order_id, pending_new_->side, pending_new_->price, pending_new_->qty});
  } else if (const auto* r = std::get_if<lob::Reject>(&event)) {
    ++counts_.ev_reject;
    ++counts_.rejects[static_cast<std::size_t>(r->reason)];
  } else if (const auto* t = std::get_if<lob::Trade>(&event)) {
    ++counts_.ev_trade;
    ++counts_.trades;
    counts_.traded_volume += t->qty;
    const Role mr = role_of_owner(t->maker_owner), tr = role_of_owner(t->taker_owner);
    if (mr != Role::Other) counts_.roles[role_slot(mr)].maker_volume += t->qty;
    if (tr != Role::Other) counts_.roles[role_slot(tr)].taker_volume += t->qty;
    trades_.push_back({now, *t});
    if (have_mm) {
      if (t->maker_owner == mm) {
        record_fill(now, *t, lob::opposite(t->aggressor_side), true);
        reduce_quote(t->maker_order_id, t->qty);
      }
      if (t->taker_owner == mm) {
        record_fill(now, *t, t->aggressor_side, false);
        reduce_quote(t->taker_order_id, t->qty);
      }
    }
  } else if (const auto* c = std::get_if<lob::Cancelled>(&event)) {
    ++counts_.ev_cancelled;
    if (have_mm && c->owner == mm)
      std::erase_if(quotes_, [&](const QuoteOrder& q) { return q.id == c->order_id; });
  } else if (const auto* m = std::get_if<lob::Modified>(&event)) {
    ++counts_.ev_modified;
    if (have_mm && m->owner == mm) {
      for (auto& q : quotes_)
        if (q.id == m->order_id) {
          q.price = m->new_price;
          q.remaining = m->new_qty;
        }
    }
  }
}

void MetricsObserver::on_book(TimeNs, const BBO& top, bool) {
  top_ = top;
  if (top_.two_sided()) {
    mid_ = static_cast<double>(top_.bid + top_.ask) / 2.0;
    last_mid_ = mid_;
  } else {
    mid_.reset();
  }
  std::optional<lob::Price> bid, ask;
  for (const auto& q : quotes_) {
    if (q.side == lob::Side::Buy) bid = bid ? std::max(*bid, q.price) : q.price;
    else ask = ask ? std::min(*ask, q.price) : q.price;
  }
  quoting_both_ = bid && ask;
  quoted_spread_ = quoting_both_ ? *ask - *bid : 0;
}

void MetricsObserver::on_fundamental(TimeNs now, double value) {
  advance(now);
  fundamental_ = value;
}

void MetricsObserver::on_sample(TimeNs now) {
  advance(now);
  take_sample(now);
}

void MetricsObserver::on_end(TimeNs end) {
  advance(end);
  if (!warm_captured_) {  // warm-up beyond the end: an empty window
    warm_captured_ = true;
    pnl_at_warmup_ = pnl_now();
    window_max_inventory_ = inventory_ < 0 ? -inventory_ : inventory_;
  }
  // No event can follow: everything still pending is due at or before `end`.
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    auto& queue = pending_[i];
    for (; pending_head_[i] < queue.size(); ++pending_head_[i]) {
      Fill& f = fills_[queue[pending_head_[i]]];
      if (mid_) f.markouts[i] = stats::markout_per_lot(f.side, f.price, *mid_);
    }
  }
  if (opt_.sample_interval_ns > 0 && end > 0 && end % opt_.sample_interval_ns == 0) take_sample(end);
  final_pnl_ = pnl_now();
  final_inventory_value_ = static_cast<double>(inventory_) * last_mid_.value_or(0.0);
}

MetricsSummary MetricsObserver::summary() const {
  MetricsSummary s;
  s.counts = counts_;
  s.has_mm = opt_.mm_owner.has_value();
  const double window_ns = static_cast<double>(std::max<TimeNs>(0, opt_.end_ns - opt_.warmup_ns));

  if (s.has_mm) {
    MmTotalStats& tot = s.mm_total;
    MmWindowStats& win = s.mm_window;
    for (const Fill& f : fills_) {
      const bool buy = f.side == lob::Side::Buy;
      ++tot.fills;
      (buy ? tot.buy_fills : tot.sell_fills) += 1;
      (buy ? tot.volume_buy : tot.volume_sell) += f.qty;
      if (f.in_window) {
        ++win.fills;
        (buy ? win.buy_fills : win.sell_fills) += 1;
        (buy ? win.volume_buy : win.volume_sell) += f.qty;
      }
    }
    tot.cash = cash_;
    tot.inventory_value = final_inventory_value_;
    tot.fees = fees_;
    tot.pnl_total = final_pnl_;
    tot.final_inventory = inventory_;
    tot.max_abs_inventory = total_max_inventory_;
    win.pnl_after_warmup = final_pnl_ - pnl_at_warmup_;
    win.max_abs_inventory = window_max_inventory_;
    long double quoting = 0;
    for (const auto& kv : quoted_spread_time_) quoting += static_cast<long double>(kv.second);
    if (window_ns > 0) win.quoting_fraction = static_cast<double>(quoting / static_cast<long double>(window_ns));
    win.avg_quoted_spread = stats::weighted_mean(quoted_spread_time_);

    for (std::size_t i = 0; i < opt_.markout_horizons_ns.size(); ++i) {
      MarkoutStats m;
      m.horizon_s = i < opt_.markout_horizons_s.size() ? opt_.markout_horizons_s[i]
                                                       : static_cast<double>(opt_.markout_horizons_ns[i]) / 1e9;
      std::vector<double> values, d_markout, d_capture, d_move;
      std::vector<double> d_qty;
      double wsum = 0, qsum = 0;
      for (const Fill& f : fills_) {
        if (!f.in_window) continue;
        if (f.markouts[i] && f.mid_before) {
          const double sc = spread_capture(f);
          d_markout.push_back(*f.markouts[i]);
          d_capture.push_back(sc);
          d_move.push_back(*f.markouts[i] - sc);
          d_qty.push_back(static_cast<double>(f.qty));
        }
        if (f.markouts[i]) {
          values.push_back(*f.markouts[i]);
          wsum += *f.markouts[i] * static_cast<double>(f.qty);
          qsum += static_cast<double>(f.qty);
        } else {
          ++m.skipped;
        }
      }
      m.n = values.size();
      m.mean = stats::mean(values);
      m.sd = stats::sample_sd(values);
      if (m.mean && m.sd) {
        if (const auto ci = stats::confidence_interval_95(values.size(), *m.mean, *m.sd)) {
          m.ci_lo = ci->lo;
          m.ci_hi = ci->hi;
        }
      }
      if (qsum > 0) m.mean_qty_weighted = wsum / qsum;
      Decomposition& d = m.decomposition;
      d.n = d_move.size();
      d.markout_mean = stats::mean(d_markout);
      d.spread_capture_mean = stats::mean(d_capture);
      d.mid_move_mean = stats::mean(d_move);
      d.mid_move_sd = stats::sample_sd(d_move);
      if (d.mid_move_mean && d.mid_move_sd)
        if (const auto ci = stats::confidence_interval_95(d_move.size(), *d.mid_move_mean, *d.mid_move_sd)) {
          d.mid_move_ci_lo = ci->lo;
          d.mid_move_ci_hi = ci->hi;
        }
      double mw = 0, mq = 0;
      for (std::size_t k = 0; k < d_move.size(); ++k) {
        mw += d_move[k] * d_qty[k];
        mq += d_qty[k];
      }
      if (mq > 0) d.mid_move_mean_qty_weighted = mw / mq;
      s.markouts.push_back(m);
    }
  }

  if (s.has_mm) {
    std::vector<double> sc;
    double wsum = 0, qsum = 0;
    for (const Fill& f : fills_) {
      if (!f.in_window || !f.mid_before) continue;
      sc.push_back(spread_capture(f));
      wsum += sc.back() * static_cast<double>(f.qty);
      qsum += static_cast<double>(f.qty);
    }
    SpreadCaptureStats& c = s.spread_capture;
    c.n = sc.size();
    c.mean = stats::mean(sc);
    c.sd = stats::sample_sd(sc);
    if (c.mean && c.sd)
      if (const auto ci = stats::confidence_interval_95(sc.size(), *c.mean, *c.sd)) {
        c.ci_lo = ci->lo;
        c.ci_hi = ci->hi;
      }
    if (qsum > 0) c.mean_qty_weighted = wsum / qsum;
  }

  // Spread.
  SpreadStats& sp = s.spread;
  long double two_sided = 0;
  for (const auto& kv : spread_time_) two_sided += static_cast<long double>(kv.second);
  if (window_ns > 0) sp.two_sided_fraction = static_cast<double>(two_sided / static_cast<long double>(window_ns));
  sp.mean = stats::weighted_mean(spread_time_);
  sp.median = stats::weighted_quantile(spread_time_, 0.5);
  sp.p90 = stats::weighted_quantile(spread_time_, 0.9);
  sp.p99 = stats::weighted_quantile(spread_time_, 0.99);
  sp.histogram.assign(static_cast<std::size_t>(kSpreadHistogramMax), 0.0);
  if (two_sided > 0) {
    for (const auto& [spread, ns] : spread_time_) {
      const double frac = static_cast<double>(static_cast<long double>(ns) / two_sided);
      if (spread >= 1 && spread <= kSpreadHistogramMax) sp.histogram[static_cast<std::size_t>(spread - 1)] += frac;
      else sp.overflow += frac;
    }
  }

  // Returns.
  ReturnStats& rs = s.returns;
  rs.samples = sampled_mids_.size();
  for (const auto& m : sampled_mids_)
    if (!m) ++rs.missing;
  const stats::Series rets = stats::log_returns(sampled_mids_);
  std::vector<double> present;
  std::uint64_t zeros = 0;
  for (const auto& r : rets)
    if (r) {
      present.push_back(*r);
      if (*r == 0.0) ++zeros;
    }
  rs.returns = present.size();
  rs.mean = stats::mean(present);
  rs.sd = stats::sample_sd(present);
  if (!present.empty()) rs.zero_fraction = static_cast<double>(zeros) / static_cast<double>(present.size());
  rs.excess_kurtosis = stats::excess_kurtosis(present);
  stats::Series abs_rets = rets;
  for (auto& r : abs_rets)
    if (r) r = std::fabs(*r);
  for (std::size_t lag = 1; lag <= 5; ++lag) rs.acf.push_back(stats::autocorrelation(rets, lag));
  for (std::size_t lag = 1; lag <= 10; ++lag) rs.abs_acf.push_back(stats::autocorrelation(abs_rets, lag));
  return s;
}

// ---------------------------------------------------------------------------------------
// files
// ---------------------------------------------------------------------------------------
void MetricsObserver::write_trades_csv(std::ostream& out) const {
  std::string s = "time_ns,trade_id,price,qty,aggressor_side,maker_order_id,taker_order_id,maker_owner,taker_owner\n";
  for (const TradeRow& r : trades_) {
    const lob::Trade& t = r.trade;
    append_ll(s, r.time);
    s += ',';
    s += std::to_string(t.trade_id);
    s += ',';
    append_ll(s, t.price);
    s += ',';
    append_ll(s, t.qty);
    s += t.aggressor_side == lob::Side::Buy ? ",buy," : ",sell,";
    s += std::to_string(t.maker_order_id);
    s += ',';
    s += std::to_string(t.taker_order_id);
    s += ',';
    s += std::to_string(t.maker_owner);
    s += ',';
    s += std::to_string(t.taker_owner);
    s += '\n';
    if (s.size() > (1u << 16)) {
      out.write(s.data(), static_cast<std::streamsize>(s.size()));
      s.clear();
    }
  }
  out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void MetricsObserver::write_bbo_csv(std::ostream& out) const {
  std::string s = "time_ns,bid,bid_qty,ask,ask_qty,mid,fundamental,mm_inventory,mm_pnl\n";
  for (const BboRow& r : bbo_rows_) {
    append_ll(s, r.time);
    s += ',';
    if (r.top.has_bid()) {
      append_ll(s, r.top.bid);
      s += ',';
      append_ll(s, r.top.bid_qty);
    } else {
      s += ',';
    }
    s += ',';
    if (r.top.has_ask()) {
      append_ll(s, r.top.ask);
      s += ',';
      append_ll(s, r.top.ask_qty);
    } else {
      s += ',';
    }
    s += ',';
    if (r.mid) s += fmt("%.1f", *r.mid);
    s += ',';
    s += fmt("%.6f", r.fundamental);
    s += ',';
    if (opt_.mm_owner) {
      append_ll(s, r.mm_inventory);
      s += ',';
      s += fmt("%.6f", r.mm_pnl);
    } else {
      s += ',';
    }
    s += '\n';
  }
  out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void MetricsObserver::write_mm_fills_csv(std::ostream& out) const {
  std::string s = "time_ns,trade_id,side,price,qty,role,inventory_after,in_window,mid_before";
  for (std::size_t i = 0; i < opt_.markout_horizons_ns.size(); ++i) {
    const double h = i < opt_.markout_horizons_s.size() ? opt_.markout_horizons_s[i]
                                                        : static_cast<double>(opt_.markout_horizons_ns[i]) / 1e9;
    s += ",markout_" + fmt("%g", h) + "s";
  }
  s += '\n';
  for (const Fill& f : fills_) {
    append_ll(s, f.time);
    s += ',';
    s += std::to_string(f.trade_id);
    s += f.side == lob::Side::Buy ? ",buy," : ",sell,";
    append_ll(s, f.price);
    s += ',';
    append_ll(s, f.qty);
    s += f.maker ? ",maker," : ",taker,";
    append_ll(s, f.inventory_after);
    s += f.in_window ? ",1," : ",0,";
    if (f.mid_before) s += fmt("%.1f", *f.mid_before);
    for (const auto& m : f.markouts) {
      s += ',';
      if (m) s += fmt("%.1f", *m);
    }
    s += '\n';
  }
  out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

namespace {

using json = nlohmann::ordered_json;

// Doubles go through "%.6f" so the printed value does not depend on shortest-round-trip
// details; non-finite values and missing values are null.
json num(double v) {
  if (!std::isfinite(v)) return nullptr;
  return std::strtod(fmt("%.6f", v).c_str(), nullptr);
}
json num(const std::optional<double>& v) { return v ? num(*v) : json(nullptr); }
// Six significant digits, scientific form: for the return mean and sd, which are around 1e-5..1e-7.
json num_sci(const std::optional<double>& v) {
  if (!v || !std::isfinite(*v)) return nullptr;
  return std::strtod(fmt("%.6e", *v).c_str(), nullptr);
}
json num(const std::optional<std::int64_t>& v) { return v ? json(*v) : json(nullptr); }
json num_list(const std::vector<std::optional<double>>& v) {
  json a = json::array();
  for (const auto& x : v) a.push_back(num(x));
  return a;
}

}  // namespace

std::string MetricsObserver::summary_json(const SimConfig& cfg, std::uint64_t engine_event_hash) const {
  const MetricsSummary s = summary();
  json j = json::object();
  j["seed"] = cfg.seed;
  j["config"] = json::parse(to_json(cfg).dump());
  char hash[17];
  std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(engine_event_hash));
  j["engine_event_hash"] = std::string(hash);
  j["window"] = {{"warmup_ns", opt_.warmup_ns}, {"end_ns", opt_.end_ns}};

  if (s.has_mm) {
    json w = json::object();
    w["fills"] = s.mm_window.fills;
    w["buy_fills"] = s.mm_window.buy_fills;
    w["sell_fills"] = s.mm_window.sell_fills;
    w["volume_buy_lots"] = s.mm_window.volume_buy;
    w["volume_sell_lots"] = s.mm_window.volume_sell;
    w["pnl_after_warmup"] = num(s.mm_window.pnl_after_warmup);
    w["max_abs_inventory"] = s.mm_window.max_abs_inventory;
    w["quoting_fraction"] = num(s.mm_window.quoting_fraction);
    w["avg_quoted_spread_ticks"] = num(s.mm_window.avg_quoted_spread);
    json t = json::object();
    t["fills"] = s.mm_total.fills;
    t["buy_fills"] = s.mm_total.buy_fills;
    t["sell_fills"] = s.mm_total.sell_fills;
    t["volume_buy_lots"] = s.mm_total.volume_buy;
    t["volume_sell_lots"] = s.mm_total.volume_sell;
    t["pnl_total"] = num(s.mm_total.pnl_total);
    t["cash_ticks"] = s.mm_total.cash;
    t["inventory_value_ticks"] = num(s.mm_total.inventory_value);
    t["fees_ticks"] = num(s.mm_total.fees);
    t["final_inventory"] = s.mm_total.final_inventory;
    t["max_abs_inventory"] = s.mm_total.max_abs_inventory;
    j["market_maker"] = {{"window", w}, {"whole_run", t}};
  } else {
    j["market_maker"] = nullptr;
  }

  json mo = json::array();
  for (const MarkoutStats& m : s.markouts) {
    mo.push_back({{"horizon_s", num(m.horizon_s)},
                  {"n", m.n},
                  {"skipped", m.skipped},
                  {"mean", num(m.mean)},
                  {"sd", num(m.sd)},
                  {"ci95_lo", num(m.ci_lo)},
                  {"ci95_hi", num(m.ci_hi)},
                  {"mean_qty_weighted", num(m.mean_qty_weighted)},
                  {"decomposition",
                   {{"n", m.decomposition.n},
                    {"markout_mean", num(m.decomposition.markout_mean)},
                    {"spread_capture_mean", num(m.decomposition.spread_capture_mean)},
                    {"mid_move_mean", num(m.decomposition.mid_move_mean)},
                    {"mid_move_sd", num(m.decomposition.mid_move_sd)},
                    {"mid_move_ci95_lo", num(m.decomposition.mid_move_ci_lo)},
                    {"mid_move_ci95_hi", num(m.decomposition.mid_move_ci_hi)},
                    {"mid_move_mean_qty_weighted", num(m.decomposition.mid_move_mean_qty_weighted)}}}});
  }
  j["markouts"] = std::move(mo);
  if (s.has_mm) {
    const SpreadCaptureStats& c = s.spread_capture;
    j["spread_capture"] = {{"n", c.n},
                           {"mean", num(c.mean)},
                           {"sd", num(c.sd)},
                           {"ci95_lo", num(c.ci_lo)},
                           {"ci95_hi", num(c.ci_hi)},
                           {"mean_qty_weighted", num(c.mean_qty_weighted)}};
  } else {
    j["spread_capture"] = nullptr;
  }

  json hist = json::object();
  for (std::size_t i = 0; i < s.spread.histogram.size(); ++i) hist[std::to_string(i + 1)] = num(s.spread.histogram[i]);
  hist["overflow"] = num(s.spread.overflow);
  j["spread"] = {{"two_sided_fraction", num(s.spread.two_sided_fraction)},
                 {"mean_ticks", num(s.spread.mean)},
                 {"median_ticks", num(s.spread.median)},
                 {"p90_ticks", num(s.spread.p90)},
                 {"p99_ticks", num(s.spread.p99)},
                 {"histogram_max_ticks", kSpreadHistogramMax},
                 {"histogram", hist}};

  j["returns"] = {{"sample_interval_s", num(static_cast<double>(opt_.sample_interval_ns) / 1e9)},
                  {"samples", s.returns.samples},
                  {"missing_samples", s.returns.missing},
                  {"returns", s.returns.returns},
                  {"mean", num_sci(s.returns.mean)},
                  {"sd", num_sci(s.returns.sd)},
                  {"zero_fraction", num(s.returns.zero_fraction)},
                  {"excess_kurtosis", num(s.returns.excess_kurtosis)},
                  {"acf_lags_1_to_5", num_list(s.returns.acf)},
                  {"abs_acf_lags_1_to_10", num_list(s.returns.abs_acf)}};

  const RunCounts& c = s.counts;
  json rej = json::object();
  for (std::size_t r = 0; r < c.rejects.size(); ++r) rej[lob::to_string(static_cast<lob::RejectReason>(r))] = c.rejects[r];
  json roles = json::object();
  static constexpr const char* names[kRoleCount] = {"noise", "informed", "market_maker"};
  for (std::size_t r = 0; r < kRoleCount; ++r)
    roles[names[r]] = {{"orders_sent", c.roles[r].orders_sent},
                       {"taker_volume_lots", c.roles[r].taker_volume},
                       {"maker_volume_lots", c.roles[r].maker_volume}};
  j["counts"] = {{"requests", {{"new", c.requests_new}, {"cancel", c.requests_cancel}, {"modify", c.requests_modify}}},
                 {"engine_events",
                  {{"ack", c.ev_ack},
                   {"reject", c.ev_reject},
                   {"trade", c.ev_trade},
                   {"cancelled", c.ev_cancelled},
                   {"modified", c.ev_modified}}},
                 {"trades", c.trades},
                 {"traded_volume_lots", c.traded_volume},
                 {"rejects", rej},
                 {"roles", roles}};
  return j.dump(2) + "\n";
}

}  // namespace sim
