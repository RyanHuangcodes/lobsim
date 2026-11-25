// The estimator: a replay observer that measures rates, placement, sizes and lifetimes inside the
// window, and the code that resolves the window and the tick and fills the Calibration. The
// statistics are in estimator_stats.cpp, the file names and the summary in estimator_report.cpp.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "calib/estimator.hpp"
#include "calib/sha256.hpp"
#include "replay/lobster.hpp"
#include "replay/replayer.hpp"

namespace calib {

const char* to_string(LifetimeEnd e) noexcept {
  switch (e) {
    case LifetimeEnd::Cancelled: return "cancelled";
    case LifetimeEnd::Executed: return "executed";
    case LifetimeEnd::WindowEnd: return "window_end";
    case LifetimeEnd::LeftWindow: return "left_window";
  }
  return "?";
}

namespace {

using json = nlohmann::json;
using replay::LobsterMsg;

// Seconds after midnight with 9 decimals, from integer nanoseconds.
std::string seconds_text(std::int64_t ns) {
  std::string frac = std::to_string(ns % kNsPerSecond);
  frac.insert(0, 9 - frac.size(), '0');
  return std::to_string(ns / kNsPerSecond) + "." + frac;
}

// Messages of types 2, 3 and 4, counted by type.
struct ByType {
  std::uint64_t n[3]{};
  void add(int type) { ++n[type - 2]; }
  std::uint64_t total() const { return n[0] + n[1] + n[2]; }
  json to_json() const { return json{{"type_2", n[0]}, {"type_3", n[1]}, {"type_4", n[2]}}; }
};

struct Tracked {
  int side{kBuy};
  std::int64_t price{0};
  std::int64_t remaining{0};
  std::int64_t submit_ns{0};
  std::uint64_t submit_line{0};
  std::int64_t delta{0};
  std::int64_t spread{0};
  int bucket{0};
};

// The observer. Every message handed to it lands in the counters of the report; see D-301..D-319.
class Collector final : public replay::ReplayObserver {
 public:
  Collector(std::int64_t tick, std::int64_t from_ns, std::int64_t to_ns, bool keep)
      : tick_(tick), from_ns_(from_ns), to_ns_(to_ns), keep_(keep), last_t_(from_ns), mid_since_(from_ns) {}

  void on_message(const replay::ObservedMessage& m) override;
  void on_end(const replay::RunEnd& end) override;

  // Results, read by estimate() after the run.
  std::int64_t tick_;
  std::int64_t from_ns_, to_ns_;
  bool keep_;
  std::uint64_t lines_{0}, unparsed_{0}, time_decreased_{0};
  std::uint64_t by_type_[8]{};
  std::optional<std::int64_t> first_ns_, last_ns_;
  std::uint64_t first_line_{0}, last_line_{0};
  std::array<std::uint64_t, 2> limit_count_{}, aggressive_count_{};
  std::map<std::int64_t, std::uint64_t> limit_size_, aggressive_size_;
  std::array<std::array<std::map<std::int64_t, std::uint64_t>, kSpreadBuckets>, 2> placement_{};
  std::map<std::string, std::uint64_t> placement_excluded_;
  std::uint64_t subpenny_{0}, dup_id_{0}, id_reused_{0};
  std::uint64_t execution_line_1_{0}, burst_partial_{0}, aggressive_size_overflow_{0}, bursts_messages_{0};
  std::vector<std::int8_t> signs_;
  ByType unknown_id_, after_censoring_, to_excluded_, mismatch_;
  std::uint64_t delete_size_mismatch_{0}, partial_cancel_to_zero_{0}, oversized_execution_{0};
  std::uint64_t partial_cancels_{0}, partial_executions_{0}, zero_lifetime_clamped_{0};
  std::uint64_t hidden_{0}, cross_{0}, halt_{0};
  std::uint64_t rows_unchecked_{0};
  std::array<std::vector<std::int64_t>, kDistanceBuckets> events_{}, censored_{};
  std::array<Survival, kDistanceBuckets> counts_{};  // only the order and outcome counters
  std::vector<LifetimeRecord> records_;
  // Time-weighted mid: weight (ns) per best bid + best ask, and the time without a two-sided row.
  std::map<std::int64_t, std::int64_t> mid2_ns_;
  std::int64_t one_sided_ns_{0}, no_row_ns_{0};
  replay::RunEnd end_;

