// The pass over a LOBSTER message/orderbook pair: per-second sufficient statistics (see
// fingerprint_impl.hpp) and the fingerprint built from them. Estimator definitions are written next to
// the code that implements them and collected in docs/FINGERPRINT.md.
#include "calib/fingerprint.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <istream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "calib/fingerprint_impl.hpp"
#include "replay/lobster.hpp"

namespace calib {
namespace {

constexpr std::int64_t kNs = 1'000'000'000;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

std::int64_t floor_div(std::int64_t a, std::int64_t b) {  // b > 0
  std::int64_t q = a / b;
  if ((a % b != 0) && (a < 0)) --q;
  return q;
}

// The book in force: best prices and spread of a row.
struct State {
  replay::BookRow row;
  bool bid_ok{false}, ask_ok{false};
  std::int64_t bid{0}, ask{0}, spread{0};
};

State make_state(replay::BookRow&& row, std::int64_t tick, std::uint64_t* off_grid) {
  State s;
  s.row = std::move(row);
  s.bid_ok = !s.row.bids.empty() && !s.row.bids[0].empty();
  s.ask_ok = !s.row.asks.empty() && !s.row.asks[0].empty();
  if (s.bid_ok) s.bid = s.row.bids[0].price;
  if (s.ask_ok) s.ask = s.row.asks[0].price;
  if (s.bid_ok && s.ask_ok) {
    const std::int64_t d = s.ask - s.bid;
    if (off_grid != nullptr && d % tick != 0) ++*off_grid;
    s.spread = floor_div(d, tick);
  }
  return s;
}

struct Bin {
  std::int64_t dt{0}, nostate{0}, empty{0}, one{0}, crossed{0}, msgs{0};
  std::vector<double> depth;                              // 2L: side * L + level
  std::vector<std::pair<std::int64_t, std::int64_t>> hist;  // spread (ticks) -> ns
  std::int64_t lim[2]{0, 0}, mkt[2]{0, 0};
};

struct Order {
  int bin;
  std::int64_t size;
  std::int64_t remaining;  // what cancels and fills have left
  int bucket;  // -1: no best price on its side before it
  std::int64_t executed{0};
  bool filled{false};
};

struct Burst {
  int bin;
  int sign;
};

[[noreturn]] void bad(const char* what, std::size_t line, const std::string& why) {
  throw std::runtime_error(std::string(what) + " line " + std::to_string(line) + ": " + why);
}

// Reads the next non-empty line; false at the end of the stream.
bool next_line(std::istream& in, std::string& line, std::size_t& no) {
  while (std::getline(in, line)) {
    ++no;
    if (!line.empty() && line != "\r") return true;
  }
  return false;
}

// A series of values (NaN = missing) -> prefix sums of its channel stats. The values are centered at
// their own mean m over the valid entries; ACF numerators use the pairs (i, i + k) of valid entries.
void fill_series(Channel& ch, const std::vector<double>& x, const std::vector<int>& lags) {
  const std::size_t n = x.size();
  double m = 0;
  std::size_t nv = 0;
  for (double v : x)
    if (std::isfinite(v)) {
      m += v;
      ++nv;
    }
  if (nv > 0) m /= static_cast<double>(nv);
  std::vector<double> c(n, 0.0);
  std::vector<char> ok(n, 0);
  for (std::size_t i = 0; i < n; ++i)
    if (std::isfinite(x[i])) {
      c[i] = x[i] - m;
      ok[i] = 1;
    }
  const std::size_t stats = kSFirstLag + 2 * lags.size();
  ch.pre.assign(stats, std::vector<double>(n + 1, 0.0));
  ch.lag.assign(stats, 0);
  for (std::size_t i = 0; i < n; ++i) {
    ch.pre[kSCount][i + 1] = static_cast<double>(i + 1);
    ch.pre[kSValid][i + 1] = ch.pre[kSValid][i] + (ok[i] ? 1.0 : 0.0);
    const double v = c[i], v2 = v * v;
    ch.pre[kSS1][i + 1] = ch.pre[kSS1][i] + v;
    ch.pre[kSS2][i + 1] = ch.pre[kSS2][i] + v2;
    ch.pre[kSS3][i + 1] = ch.pre[kSS3][i] + v2 * v;
    ch.pre[kSS4][i + 1] = ch.pre[kSS4][i] + v2 * v2;
  }
  for (std::size_t li = 0; li < lags.size(); ++li) {
    const std::size_t k = static_cast<std::size_t>(lags[li]);
    auto& P = ch.pre[kSFirstLag + 2 * li];
    auto& N = ch.pre[kSFirstLag + 2 * li + 1];
    ch.lag[kSFirstLag + 2 * li] = ch.lag[kSFirstLag + 2 * li + 1] = lags[li];
    for (std::size_t i = 0; i < n; ++i) {
      const bool pair = i + k < n && ok[i] && ok[i + k];
      P[i + 1] = P[i] + (pair ? c[i] * c[i + k] : 0.0);
      N[i + 1] = N[i] + (pair ? 1.0 : 0.0);
    }
  }
}

void fill_additive(Channel& ch, const std::vector<std::vector<double>>& per_bin_stats /*[stat][bin]*/, int nb) {
  ch.pre.assign(per_bin_stats.size(), std::vector<double>(static_cast<std::size_t>(nb) + 1, 0.0));
  ch.lag.assign(per_bin_stats.size(), 0);
  for (std::size_t s = 0; s < per_bin_stats.size(); ++s)
    for (int i = 0; i < nb; ++i)
      ch.pre[s][static_cast<std::size_t>(i) + 1] = ch.pre[s][static_cast<std::size_t>(i)] + per_bin_stats[s][static_cast<std::size_t>(i)];
}

}  // namespace

// ---- the pass ----------------------------------------------------------------------------------

Prepared::Prepared(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Prepared::Prepared(Prepared&&) noexcept = default;
Prepared& Prepared::operator=(Prepared&&) noexcept = default;
Prepared::~Prepared() = default;

Prepared Prepared::from_files(const std::string& message_path, const std::string& orderbook_path,
                              std::int64_t tick_units, const FingerprintWindow& window) {
  std::ifstream m(message_path, std::ios::binary), b(orderbook_path, std::ios::binary);
  if (!m) throw std::runtime_error("cannot read " + message_path);
  if (!b) throw std::runtime_error("cannot read " + orderbook_path);
  return from_streams(m, b, tick_units, window);
}

Prepared Prepared::from_streams(std::istream& messages, std::istream& orderbook, std::int64_t tick_units,
                                const FingerprintWindow& window) {
  if (tick_units < 1) throw std::invalid_argument("tick_units must be at least 1");
  auto impl = std::make_unique<Impl>();
  Impl& I = *impl;
  I.tick = tick_units;

  std::optional<std::int64_t> from = window.from_ns;
  std::optional<std::int64_t> to = window.to_ns;
  if (from && to && *to <= *from) throw std::invalid_argument("the window must have to > from");

  std::vector<Bin> bins;
  std::vector<Order> orders;
  std::unordered_map<std::int64_t, std::size_t> order_of_id;  // resting orders submitted in the window
  std::unordered_set<std::int64_t> ended_ids;                 // ids of orders of the window that have ended
  std::vector<Burst> bursts;
  std::array<std::vector<double>, kFingerprintClocks> lm;       // log(ask + bid) on each clock grid
  std::array<std::int64_t, kFingerprintClocks> next_g{};
  std::optional<State> cur;
  std::int64_t pos = 0;
  bool started = false;
  int levels = 0;

  std::string ml, bl;
  std::size_t mno = 0, bno = 0;
  std::int64_t last_time = std::numeric_limits<std::int64_t>::min();
  std::int64_t last_in_window = 0;
  bool any_in_window = false;
  int prev_type = 0, prev_dir = 0;
  std::int64_t prev_time = -1;

  auto bin_at = [&](std::size_t i) -> Bin& {
    if (bins.size() <= i) bins.resize(i + 1);
    return bins[i];
  };
  auto record_grid = [&](std::int64_t limit, bool inclusive) {
    for (int c = 0; c < kFingerprintClocks; ++c) {
      const std::int64_t step = kClockSeconds[static_cast<std::size_t>(c)] * kNs;
      while (inclusive ? next_g[static_cast<std::size_t>(c)] <= limit : next_g[static_cast<std::size_t>(c)] < limit) {
        lm[static_cast<std::size_t>(c)].push_back(cur && cur->bid_ok && cur->ask_ok
                                                      ? std::log(static_cast<double>(cur->ask + cur->bid))
                                                      : kNaN);
        next_g[static_cast<std::size_t>(c)] += step;
      }
    }
  };
  // Accounts [pos, t1) to the state in force, split at second boundaries.
  auto advance = [&](std::int64_t t1, bool final) {
    record_grid(t1, final);
    std::int64_t t0 = pos;
    while (t0 < t1) {
      const std::int64_t b = (t0 - I.from) / kNs;
      const std::int64_t seg_end = std::min(t1, I.from + (b + 1) * kNs);
      const std::int64_t seg = seg_end - t0;
      Bin& bin = bin_at(static_cast<std::size_t>(b));
      bin.dt += seg;
      if (!cur) {
        bin.nostate += seg;
      } else {
        const State& s = *cur;
        if (s.bid_ok && s.ask_ok) {
          bool found = false;
          for (auto& h : bin.hist)
            if (h.first == s.spread) {
              h.second += seg;
              found = true;
              break;
            }
          if (!found) bin.hist.emplace_back(s.spread, seg);
          if (s.ask <= s.bid) bin.crossed += seg;
        } else if (s.bid_ok || s.ask_ok) {
          bin.one += seg;
        } else {
          bin.empty += seg;
        }
        if (bin.depth.empty()) bin.depth.assign(static_cast<std::size_t>(2 * levels), 0.0);
        for (int l = 0; l < levels; ++l) {
          bin.depth[static_cast<std::size_t>(l)] += static_cast<double>(s.row.bids[static_cast<std::size_t>(l)].size) * static_cast<double>(seg);
          bin.depth[static_cast<std::size_t>(levels + l)] += static_cast<double>(s.row.asks[static_cast<std::size_t>(l)].size) * static_cast<double>(seg);
        }
      }
      t0 = seg_end;
    }
    pos = t1;
  };

  while (true) {
    const bool gm = next_line(messages, ml, mno);
    const bool gb = next_line(orderbook, bl, bno);
    if (gm != gb) throw std::runtime_error(std::string("the ") + (gm ? "orderbook" : "message") +
                                           " file ends before the other one (message line " + std::to_string(mno) +
                                           ", orderbook line " + std::to_string(bno) + ")");
    if (!gm) break;
    const auto pm = replay::parse_message_line(ml);
    if (!pm.ok()) bad("message", mno, pm.error);
    if (levels == 0) {
      const auto lv = replay::levels_from_columns(replay::count_fields(bl));
      if (!lv) bad("orderbook", bno, "the number of columns is not a positive multiple of 4");
      levels = *lv;
      I.levels = levels;
    }
    auto pb = replay::parse_book_line(bl, levels);
    if (!pb.ok()) bad("orderbook", bno, pb.error);
    const replay::LobsterMsg msg = *pm.value;
    if (msg.time_ns < last_time) bad("message", mno, "time goes backwards");
    last_time = msg.time_ns;

    if (to && msg.time_ns >= *to) break;
    const int type = msg.type, dir = msg.direction;
    const bool new_burst_run = !(type == 4 && prev_type == 4 && prev_time == msg.time_ns && prev_dir == dir);
    prev_type = type;
    prev_dir = dir;
    prev_time = msg.time_ns;

    if (from && msg.time_ns < *from) {  // carry-in: the book in force when the window opens
      cur = make_state(std::move(*pb.value), tick_units, nullptr);
      continue;
    }
    if (!started) {
      started = true;
      if (!from) from = msg.time_ns;
      I.from = *from;
      pos = I.from;
      for (auto& g : next_g) g = I.from;
    }
    advance(msg.time_ns, false);
    any_in_window = true;
    last_in_window = msg.time_ns;
    const std::size_t bin = static_cast<std::size_t>((msg.time_ns - I.from) / kNs);
    Bin& B = bin_at(bin);
    ++B.msgs;
    if (type == 1) {
      const int side = dir == 1 ? kBuy : kSell;
      ++B.lim[side];
      int bucket = -1;
      if (cur) {
        if (dir == 1 && cur->bid_ok) bucket = distance_bucket(floor_div(cur->bid - msg.price, tick_units));
        if (dir == -1 && cur->ask_ok) bucket = distance_bucket(floor_div(msg.price - cur->ask, tick_units));
      }
      const auto ins = order_of_id.emplace(msg.order_id, orders.size());
      if (ins.second) {
        // The estimator's rule (D-309): an id whose earlier order has ended starts a new order.
        if (ended_ids.erase(msg.order_id) != 0) ++I.reused_ids;
        orders.push_back(Order{static_cast<int>(bin), msg.size, msg.size, bucket});
      } else {
        ++I.duplicate_ids;
      }
    } else if (type == 2 || type == 3 || type == 4) {
      const auto it = order_of_id.find(msg.order_id);
      if (it != order_of_id.end()) {
        Order& o = orders[it->second];
        if (type == 4) {
          o.executed += msg.size;
          o.filled = true;
        }
        o.remaining -= msg.size;
        if (type == 3 || o.remaining <= 0) {
          ended_ids.insert(msg.order_id);
          order_of_id.erase(it);
        }
      }
    }
    if (type == 4) {
      if (new_burst_run) {  // a burst: the aggressor is on the side opposite to the resting order
        const int sign = -dir;
        bursts.push_back(Burst{static_cast<int>(bin), sign});
        ++B.mkt[sign == 1 ? kBuy : kSell];
      }
    }
    cur = make_state(std::move(*pb.value), tick_units, &I.off_grid_rows);
  }
  if (!started) {
    if (!from) throw std::runtime_error("the files hold no message");
    I.from = *from;
    pos = I.from;
    for (auto& g : next_g) g = I.from;
  }
  if (!to) {
    if (!any_in_window) throw std::runtime_error("no message inside the window");
    to = last_in_window + 1;
  }
  I.to = *to;
  if (I.to <= I.from) throw std::runtime_error("the window is empty");
  if (levels == 0) throw std::runtime_error("the files hold no row");
  advance(I.to, true);
  I.nb = static_cast<int>((I.to - I.from + kNs - 1) / kNs);
  const std::size_t nb = static_cast<std::size_t>(I.nb);
  bins.resize(nb);

  // ---- the additive channel
  const std::size_t L = static_cast<std::size_t>(levels);
  for (const Bin& b : bins)
    for (const auto& h : b.hist) I.spread_values.push_back(h.first);
  std::sort(I.spread_values.begin(), I.spread_values.end());
  I.spread_values.erase(std::unique(I.spread_values.begin(), I.spread_values.end()), I.spread_values.end());
  const std::size_t V = I.spread_values.size();
  std::size_t o = 0;
  I.o_dt = o++;
  I.o_nostate = o++;
  I.o_empty = o++;
  I.o_one = o++;
  I.o_crossed = o++;
  I.o_msgs = o++;
  I.o_depth = o;
  o += 2 * L;
  I.o_hist = o;
  o += V;
  I.o_fn = o;
  o += kDistanceBuckets;
  I.o_fk = o;
  o += kDistanceBuckets;
  I.o_nodelta_n = o++;
  I.o_nodelta_k = o++;
  I.o_subsh = o++;
  I.o_exsh = o++;
  I.o_lim = o;
  o += 2;
  I.o_mkt = o;
  o += 2;
  std::vector<std::vector<double>> st(o, std::vector<double>(nb, 0.0));
  for (std::size_t i = 0; i < nb; ++i) {
    const Bin& b = bins[i];
    st[I.o_dt][i] = static_cast<double>(b.dt);
    st[I.o_nostate][i] = static_cast<double>(b.nostate);
    st[I.o_empty][i] = static_cast<double>(b.empty);
    st[I.o_one][i] = static_cast<double>(b.one);
    st[I.o_crossed][i] = static_cast<double>(b.crossed);
    st[I.o_msgs][i] = static_cast<double>(b.msgs);
    for (std::size_t d = 0; d < b.depth.size(); ++d) st[I.o_depth + d][i] = b.depth[d];
    for (const auto& h : b.hist) {
      const auto idx = static_cast<std::size_t>(std::lower_bound(I.spread_values.begin(), I.spread_values.end(), h.first) - I.spread_values.begin());
      st[I.o_hist + idx][i] += static_cast<double>(h.second);
    }
    st[I.o_lim + 0][i] = static_cast<double>(b.lim[0]);
    st[I.o_lim + 1][i] = static_cast<double>(b.lim[1]);
    st[I.o_mkt + 0][i] = static_cast<double>(b.mkt[0]);
    st[I.o_mkt + 1][i] = static_cast<double>(b.mkt[1]);
  }
  for (const Order& r : orders) {
    const std::size_t i = static_cast<std::size_t>(r.bin);
    st[I.o_subsh][i] += static_cast<double>(r.size);
    st[I.o_exsh][i] += static_cast<double>(r.executed);
    if (r.bucket < 0) {
      st[I.o_nodelta_n][i] += 1;
      if (r.filled) st[I.o_nodelta_k][i] += 1;
    } else {
      st[I.o_fn + static_cast<std::size_t>(r.bucket)][i] += 1;
      if (r.filled) st[I.o_fk + static_cast<std::size_t>(r.bucket)][i] += 1;
    }
  }
  I.chans.resize(1 + kSeriesCount);
  {
    Channel& ch = I.chans[0];
    ch.lo.resize(nb + 1);
    ch.hi.resize(nb + 1);
    for (std::size_t i = 0; i <= nb; ++i) ch.lo[i] = ch.hi[i] = static_cast<std::int64_t>(i);
    fill_additive(ch, st, I.nb);
  }
  // ---- the series channels
  std::array<std::vector<double>, kFingerprintClocks> ret;
  std::array<std::int64_t, kFingerprintClocks> nret{};
  for (std::size_t c = 0; c < kFingerprintClocks; ++c) {
    const std::int64_t step = kClockSeconds[c] * kNs;
    nret[c] = (I.to - I.from) / step;
    ret[c].assign(static_cast<std::size_t>(nret[c]), kNaN);
    for (std::size_t j = 0; j < ret[c].size(); ++j)
      if (j + 1 < lm[c].size() && std::isfinite(lm[c][j]) && std::isfinite(lm[c][j + 1])) ret[c][j] = lm[c][j + 1] - lm[c][j];
  }
  I.series_lags[kSerR1] = {1};
  I.series_lags[kSerR10] = {};
  I.series_lags[kSerR60] = {};
  for (int k = 1; k <= kAbsAcfLags; ++k) I.series_lags[kSerAbs10].push_back(k);
  I.series_lags[kSerSign] = {kSignLags.begin(), kSignLags.end()};
  auto returns_channel = [&](Channel& ch, std::size_t c) {
    const std::int64_t sec = kClockSeconds[c];
    ch.lo.resize(nb + 1);
    ch.hi.resize(nb + 1);
    for (std::size_t i = 0; i <= nb; ++i) {
      const std::int64_t ii = static_cast<std::int64_t>(i);
      ch.lo[i] = std::min((ii + sec - 1) / sec, nret[c]);
      ch.hi[i] = i == nb ? nret[c] : std::min(ii / sec, nret[c]);
    }
  };
  for (std::size_t c = 0; c < kFingerprintClocks; ++c) {
    Channel& ch = I.chans[1 + c];
    returns_channel(ch, c);
    fill_series(ch, ret[c], I.series_lags[c]);
  }
  {
    Channel& ch = I.chans[1 + kSerAbs10];
    returns_channel(ch, 1);
    std::vector<double> a(ret[1].size());
    for (std::size_t j = 0; j < a.size(); ++j) a[j] = std::fabs(ret[1][j]);
    fill_series(ch, a, I.series_lags[kSerAbs10]);
  }
  {
    Channel& ch = I.chans[1 + kSerSign];
    ch.lo.assign(nb + 1, 0);
    std::size_t k = 0;
    for (std::size_t i = 0; i <= nb; ++i) {
      while (k < bursts.size() && static_cast<std::size_t>(bursts[k].bin) < i) ++k;
      ch.lo[i] = static_cast<std::int64_t>(k);
    }
    ch.hi = ch.lo;
    std::vector<double> sg(bursts.size());
    for (std::size_t j = 0; j < sg.size(); ++j) sg[j] = bursts[j].sign;
    fill_series(ch, sg, I.series_lags[kSerSign]);
  }
  std::size_t base = 0;
  for (Channel& ch : I.chans) {
    ch.base = base;
    base += ch.pre.size();
  }
  I.acc_size = base;
  return Prepared(std::move(impl));
}

// ---- recombination -----------------------------------------------------------------------------

void Prepared::Impl::add_range(std::vector<double>& acc, int s, int e) const {
  for (const Channel& ch : chans) {
    const std::int64_t a = ch.lo[static_cast<std::size_t>(s)], b = ch.hi[static_cast<std::size_t>(e)];
    for (std::size_t k = 0; k < ch.pre.size(); ++k) {
      const std::int64_t bb = b - ch.lag[k];
      if (bb > a) acc[ch.base + k] += ch.pre[k][static_cast<std::size_t>(bb)] - ch.pre[k][static_cast<std::size_t>(a)];
    }
  }
}

Fingerprint Prepared::Impl::build(const std::vector<double>& acc, std::int64_t from_ns, std::int64_t to_ns) const {
  Fingerprint f;
  const std::size_t L = static_cast<std::size_t>(levels);
  auto A = [&](std::size_t i) { return acc[i]; };
  auto ns = [&](std::size_t i) { return static_cast<std::int64_t>(std::llround(acc[i])); };
  auto cnt = [&](std::size_t i) { return static_cast<std::uint64_t>(std::llround(acc[i])); };
  f.levels = levels;
  f.tick_units = tick;
  f.from_ns = from_ns;
  f.to_ns = to_ns;
  f.total_ns = ns(o_dt);
  f.messages = cnt(o_msgs);
  f.no_state_ns = ns(o_nostate);
  f.empty_ns = ns(o_empty);
  f.one_sided_ns = ns(o_one);
  f.two_sided_ns = f.total_ns - f.no_state_ns - f.empty_ns - f.one_sided_ns;
  f.crossed_ns = ns(o_crossed);
  f.off_grid_rows = off_grid_rows;
  f.duplicate_order_ids = duplicate_ids;
  f.reused_order_ids = reused_ids;
  double wsum = 0, vsum = 0, one_tick = 0;
  for (std::size_t v = 0; v < spread_values.size(); ++v) {
    const std::int64_t h = ns(o_hist + v);
    if (h <= 0) continue;
    f.spread_pmf.values.push_back(spread_values[v]);
    f.spread_pmf.counts.push_back(static_cast<std::uint64_t>(h));
    wsum += static_cast<double>(h);
    vsum += static_cast<double>(spread_values[v]) * static_cast<double>(h);
    if (spread_values[v] == 1) one_tick += static_cast<double>(h);
  }
  f.spread_mean = wsum > 0 ? vsum / wsum : kNaN;
  f.spread_one_tick_share = wsum > 0 ? one_tick / wsum : kNaN;
  f.one_sided_share = f.total_ns > 0 ? static_cast<double>(f.one_sided_ns) / static_cast<double>(f.total_ns) : kNaN;
  const double with_book = static_cast<double>(f.total_ns - f.no_state_ns);
  for (int side = 0; side < 2; ++side) {
    f.depth[static_cast<std::size_t>(side)].resize(L);
    for (std::size_t l = 0; l < L; ++l)
      f.depth[static_cast<std::size_t>(side)][l] = with_book > 0 ? A(o_depth + static_cast<std::size_t>(side) * L + l) / with_book : kNaN;
  }
  // The sample moments of a series from its block sums (sums of values centered at the whole-window mean).
  auto series = [&](int id) {
    struct S { double cnt, n, s1, s2, s3, s4; };
    const std::size_t b = chans[static_cast<std::size_t>(1 + id)].base;
    return S{A(b + kSCount), A(b + kSValid), A(b + kSS1), A(b + kSS2), A(b + kSS3), A(b + kSS4)};
  };
  auto acf = [&](int id, std::size_t lag_index) {
    const std::size_t b = chans[static_cast<std::size_t>(1 + id)].base;
    const auto s = series(id);
    const double P = A(b + kSFirstLag + 2 * lag_index), N = A(b + kSFirstLag + 2 * lag_index + 1);
    if (N <= 0 || s.n <= 0 || s.s2 <= 0) return kNaN;
    return (P / N) / (s.s2 / s.n);
  };
  for (std::size_t c = 0; c < kFingerprintClocks; ++c) {
    const auto s = series(static_cast<int>(c));
    VolStat& v = f.vol[c];
    v.clock_s = kClockSeconds[c];
    v.n = static_cast<std::uint64_t>(std::llround(s.n));
    v.skipped = static_cast<std::uint64_t>(std::llround(s.cnt - s.n));
    v.stdev = s.n >= 2 ? std::sqrt(std::max(0.0, (s.s2 - s.s1 * s.s1 / s.n) / (s.n - 1))) : kNaN;
  }
  {
    const auto s = series(kSerR10);
    f.kurtosis_10s = kNaN;
    if (s.n >= 2) {
      const double m = s.s1 / s.n, e2 = s.s2 / s.n, e3 = s.s3 / s.n, e4 = s.s4 / s.n;
      const double m2 = e2 - m * m;
      const double m4 = e4 - 4 * m * e3 + 6 * m * m * e2 - 3 * m * m * m * m;
      if (m2 > 0) f.kurtosis_10s = m4 / (m2 * m2) - 3.0;
    }
  }
  f.acf_ret_1s_lag1 = acf(kSerR1, 0);
  for (std::size_t k = 0; k < kAbsAcfLags; ++k) f.acf_abs_10s[k] = acf(kSerAbs10, k);
  for (std::size_t k = 0; k < kSignAcfLags; ++k) f.sign_acf[k] = acf(kSerSign, k);
  f.bursts = static_cast<std::uint64_t>(std::llround(series(kSerSign).cnt));
  std::uint64_t all_n = 0, all_k = 0;
  for (std::size_t b = 0; b < kDistanceBuckets; ++b) {
    f.fill[b].orders = cnt(o_fn + b);
    f.fill[b].filled = cnt(o_fk + b);
    f.fill[b].prob = f.fill[b].orders ? static_cast<double>(f.fill[b].filled) / static_cast<double>(f.fill[b].orders) : kNaN;
    all_n += f.fill[b].orders;
    all_k += f.fill[b].filled;
  }
  f.placement_unknown = cnt(o_nodelta_n);
  all_n += f.placement_unknown;
  all_k += cnt(o_nodelta_k);
  f.fill_all = FillStat{all_n, all_k, all_n ? static_cast<double>(all_k) / static_cast<double>(all_n) : kNaN};
  f.submitted_shares = cnt(o_subsh);
  f.executed_shares = cnt(o_exsh);
  f.executed_share = f.submitted_shares ? static_cast<double>(f.executed_shares) / static_cast<double>(f.submitted_shares) : kNaN;
  const double secs = static_cast<double>(f.total_ns) / 1e9;
  for (std::size_t s = 0; s < 2; ++s) {
    f.lambda_limit[s] = secs > 0 ? A(o_lim + s) / secs : kNaN;
    f.lambda_market[s] = secs > 0 ? A(o_mkt + s) / secs : kNaN;
  }
  f.lambda_limit_total = secs > 0 ? (A(o_lim) + A(o_lim + 1)) / secs : kNaN;
  f.lambda_market_total = secs > 0 ? (A(o_mkt) + A(o_mkt + 1)) / secs : kNaN;
  return f;
}

Fingerprint Prepared::Impl::of_bins(int first, int last) const {
  std::vector<double> acc(acc_size, 0.0);
  add_range(acc, first, last);
  const std::int64_t f = from + static_cast<std::int64_t>(first) * kNs;
  const std::int64_t t = std::min(to, from + static_cast<std::int64_t>(last) * kNs);
  return build(acc, f, t);
}

int Prepared::bins() const { return impl_->nb; }
Fingerprint Prepared::fingerprint() const { return impl_->of_bins(0, impl_->nb); }
Fingerprint Prepared::fingerprint_of_bins(int first, int last) const {
  if (first < 0 || last > impl_->nb || first >= last) throw std::invalid_argument("bad bin range");
  return impl_->of_bins(first, last);
}

Fingerprint compute_fingerprint(std::istream& m, std::istream& b, std::int64_t tick_units, const FingerprintWindow& w) {
  return Prepared::from_streams(m, b, tick_units, w).fingerprint();
}
Fingerprint compute_fingerprint(const std::string& mp, const std::string& bp, std::int64_t tick_units,
                                const FingerprintWindow& w) {
  return Prepared::from_files(mp, bp, tick_units, w).fingerprint();
}

}  // namespace calib