 private:
  void submit(const LobsterMsg& m, const replay::PublishedRow& before, std::uint64_t line, std::int64_t t);
  void reference(const LobsterMsg& m, std::uint64_t line, std::int64_t t);
  void burst(const replay::ObservedMessage& m);
  void leave_window(const replay::PublishedRow& after, std::uint64_t line, std::int64_t t);
  void finish(std::unordered_map<std::int64_t, Tracked>::iterator it, LifetimeEnd how, std::int64_t t,
              std::uint64_t line);
  void weigh_mid(std::int64_t t);
  static std::optional<std::int64_t> mid2(const replay::PublishedRow& r, bool& one_sided);

  std::int64_t last_t_;
  bool started_{false};
  std::optional<std::int64_t> mid_state_;  // bid + ask of the row in effect
  bool mid_one_sided_{false};
  std::int64_t mid_since_;
  std::unordered_map<std::int64_t, Tracked> tracked_;
  std::array<std::set<std::pair<std::int64_t, std::int64_t>>, 2> by_price_{};  // (price, id) of tracked orders
  std::unordered_set<std::int64_t> censored_ids_;  // left the window: later messages are theirs
  std::unordered_set<std::int64_t> excluded_ids_;  // submitted in the window but not tracked
  int burst_side_{kBuy};
  std::int64_t burst_sum_{0};
  bool burst_open_{false}, burst_overflow_{false};
};

std::optional<std::int64_t> Collector::mid2(const replay::PublishedRow& r, bool& one_sided) {
  one_sided = false;
  if (r.row == nullptr || r.malformed) return std::nullopt;
  const replay::BookLevel& bid = r.row->bids.front();
  const replay::BookLevel& ask = r.row->asks.front();
  if (bid.empty() || ask.empty()) {
    one_sided = true;
    return std::nullopt;
  }
  return bid.price + ask.price;
}

// The row in effect since mid_since_ held until t.
void Collector::weigh_mid(std::int64_t t) {
  const std::int64_t w = t - mid_since_;
  if (mid_state_) mid2_ns_[*mid_state_] += w;
  else if (mid_one_sided_) one_sided_ns_ += w;
  else no_row_ns_ += w;
  mid_since_ = t;
}

void Collector::on_message(const replay::ObservedMessage& m) {
  ++lines_;
  if (!started_) {
    started_ = true;
    first_line_ = m.line;
    mid_state_ = mid2(m.before, mid_one_sided_);  // the book at the window start
  }
  last_line_ = m.line;
  std::int64_t t = last_t_;
  if (m.msg != nullptr) {
    const std::int64_t raw = m.msg->time_ns;
    first_ns_ = first_ns_ ? std::min(*first_ns_, raw) : raw;
    last_ns_ = last_ns_ ? std::max(*last_ns_, raw) : raw;
    if (raw < last_t_) ++time_decreased_;  // LOBSTER times never decrease; clamp, so no lifetime is negative
    else t = raw;
    last_t_ = t;
  } else {
    ++unparsed_;  // its time is unknown: its row takes effect at the last known time
  }
  weigh_mid(t);
  mid_state_ = mid2(m.after, mid_one_sided_);
  if (m.msg != nullptr) {
    const LobsterMsg& msg = *m.msg;
    ++by_type_[msg.type];
    switch (msg.type) {
      case 1: submit(msg, m.before, m.line, t); break;
      case 2:
      case 3: reference(msg, m.line, t); break;
      case 4:
        burst(m);
        reference(msg, m.line, t);
        break;
      case 5: ++hidden_; break;
      case 6: ++cross_; break;
      default: ++halt_; break;  // 7: the parser accepts 1..7 only
    }
  }
  leave_window(m.after, m.line, t);
}

void Collector::submit(const LobsterMsg& m, const replay::PublishedRow& before, std::uint64_t line, std::int64_t t) {
  const int side = m.direction > 0 ? kBuy : kSell;
  if (m.price % tick_ != 0) {
    ++subpenny_;
    excluded_ids_.insert(m.order_id);
    return;
  }
  ++limit_count_[side];
  ++limit_size_[m.size];

  // Placement, from the true book just before the order.
  const char* why = nullptr;
  std::int64_t spread = 0, delta = 0;
  if (before.row == nullptr) {
    why = line == 1 ? "no_previous_row" : "previous_row_unparsed";
  } else if (before.malformed) {
    why = "previous_row_malformed";
  } else {
    const replay::BookLevel& bid = before.row->bids.front();
    const replay::BookLevel& ask = before.row->asks.front();
    if (bid.empty() || ask.empty()) {
      why = "one_sided_previous_row";
    } else if (bid.price % tick_ != 0 || ask.price % tick_ != 0) {
      why = "best_quote_off_tick";
    } else if (ask.price <= bid.price) {
      why = "crossed_previous_row";
    } else {
      spread = (ask.price - bid.price) / tick_;
      delta = side == kBuy ? (bid.price - m.price) / tick_ : (m.price - ask.price) / tick_;
      if (delta < -(spread - 1)) why = "would_cross";
    }
  }
  if (why != nullptr) {
    ++placement_excluded_[why];
    excluded_ids_.insert(m.order_id);
    return;
  }
  ++placement_[side][spread_bucket(spread)][delta];

  // Lifetime: the clock starts now.
  if (tracked_.count(m.order_id) != 0) {
    ++dup_id_;  // an id that is still resting: the first order keeps it
    return;
  }
  if (censored_ids_.erase(m.order_id) + excluded_ids_.erase(m.order_id) > 0) ++id_reused_;
  Tracked o{side, m.price, m.size, t, line, delta, spread, distance_bucket(delta)};
  ++counts_[o.bucket].orders;
  tracked_.emplace(m.order_id, o);
  by_price_[side].emplace(m.price, m.order_id);
}

void Collector::reference(const LobsterMsg& m, std::uint64_t line, std::int64_t t) {
  const auto it = tracked_.find(m.order_id);
  if (it == tracked_.end()) {
    if (censored_ids_.count(m.order_id) != 0) after_censoring_.add(m.type);
    else if (excluded_ids_.count(m.order_id) != 0) to_excluded_.add(m.type);
    else unknown_id_.add(m.type);  // submitted before the window, a seed order, or a filtered submission
    return;
  }
  Tracked& o = it->second;
  if ((m.direction > 0 ? kBuy : kSell) != o.side || m.price != o.price) {
    mismatch_.add(m.type);  // the message disagrees with the order its id names: ignored
    return;
  }
  if (m.type == 3) {
    if (m.size != o.remaining) ++delete_size_mismatch_;
    finish(it, LifetimeEnd::Cancelled, t, line);
  } else if (m.type == 2) {
    o.remaining -= m.size;
    ++partial_cancels_;
    if (o.remaining <= 0) {
      ++partial_cancel_to_zero_;  // nothing left: it was a full cancellation
      finish(it, LifetimeEnd::Cancelled, t, line);
    }
  } else {
    o.remaining -= m.size;
    if (o.remaining < 0) ++oversized_execution_;
    if (o.remaining <= 0) finish(it, LifetimeEnd::Executed, t, line);
    else ++partial_executions_;
  }
}

void Collector::burst(const replay::ObservedMessage& m) {
  const LobsterMsg& msg = *m.msg;
  if (m.burst_size == 0) {
    ++execution_line_1_;  // line 1, consumed by the opening seed: in no burst (D-180)
    return;
  }
  if (m.burst_position == 0) {
    burst_open_ = true;
    burst_side_ = msg.direction > 0 ? kSell : kBuy;  // Direction is the resting side
    burst_sum_ = 0;
    burst_overflow_ = false;
  } else if (!burst_open_) {
    ++burst_partial_;  // unreachable: a burst shares one time, so the window never cuts one
    return;
  }
  ++bursts_messages_;
  if (__builtin_add_overflow(burst_sum_, msg.size, &burst_sum_)) burst_overflow_ = true;
  if (m.burst_position + 1 < m.burst_size) return;
  burst_open_ = false;
  ++aggressive_count_[burst_side_];
  signs_.push_back(burst_side_ == kBuy ? 1 : -1);
  if (burst_overflow_) ++aggressive_size_overflow_;
  else ++aggressive_size_[burst_sum_];
}

// Censors every tracked order priced worse than the published L-th level of its side.
void Collector::leave_window(const replay::PublishedRow& after, std::uint64_t line, std::int64_t t) {
  if (after.row == nullptr || after.malformed) {
    ++rows_unchecked_;
    return;
  }
  if (after.bid_bound) {
    auto& buys = by_price_[kBuy];
    while (!buys.empty() && buys.begin()->first < *after.bid_bound) {
      finish(tracked_.find(buys.begin()->second), LifetimeEnd::LeftWindow, t, line);
    }
  }
  if (after.ask_bound) {
    auto& sells = by_price_[kSell];
    while (!sells.empty() && sells.rbegin()->first > *after.ask_bound) {
      finish(tracked_.find(sells.rbegin()->second), LifetimeEnd::LeftWindow, t, line);
    }
  }
}

void Collector::finish(std::unordered_map<std::int64_t, Tracked>::iterator it, LifetimeEnd how, std::int64_t t,
                       std::uint64_t line) {
  const std::int64_t id = it->first;
  const Tracked o = it->second;
  std::int64_t life = t - o.submit_ns;
  Survival& c = counts_[o.bucket];
  switch (how) {
    case LifetimeEnd::Cancelled:
      if (life == 0) {
        ++zero_lifetime_clamped_;  // a cancellation in the submission's nanosecond: 1 ns
        life = 1;
      }
      events_[o.bucket].push_back(life);
      ++c.cancelled;
      break;
    case LifetimeEnd::Executed:
      censored_[o.bucket].push_back(life);
      ++c.censored_executed;
      break;
    case LifetimeEnd::WindowEnd:
      censored_[o.bucket].push_back(life);
      ++c.censored_window_end;
      break;
    case LifetimeEnd::LeftWindow:
      censored_[o.bucket].push_back(life);
      ++c.censored_left_window;
      censored_ids_.insert(id);
      break;
  }
  if (keep_) {
    records_.push_back(LifetimeRecord{id, o.side, o.price, o.delta, o.spread, o.bucket, o.submit_ns, o.submit_line,
                                      how, o.submit_ns + life, line});
  }
  by_price_[o.side].erase({o.price, id});
  tracked_.erase(it);
}

void Collector::on_end(const replay::RunEnd& end) {
  end_ = end;
  if (!started_) return;
  weigh_mid(std::max(to_ns_, last_t_));
  // Orders still resting are censored at the window end, in submission order (deterministic records).
  std::vector<std::pair<std::uint64_t, std::int64_t>> alive;
  alive.reserve(tracked_.size());
  for (const auto& [id, o] : tracked_) alive.emplace_back(o.submit_line, id);
  std::sort(alive.begin(), alive.end());
  for (const auto& [submit_line, id] : alive) {
    finish(tracked_.find(id), LifetimeEnd::WindowEnd, std::max(to_ns_, last_t_), 0);
  }
}

// ---------------------------------------------------------------- before the replay

// One pass over the message file up to the first message at or after `to_ns`, which it only looks
// at, as the replay does: nothing at or after the window end is read. It finds the first and last
// message times before that bound, whether the bound was reached, and the GCD of the type-1 prices,
// over everything read and inside [from, to).
struct Prescan {
  std::optional<std::int64_t> first_ns, last_ns;
  bool reached_to{false};
  std::int64_t gcd_all{0}, gcd_window{0};
};

Prescan prescan(std::istream& messages, std::int64_t from_ns, std::int64_t to_ns) {
  Prescan p;
  std::string line;
  while (std::getline(messages, line)) {
    const replay::Parsed<LobsterMsg> m = replay::parse_message_line(line);
    if (!m.ok()) continue;
    const std::int64_t t = m.value->time_ns;
    if (t >= to_ns) {
      p.reached_to = true;
      if (!p.first_ns) p.first_ns = t;  // the data starts at or after the window end
      break;
    }
    if (!p.first_ns) p.first_ns = t;
    p.last_ns = t;
    if (m.value->type != 1) continue;
    p.gcd_all = std::gcd(p.gcd_all, m.value->price);
    if (t >= from_ns) p.gcd_window = std::gcd(p.gcd_window, m.value->price);
  }
  if (messages.bad()) throw replay::InputError("read error in the message file");
  return p;
}

Pmf to_pmf(const std::map<std::int64_t, std::uint64_t>& m) {
  Pmf p;
  for (const auto& [v, n] : m) {
    p.values.push_back(v);
    p.counts.push_back(n);
  }
  return p;
}

json optional_number(const std::optional<double>& v) { return v ? json(*v) : json(nullptr); }

}  // namespace

// ---------------------------------------------------------------- estimate

EstimateResult estimate(std::istream& messages, std::istream& orderbook, const InputInfo& info,
                        const EstimateOptions& options) {
  if (info.levels < 1 || info.levels > replay::kMaxLevels) {
    throw std::invalid_argument("the level count must be between 1 and " + std::to_string(replay::kMaxLevels));
  }
  if (options.tick_units && *options.tick_units < 1) throw std::invalid_argument("--tick-units must be at least 1");
  if (options.source && *options.source != "real" && *options.source != "synthetic") {
    throw std::invalid_argument("--source must be real or synthetic");
  }
  const bool given = options.from_ns || options.to_ns;
  const std::int64_t req_from = options.from_ns.value_or(kDefaultFromNs);
  const std::int64_t req_to = options.to_ns.value_or(kDefaultToNs);
  if (req_from < 0 || req_from >= req_to) {
    throw std::invalid_argument("the window must satisfy 0 <= from < to (from " + seconds_text(req_from) + " s, to " +
                                seconds_text(req_to) + " s)");
  }

  EstimateResult result;
  Calibration& c = result.calibration;
  json report = json::object();

  // The window.
  Prescan pre = prescan(messages, req_from, req_to);
  if (!pre.first_ns) throw EstimateError("no message line parses: nothing to estimate");
  std::int64_t from = req_from, to = req_to, gcd = pre.gcd_window;
  const auto span = [](const Prescan& p) {
    return "the data's messages run from " + seconds_text(*p.first_ns) +
           (p.reached_to ? " to past the window end" : " to " + seconds_text(*p.last_ns) + " s after midnight");
  };
  if (!given && *pre.first_ns <= kDefaultFromNs && pre.reached_to) {
    c.window_note = "default window 36000-55800 s after midnight (10:00-15:30, without the open and the close)";
  } else if (!given) {
    // Not covered: the whole span. No window was asked for, so there is no later period to keep
    // unread, and the file is read to its end.
    if (pre.reached_to) {
      messages.clear();
      messages.seekg(0);
      pre = prescan(messages, std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());
    }
    from = *pre.first_ns;
    to = *pre.last_ns + 1;  // [first, last]
    gcd = pre.gcd_all;
    c.window_note = "the default window 36000-55800 s is not covered (" + span(pre) +
                    "), so the data's whole span is used, from its first message to its last, both included";
  } else {
    if (!pre.last_ns) {
      throw EstimateError("the window " + seconds_text(req_from) + "-" + seconds_text(req_to) +
                          " s holds no data: the first message is at " + seconds_text(*pre.first_ns) + " s");
    }
    // Clipped to the data as far as it was read: [first, last] when the file ends inside the window.
    from = std::max(req_from, *pre.first_ns);
    to = pre.reached_to ? req_to : std::min(req_to, *pre.last_ns + 1);
    c.window_note = "given window " + seconds_text(req_from) + "-" + seconds_text(req_to) + " s";
    if (from != req_from || to != req_to) c.window_note += ", clipped to the data (" + span(pre) + ")";
    if (from >= to) {
      throw EstimateError("the window " + seconds_text(req_from) + "-" + seconds_text(req_to) + " s holds no data: " +
                          span(pre));
    }
  }
  c.window_from_ns = from;
  c.window_to_ns = to;
  c.observed_ns = to - from;

  // The tick.
  std::string tick_rule;
  if (options.tick_units) {
    c.tick_units = *options.tick_units;
    tick_rule = "given (--tick-units)";
  } else {
    if (gcd == 0) throw EstimateError("no limit order (type 1) in the window: --tick-units auto has nothing to measure");
    std::string warning;
    c.tick_units = choose_tick(gcd, &warning);
    if (!warning.empty()) result.warnings.push_back(warning);
    tick_rule = "auto: GCD of the type-1 prices in the window " + std::to_string(gcd) +
                (gcd % 100 == 0 ? ", a multiple of 100, so 100" : ", not a multiple of 100, so the GCD itself");
  }

  // The replay, observed inside the window and stopped at its end.
  messages.clear();
  messages.seekg(0);
  if (!messages) throw replay::InputError("the message stream cannot be read a second time");
  Collector col(c.tick_units, from, to, options.keep_lifetimes);
  replay::Replayer replayer(info.levels, replay::ReplayOptions{});
  replayer.observe(&col, from);
  replayer.stop_before(to);
  const replay::Summary& summary = replayer.run(messages, orderbook);

  // Identity and provenance.
  c.source = options.source ? *options.source : (info.readme_synthetic ? "synthetic" : "real");
  c.ticker = info.ticker;
  c.date = info.date;
  c.inputs = info.inputs;
  c.code_commit = build_commit();
  c.levels = info.levels;

  // Median mid: time-weighted lower median of bid + ask, floored to ticks.
  std::int64_t two_sided_ns = 0;
  for (const auto& [m2, w] : col.mid2_ns_) two_sided_ns += w;
  std::optional<std::int64_t> median_mid2;
  std::int64_t cum = 0;
  for (const auto& [m2, w] : col.mid2_ns_) {
    cum += w;
    if (2 * cum >= two_sided_ns && w > 0) {
      median_mid2 = m2;
      break;
    }
  }
  if (!median_mid2) throw EstimateError("no two-sided published book in the window: no median mid-price");
  c.median_mid_ticks = *median_mid2 / (2 * c.tick_units);
  if (c.median_mid_ticks < 1) throw EstimateError("the median mid-price is below one tick");

  // Rates.
  const double seconds = static_cast<double>(c.observed_ns) / static_cast<double>(kNsPerSecond);
  for (int s = 0; s < 2; ++s) {
    c.rates.limit[s] = Rate{col.limit_count_[s], static_cast<double>(col.limit_count_[s]) / seconds};
    c.rates.aggressive[s] = Rate{col.aggressive_count_[s], static_cast<double>(col.aggressive_count_[s]) / seconds};
  }

  // Placement and its fit (report only).
  json fit_report = json::object();
  for (int s = 0; s < 2; ++s) {
    std::map<std::int64_t, std::uint64_t> nonneg;
    for (int b = 0; b < kSpreadBuckets; ++b) {
      c.placement[s][b] = to_pmf(col.placement_[s][b]);
      for (const auto& [d, n] : col.placement_[s][b]) {
        if (d >= 0) nonneg[d] += n;
      }
    }
    c.placement_fit[s] = fit_placement(to_pmf(nonneg));
    const char* name = s == kBuy ? "buy" : "sell";
    if (!c.placement_fit[s]) {
      fit_report[name] = "no fit: fewer than 2 samples with Delta >= 0 or a single value";
    } else if (c.placement_fit[s]->mu <= kFitMuLow + 1e-6 || c.placement_fit[s]->mu >= kFitMuHigh - 1e-6) {
      fit_report[name] =
          "mu reached the end of its search range: the PMF is not power-law shaped on Delta >= 0 (a near-uniform "
          "PMF drives delta0 and mu up together, towards an exponential); the fit describes nothing";
    } else {
      fit_report[name] = "ok";
    }
  }

  // Sizes, with the 99.9th percentile pooling.
  c.limit_size = to_pmf(col.limit_size_);
  c.aggressive_size = to_pmf(col.aggressive_size_);
  json pooling = json::object();
  const auto pool = [&](Pmf& p, std::uint64_t& pooled, const char* name) {
    if (p.empty()) {
      pooling[name] = json{{"samples", 0}};
      return;
    }
    const std::uint64_t n = p.total();
    const std::int64_t top = percentile_999(p);
    pooled = pool_above_999(p);
    pooling[name] = json{{"samples", n}, {"percentile_999", top}, {"pooled", pooled}};
  };
  pool(c.limit_size, c.pooled_limit_size, "limit_size");
  pool(c.aggressive_size, c.pooled_aggressive_size, "aggressive_size");

  // Lifetimes.
  for (int b = 0; b < kDistanceBuckets; ++b) {
    Survival s = kaplan_meier(std::move(col.events_[b]), std::move(col.censored_[b]));
    const Survival& n = col.counts_[b];
    s.orders = n.orders;
    s.cancelled = n.cancelled;
    s.censored_executed = n.censored_executed;
    s.censored_window_end = n.censored_window_end;
    s.censored_left_window = n.censored_left_window;
    c.lifetime[b] = std::move(s);
  }

  // The report: every counter, the sign autocorrelation, the observed time range.
  result.first_message_ns = col.first_ns_;
  result.last_message_ns = col.last_ns_;
  json observed = json::object();
  observed["lines"] = col.lines_;
  observed["first_line"] = col.first_line_;
  observed["last_line"] = col.last_line_;
  observed["first_message_ns"] = col.first_ns_ ? json(*col.first_ns_) : json(nullptr);
  observed["last_message_ns"] = col.last_ns_ ? json(*col.last_ns_) : json(nullptr);
  observed["stopped_by"] = col.end_.reason == replay::StopReason::TimeBound ? "the window end (a message at or after it)"
                                                                           : "the end of the input";
  if (col.end_.reason == replay::StopReason::TimeBound) {
    observed["bound_line"] = col.end_.bound_line;  // read to see the bound, never observed
    observed["bound_time_ns"] = col.end_.bound_time_ns;
  }
  report["observed"] = observed;
  report["window"] = json{{"requested_from_ns", req_from},
                          {"requested_to_ns", req_to},
                          {"given", given},
                          {"data_first_message_ns", *pre.first_ns},
                          {"data_last_message_read_ns", *pre.last_ns},
                          {"data_goes_past_the_window_end", pre.reached_to},
                          {"before_the_window", "replayed (the book needs them) but not observed"},
                          {"at_or_after_the_end", "never read, except the one line that shows where the data "
                                                  "crosses the end (the SHA-256 of the files covers all of them)"}};
  json types = json::object();
  for (int t = 1; t <= 7; ++t) types["type_" + std::to_string(t)] = col.by_type_[t];
  report["messages"] = json{{"by_type", types},
                            {"unparsed", col.unparsed_},
                            {"time_decreased", col.time_decreased_},
                            {"hidden_executions_type_5", col.hidden_},
                            {"cross_trades_type_6", col.cross_},
                            {"halts_type_7", col.halt_}};
  report["tick"] = json{{"tick_units", c.tick_units}, {"rule", tick_rule}, {"subpenny_limit_orders_excluded", col.subpenny_}};
  json excl = json::object();
  std::uint64_t excluded = 0;
  for (const auto& [why, n] : col.placement_excluded_) {
    excl[why] = n;
    excluded += n;
  }
  std::uint64_t placed = 0;
  for (int s = 0; s < 2; ++s) {
    for (int b = 0; b < kSpreadBuckets; ++b) placed += c.placement[s][b].total();
  }
  report["placement"] = json{{"samples", placed}, {"excluded", excl}, {"excluded_total", excluded}, {"fit", fit_report}};
  report["sizes"] = pooling;
  report["aggressive"] = json{{"bursts", col.aggressive_count_[0] + col.aggressive_count_[1]},
                              {"burst_messages", col.bursts_messages_},
                              {"execution_on_line_1_not_in_a_burst", col.execution_line_1_},
                              {"burst_cut_by_the_window", col.burst_partial_},
                              {"size_overflow_excluded", col.aggressive_size_overflow_}};
  report["lifetime"] = json{
      {"tracked_orders", col.counts_[0].orders + col.counts_[1].orders + col.counts_[2].orders + col.counts_[3].orders},
      {"not_tracked_no_placement", excluded},
      {"not_tracked_duplicate_id", col.dup_id_},
      {"id_reused", col.id_reused_},
      {"unknown_id", col.unknown_id_.to_json()},
      {"unknown_id_note", "submitted before the window, a seed order, or a submission the price-window filter dropped"},
      {"after_censoring", col.after_censoring_.to_json()},
      {"to_an_excluded_order", col.to_excluded_.to_json()},
      {"direction_or_price_mismatch_ignored", col.mismatch_.to_json()},
      {"delete_size_mismatch", col.delete_size_mismatch_},
      {"partial_cancels", col.partial_cancels_},
      {"partial_cancel_to_zero_counted_cancelled", col.partial_cancel_to_zero_},
      {"partial_executions", col.partial_executions_},
      {"oversized_execution", col.oversized_execution_},
      {"zero_lifetime_cancellations_set_to_1ns", col.zero_lifetime_clamped_},
      {"rows_not_checked_for_leaving_the_window", col.rows_unchecked_}};
  json medians = json::object();
  for (int b = 0; b < kDistanceBuckets; ++b) {
    const std::optional<std::int64_t> m = c.lifetime[b].median_ns();
    medians[distance_bucket_name(b)] = m ? json(*m) : json(nullptr);
  }
  report["lifetime_median_ns"] = medians;
  report["median_mid"] = json{{"rule",
                               "time-weighted lower median of (best bid + best ask) / 2 over the published rows of the "
                               "window, each row weighted by the time until the next message, floored to ticks"},
                              {"two_sided_ns", two_sided_ns},
                              {"one_sided_ns", col.one_sided_ns_},
                              {"no_usable_row_ns", col.no_row_ns_}};
  report["sign_autocorrelation"] =
      json{{"definition", "aggressive orders (bursts) in event order, +1 buy and -1 sell; c_k / c_0 with "
                          "c_k = (1/n) sum_{i<n-k} (x_i - mean)(x_{i+k} - mean)"},
           {"n", col.signs_.size()},
           {"lag_1", optional_number(sign_autocorrelation(col.signs_, 1))},
           {"lag_10", optional_number(sign_autocorrelation(col.signs_, 10))},
           {"lag_100", optional_number(sign_autocorrelation(col.signs_, 100))}};
  const replay::Counters& rc = summary.counters;
  report["replay"] = json{{"note", "the replay of lines 1.." + std::to_string(summary.lines) +
                                       " (the window and everything before it), resync mode"},
                          {"lines", summary.lines},
                          {"seed_orders", rc.seed_orders},
                          {"seed_attributed", rc.seed_attributed},
                          {"unknown_id", rc.unknown_id},
                          {"dup_id", rc.dup_id},
                          {"submit_would_cross", rc.submit_would_cross},
                          {"direction_mismatch", rc.direction_mismatch},
                          {"price_mismatch", rc.price_mismatch},
                          {"oracle_malformed", rc.oracle_malformed},
                          {"oracle_rejected", rc.oracle_rejected},
                          {"resync_rows", summary.resync.rows}};
  report["known_bias"] =
      "With window-filtered data (real LOBSTER files), submissions placed outside the published window are never "
      "seen, so lambda_L and the far tail of the placement PMF are underestimated.";
  report["warnings"] = result.warnings;
  c.report = std::move(report);
  if (options.keep_lifetimes) {
    result.lifetimes = std::move(col.records_);
    std::stable_sort(result.lifetimes.begin(), result.lifetimes.end(),
                     [](const LifetimeRecord& a, const LifetimeRecord& b) { return a.submit_line < b.submit_line; });
  }
  validate(c);
  return result;
}

EstimateResult estimate_files(const std::string& messages_path, const std::string& orderbook_path,
                              const EstimateOptions& options) {
  namespace fs = std::filesystem;
  const std::optional<replay::LobsterFileName> mname = replay::parse_lobster_filename(messages_path);
  const std::optional<replay::LobsterFileName> oname = replay::parse_lobster_filename(orderbook_path);
  if (!oname || oname->kind != "orderbook") {
    throw replay::InputError("the orderbook file name " + orderbook_path +
                             " is not a LOBSTER name <TICKER>_<DATE>_<START>_<END>_orderbook_<L>.csv");
  }
  if (!mname || mname->kind != "message") {
    throw replay::InputError("the message file name " + messages_path +
                             " is not a LOBSTER name <TICKER>_<DATE>_<START>_<END>_message_<L>.csv");
  }
  if (mname->levels != oname->levels) {
    throw replay::InputError("the message file name says " + std::to_string(mname->levels) +
                             " levels but the orderbook file name says " + std::to_string(oname->levels));
  }
  if (oname->levels > replay::kMaxLevels) {
    throw replay::InputError("the orderbook file name says " + std::to_string(oname->levels) + " levels, at most " +
                             std::to_string(replay::kMaxLevels) + " are accepted");
  }
  InputInfo info;
  info.levels = oname->levels;
  info.ticker = oname->ticker;
  info.date = oname->date;
  try {
    info.inputs.push_back(InputFile{fs::path(messages_path).filename().string(), sha256_file(messages_path)});
    info.inputs.push_back(InputFile{fs::path(orderbook_path).filename().string(), sha256_file(orderbook_path)});
  } catch (const std::runtime_error& e) {
    throw replay::InputError(e.what());
  }
  std::error_code ec;
  info.readme_synthetic = fs::exists(fs::path(messages_path).parent_path() / "README_SYNTHETIC.txt", ec);
  if (!info.readme_synthetic) {
    // A folder of symbolic links has no README of its own: look beside the file the link points to.
    const fs::path canonical = fs::canonical(messages_path, ec);
    if (!ec) info.readme_synthetic = fs::exists(canonical.parent_path() / "README_SYNTHETIC.txt", ec);
  }
  if (const auto name = replay::parse_lobster_filename(messages_path); name && name->ticker.starts_with("SYNTH")) {
    info.readme_synthetic = true;  // a made-up ticker (scripts/make_fake_day.sh)
  }
  std::ifstream messages(messages_path, std::ios::binary);
  if (!messages) throw replay::InputError("cannot read " + messages_path);
  std::ifstream orderbook(orderbook_path, std::ios::binary);
  if (!orderbook) throw replay::InputError("cannot read " + orderbook_path);
  return estimate(messages, orderbook, info, options);
}

}  // namespace calib
