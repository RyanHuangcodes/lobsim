#include "viz_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "sim/order_log.hpp"

namespace viz {

namespace {
using json = nlohmann::ordered_json;
constexpr std::size_t kFullDepth = std::numeric_limits<std::size_t>::max();
constexpr int kSchemaVersion = 1;
}  // namespace

// ---------------------------------------------------------------------------------------
// numeric helpers
// ---------------------------------------------------------------------------------------
std::uint8_t encode_log_u8(double v, double v_max) noexcept {
  if (!(v > 0.0)) return 0;
  if (!(v_max > 0.0)) return 0;
  const double scaled = 255.0 * std::log1p(v) / std::log1p(v_max);
  long rounded = std::lround(scaled);
  if (rounded < 1) rounded = 1;
  if (rounded > 255) rounded = 255;
  return static_cast<std::uint8_t>(rounded);
}

double time_weighted_mean(const std::vector<std::pair<double, TimeNs>>& segments, TimeNs total_ns) noexcept {
  if (total_ns <= 0) return 0.0;
  double area = 0.0;
  for (const auto& [size, dur] : segments) area += size * static_cast<double>(dur);
  return area / static_cast<double>(total_ns);
}

std::int32_t to_i32_checked(std::int64_t v, const char* what) {
  if (v < std::numeric_limits<std::int32_t>::min() || v > std::numeric_limits<std::int32_t>::max())
    throw std::overflow_error(std::string(what) + " value " + std::to_string(v) + " does not fit in Int32");
  return static_cast<std::int32_t>(v);
}

double time_to_f64_checked(TimeNs t) {
  static constexpr std::int64_t kMaxExact = (std::int64_t{1} << 53);
  if (t < 0 || t >= kMaxExact)
    throw std::overflow_error("time " + std::to_string(t) + " is not exactly representable as Float64 (>= 2^53)");
  return static_cast<double>(t);
}

// ---------------------------------------------------------------------------------------
// export driver internals
// ---------------------------------------------------------------------------------------
namespace {

struct Fatal {
  std::string message;
};
[[noreturn]] void fail(const std::string& msg) { throw Fatal{msg}; }

std::string read_file(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) fail("cannot open " + p.string());
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Parse the `fundamental` column of bbo.csv keyed by time_ns. Empty field -> NaN.
std::map<TimeNs, double> read_fundamental(const std::filesystem::path& p) {
  std::map<TimeNs, double> out;
  std::ifstream f(p, std::ios::binary);
  if (!f) fail("cannot open " + p.string());
  std::string line;
  bool header = true;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (header) {
      header = false;
      continue;  // time_ns,bid,bid_qty,ask,ask_qty,mid,fundamental,mm_inventory,mm_pnl
    }
    if (line.empty()) continue;
    // Split on commas; fundamental is column index 6.
    std::vector<std::string> cols;
    std::string cur;
    for (char c : line) {
      if (c == ',') {
        cols.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    cols.push_back(cur);
    if (cols.size() < 7) continue;
    TimeNs t = 0;
    try {
      t = static_cast<TimeNs>(std::stoll(cols[0]));
    } catch (...) {
      continue;
    }
    const std::string& fund = cols[6];
    out[t] = fund.empty() ? std::numeric_limits<double>::quiet_NaN() : std::strtod(fund.c_str(), nullptr);
  }
  return out;
}

// One recorded trade from trades.csv, for the integrity cross-check.
struct CsvTrade {
  TimeNs time{0};
  lob::Price price{0};
  lob::Qty qty{0};
  lob::Side aggressor{lob::Side::Buy};
};

// Parse trades.csv: header time_ns,trade_id,price,qty,aggressor_side,maker_order_id,...
std::vector<CsvTrade> read_trades_csv(const std::filesystem::path& p) {
  std::vector<CsvTrade> out;
  std::ifstream f(p, std::ios::binary);
  if (!f) fail("cannot open " + p.string());
  std::string line;
  bool header = true;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (header) {
      header = false;
      continue;
    }
    if (line.empty()) continue;
    std::vector<std::string> cols;
    std::string cur;
    for (char c : line) {
      if (c == ',') { cols.push_back(cur); cur.clear(); }
      else cur.push_back(c);
    }
    cols.push_back(cur);
    if (cols.size() < 5) fail("malformed trades.csv line: " + line);
    CsvTrade t;
    t.time = static_cast<TimeNs>(std::stoll(cols[0]));
    t.price = static_cast<lob::Price>(std::stoll(cols[2]));
    t.qty = static_cast<lob::Qty>(std::stoll(cols[3]));
    t.aggressor = cols[4] == "buy" ? lob::Side::Buy : lob::Side::Sell;
    out.push_back(t);
  }
  return out;
}

// Every resting level of both sides as (price, total size), written into `out` (cleared first).
// A price is on one side only, because the book never crosses, so each price appears once. The
// heatmap adds each pair into its own cell, so the order of the pairs does not change any sum.
using Snapshot = std::vector<std::pair<lob::Price, double>>;
void full_resting(const lob::OrderBook& book, std::vector<lob::LevelView>& scratch, Snapshot& out) {
  out.clear();
  for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
    book.depth(side, kFullDepth, scratch);
    for (const lob::LevelView& lv : scratch) out.emplace_back(lv.price, static_cast<double>(lv.total_qty));
  }
}

// MM quote prices from the rebuilt book: highest resting buy / lowest resting sell owned by mm.
// Every MM order enters the book through an Ack, so the ids acked to the MM cover all of its
// resting orders; ids whose order is gone are dropped as they are found (D-908).
struct MmQuotes {
  std::optional<lob::Price> bid;
  std::optional<lob::Price> ask;
};
MmQuotes mm_quotes(const lob::OrderBook& book, std::vector<lob::OrderId>& mm_ids) {
  MmQuotes q;
  for (std::size_t i = 0; i < mm_ids.size();) {
    const std::optional<lob::OrderView> o = book.order(mm_ids[i]);
    if (!o) {
      mm_ids[i] = mm_ids.back();
      mm_ids.pop_back();
      continue;
    }
    if (o->side == lob::Side::Buy) q.bid = q.bid ? std::max(*q.bid, o->price) : o->price;
    else q.ask = q.ask ? std::min(*q.ask, o->price) : o->price;
    ++i;
  }
  return q;
}

// One ladder sample: top `levels` per side plus derived top-of-book and MM fields.
struct LadderSample {
  TimeNs time{0};
  std::vector<lob::Price> bid_px;
  std::vector<lob::Qty> bid_sz;
  std::vector<lob::Price> ask_px;
  std::vector<lob::Qty> ask_sz;
  std::optional<lob::Price> best_bid;
  std::optional<lob::Price> best_ask;
  std::optional<double> mid;
  std::optional<lob::Price> spread;
  std::optional<lob::Price> mm_bid;
  std::optional<lob::Price> mm_ask;
  std::int64_t mm_inventory{0};
  double mm_cash{0.0};
  double mm_pnl{0.0};
  double fundamental{std::numeric_limits<double>::quiet_NaN()};
};

struct TradeSample {
  TimeNs time{0};
  lob::Price price{0};
  lob::Qty qty{0};
  lob::Side aggressor{lob::Side::Buy};
};

// Little-endian columnar writer. The host is assumed little-endian (x86-64 here); each append
// keeps the stream 8-byte aligned by construction because every element is 1/4/8 bytes and we
// pad to 8 between arrays.
class BinWriter {
 public:
  std::size_t offset() const { return buf_.size(); }
  void align8() {
    while (buf_.size() % 8 != 0) buf_.push_back(0);
  }
  void f64(double v) {
    std::uint64_t bits;
    std::memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<unsigned char>(bits >> (8 * i)));
  }
  void i32(std::int32_t v) {
    auto u = static_cast<std::uint32_t>(v);
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<unsigned char>(u >> (8 * i)));
  }
  void u8(std::uint8_t v) { buf_.push_back(static_cast<unsigned char>(v)); }
  const std::vector<unsigned char>& bytes() const { return buf_; }

 private:
  std::vector<unsigned char> buf_;
};

// A named columnar array written into data.bin, recorded in meta.json with byte offset + count.
struct SeriesEntry {
  std::string name;
  std::string type;  // "f64" | "i32" | "u8"
  std::size_t offset{0};
  std::size_t count{0};
};

double rel_diff(double a, double b) {
  const double denom = std::max(std::fabs(a), std::fabs(b));
  if (denom == 0.0) return 0.0;
  return std::fabs(a - b) / denom;
}

// The whole rebuild + sampling result, independent of the budget (so the budget loop can rerun
// sampling without re-reading the log).
struct RebuildResult {
  std::uint64_t event_hash{0};
  double mm_pnl_final{0.0};
  bool any{false};  // the log has at least one request
  TimeNs first_time{0};
  TimeNs last_time{0};
  lob::Price min_bid_over_run{0};
  lob::Price max_ask_over_run{0};
  bool saw_bid{false};
  bool saw_ask{false};
  std::vector<TradeSample> trades;  // every trade, exact
};

}  // namespace

// ---------------------------------------------------------------------------------------
// the full export
// ---------------------------------------------------------------------------------------
namespace {

struct Config {
  std::optional<lob::OwnerId> mm_owner;
  double maker_fee_ticks{0.0};
  double taker_fee_ticks{0.0};
  double tick_display{0.01};
  std::uint64_t seed{0};
  std::string summary_hash_hex;
  double summary_pnl_total{0.0};
  bool have_summary_pnl{false};
};

Config read_config(const json& summary) {
  Config c;
  if (!summary.contains("config")) fail("summary.json has no config object");
  const json& cfg = summary.at("config");
  std::uint64_t noise_count = 0;
  if (cfg.contains("noise_traders") && cfg.at("noise_traders").contains("count"))
    noise_count = cfg.at("noise_traders").at("count").get<std::uint64_t>();
  const bool has_informed = cfg.contains("informed") && !cfg.at("informed").is_null();
  const bool has_mm = cfg.contains("mm") && !cfg.at("mm").is_null();
  if (has_mm) {
    c.mm_owner = static_cast<lob::OwnerId>(noise_count + (has_informed ? 1 : 0) + 1);
    const json& mm = cfg.at("mm");
    if (mm.contains("maker_fee_ticks")) c.maker_fee_ticks = mm.at("maker_fee_ticks").get<double>();
    if (mm.contains("taker_fee_ticks")) c.taker_fee_ticks = mm.at("taker_fee_ticks").get<double>();
  }
  if (cfg.contains("tick_display")) c.tick_display = cfg.at("tick_display").get<double>();
  if (summary.contains("seed")) c.seed = summary.at("seed").get<std::uint64_t>();
  if (summary.contains("engine_event_hash")) c.summary_hash_hex = summary.at("engine_event_hash").get<std::string>();
  if (has_mm && summary.contains("market_maker") && summary.at("market_maker").is_object()) {
    const json& wr = summary.at("market_maker");
    if (wr.contains("whole_run") && wr.at("whole_run").contains("pnl_total") &&
        !wr.at("whole_run").at("pnl_total").is_null()) {
      c.summary_pnl_total = wr.at("whole_run").at("pnl_total").get<double>();
      c.have_summary_pnl = true;
    }
  }
  return c;
}

// Build every ladder sample + heatmap, replaying the log once. The hash and the final PnL are
// already known (from a prior rebuild pass) and re-derived here to keep the single source of
// truth in one loop; they are returned for the budget-independent checks in run_export.
struct SampleOutput {
  std::vector<LadderSample> ladder;
  std::vector<std::map<lob::Price, double>> heatmap_cols;
  TimeNs heatmap_t0{0};
  double v_max{0.0};
  double clipped_share{0.0};
  lob::Price row_lo{0};
  lob::Price row_hi{0};
};

// A piecewise-constant book state over [col_start, col_start+dt): a HeatmapAccumulator attributes
// each constant interval of the book to the column(s) it overlaps. The book state only changes
// when a request is applied, so we account the interval [prev_change, next_change) at the state
// in force over it, split at every column boundary.
class HeatmapAccumulator {
 public:
  HeatmapAccumulator(TimeNs t0, TimeNs dt) : t0_(t0), dt_(dt) {}

  // Account the book state `snap` as in force over [from, to). Splits across column boundaries.
  void account(TimeNs from, TimeNs to, const Snapshot& snap) {
    if (to <= from) return;
    TimeNs cursor = from;
    while (cursor < to) {
      const std::int64_t idx = (cursor - t0_) / dt_;
      const TimeNs col_end = t0_ + (idx + 1) * dt_;
      const TimeNs seg_end = std::min(to, col_end);
      const double w = static_cast<double>(seg_end - cursor);
      if (static_cast<std::size_t>(idx) >= columns_.size()) columns_.resize(static_cast<std::size_t>(idx) + 1);
      auto& colmap = columns_[static_cast<std::size_t>(idx)];
      for (const auto& [price, size] : snap) colmap[price] += size * w;
      cursor = seg_end;
    }
  }

  // Finish: divide each column's area by its elapsed width (dt, or the final short tail).
  std::vector<std::map<lob::Price, double>> finish(TimeNs last) {
    std::vector<std::map<lob::Price, double>> out = std::move(columns_);
    for (std::size_t i = 0; i < out.size(); ++i) {
      const TimeNs col_start = t0_ + static_cast<std::int64_t>(i) * dt_;
      TimeNs col_end = col_start + dt_;
      if (col_end > last) col_end = last;  // the last column may be short
      const TimeNs width = col_end > col_start ? col_end - col_start : dt_;
      for (auto& [price, area] : out[i]) area /= static_cast<double>(width);
    }
    return out;
  }

 private:
  TimeNs t0_;
  TimeNs dt_;
  std::vector<std::map<lob::Price, double>> columns_;
};

// Row range = [min_bid - pad, max_ask + pad] over integer ticks, at least one row. Checked
// arithmetic: prices come from the log and pad from the command line.
void row_range(const RebuildResult& rr, std::int64_t pad_ticks, lob::Price& lo, lob::Price& hi) {
  const auto sub = [](lob::Price a, std::int64_t b) {
    lob::Price r;
    if (__builtin_sub_overflow(a, b, &r)) fail("the heatmap price range overflows");
    return r;
  };
  const auto add = [](lob::Price a, std::int64_t b) {
    lob::Price r;
    if (__builtin_add_overflow(a, b, &r)) fail("the heatmap price range overflows");
    return r;
  };
  lo = rr.saw_bid ? sub(rr.min_bid_over_run, pad_ticks) : 0;
  hi = rr.saw_ask ? add(rr.max_ask_over_run, pad_ticks) : 0;
  if (!rr.saw_bid && rr.saw_ask) lo = sub(rr.max_ask_over_run, pad_ticks);
  if (!rr.saw_ask && rr.saw_bid) hi = add(rr.min_bid_over_run, pad_ticks);
  if (lo < 1) lo = 1;
  if (hi < lo) hi = lo;
}

// Saturating arithmetic for size estimates: a wrapped estimate could look small.
std::uint64_t sat_mul(std::uint64_t a, std::uint64_t b) {
  std::uint64_t r;
  return __builtin_mul_overflow(a, b, &r) ? std::numeric_limits<std::uint64_t>::max() : r;
}
std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) {
  std::uint64_t r;
  return __builtin_add_overflow(a, b, &r) ? std::numeric_limits<std::uint64_t>::max() : r;
}

// The number of ladder samples and heatmap columns a sampling pass makes with these steps, from
// the time range alone (D-909). Ladder samples are at first + k * dt for every k with
// first + k * dt <= last. The heatmap covers [first, last), so its last column holds last - 1.
std::uint64_t ladder_count(const RebuildResult& rr, TimeNs dt) {
  return rr.any ? static_cast<std::uint64_t>((rr.last_time - rr.first_time) / dt) + 1 : 0;
}
std::uint64_t heatmap_cols(const RebuildResult& rr, TimeNs dt) {
  return rr.any && rr.last_time > rr.first_time ? static_cast<std::uint64_t>((rr.last_time - 1 - rr.first_time) / dt) + 1 : 0;
}

// estimate_bytes for the given counts.
std::uint64_t estimate_bytes(std::uint64_t L, std::uint64_t rows, std::uint64_t cols, std::uint64_t trades,
                             std::uint64_t levels) {
  // Rough but safe upper bound of data.bin: ladder arrays, trade arrays, heatmap.
  std::uint64_t bytes = sat_mul(L, 8);                    // time f64
  bytes = sat_add(bytes, sat_mul(L, 8 * 4));              // best bid/ask i32 (padded), mid f64, spread i32
  bytes = sat_add(bytes, sat_mul(sat_mul(L, levels), 16)); // ladder px/sz both sides i32
  bytes = sat_add(bytes, sat_mul(L, 8 * 3));              // mm cash/pnl f64 + inventory i32 + fundamental f64
  bytes = sat_add(bytes, sat_mul(L, 4 * 2));              // mm quote bid/ask i32
  bytes = sat_add(bytes, sat_mul(trades, 8 + 4 + 4 + 4)); // trade time f64, price i32, qty i32, side i32
  bytes = sat_add(bytes, sat_mul(rows, cols));            // heatmap u8
  return sat_add(bytes, 4096);                            // slack for alignment padding
}

// Replay once. Always fills `rr` (hash, trades, MM PnL, time range, price extents). With `sample`,
// also produces the ladder samples and the heatmap columns for the given steps.
void rebuild_and_sample(const std::filesystem::path& run_dir, const Config& cfg,
                        const std::map<TimeNs, double>& fundamental, TimeNs ladder_dt, TimeNs heatmap_dt,
                        std::int64_t pad_ticks, std::size_t levels, bool sample, RebuildResult& rr, SampleOutput& so) {
  std::ifstream in(run_dir / "orders.log", std::ios::binary);
  if (!in) fail("cannot open " + (run_dir / "orders.log").string());

  sim::OrderLogReader reader(in);
  lob::OrderBook book(lob::BookConfig{reader.max_price()});
  lob::EventHasher hasher;
  lob::EventBuffer events;

  // MM accounting mirrors src/sim/metrics.cpp record_fill / pnl_now.
  std::int64_t cash = 0;        // ticks
  std::int64_t inventory = 0;   // lots
  double fees = 0.0;            // ticks
  std::optional<double> last_mid;

  const bool have_mm = cfg.mm_owner.has_value();
  const lob::OwnerId mm = have_mm ? *cfg.mm_owner : 0;

  rr = RebuildResult{};
  so = SampleOutput{};

  // Sampling cursors.
  TimeNs next_ladder = 0;
  bool ladder_open = false;
  bool have_first = false;

  // Heatmap: the book state in force since the last change, accumulated per column.
  std::optional<HeatmapAccumulator> heat;
  TimeNs state_since = 0;  // time the current book state took effect
  Snapshot cur_state;      // current resting sizes per price
  std::vector<lob::LevelView> scratch;
  std::vector<lob::OrderId> mm_ids;  // ids acked to the market maker (see mm_quotes)

  auto current_mid = [&]() -> std::optional<double> {
    const auto bb = book.best_bid();
    const auto ba = book.best_ask();
    if (bb && ba) return static_cast<double>(*bb + *ba) / 2.0;
    return std::nullopt;
  };

  auto take_ladder = [&](TimeNs t) {
    LadderSample s;
    s.time = t;
    const auto bids = book.depth(lob::Side::Buy, levels);
    const auto asks = book.depth(lob::Side::Sell, levels);
    for (const auto& lv : bids) {
      s.bid_px.push_back(lv.price);
      s.bid_sz.push_back(lv.total_qty);
    }
    for (const auto& lv : asks) {
      s.ask_px.push_back(lv.price);
      s.ask_sz.push_back(lv.total_qty);
    }
    s.best_bid = book.best_bid();
    s.best_ask = book.best_ask();
    if (s.best_bid && s.best_ask) {
      s.mid = static_cast<double>(*s.best_bid + *s.best_ask) / 2.0;
      s.spread = *s.best_ask - *s.best_bid;
    }
    const MmQuotes q = have_mm ? mm_quotes(book, mm_ids) : MmQuotes{};
    s.mm_bid = q.bid;
    s.mm_ask = q.ask;
    s.mm_inventory = inventory;
    s.mm_cash = static_cast<double>(cash);
    const double mid_for_pnl = last_mid.value_or(0.0);
    s.mm_pnl = static_cast<double>(cash) + static_cast<double>(inventory) * mid_for_pnl - fees;
    // fundamental at or before t.
    double fund = std::numeric_limits<double>::quiet_NaN();
    auto it = fundamental.upper_bound(t);
    if (it != fundamental.begin()) {
      --it;
      fund = it->second;
    }
    s.fundamental = fund;
    so.ladder.push_back(std::move(s));
  };

  while (const auto logged = reader.next()) {
    const TimeNs t = logged->time;
    if (!have_first) {
      have_first = true;
      rr.any = true;
      rr.first_time = t;
      next_ladder = t;
      ladder_open = sample;
      if (sample) heat.emplace(t, heatmap_dt);
      state_since = t;
      cur_state.clear();  // empty book before the first request
    }

    // Emit ladder samples for every scheduled time strictly before this request's time: the
    // state at time u (next_ladder <= u < t) is the state after the previous request.
    while (ladder_open && next_ladder < t) {
      take_ladder(next_ladder);
      next_ladder += ladder_dt;
    }

    // The book was constant over [state_since, t): attribute it to the heatmap before applying.
    if (heat && t > state_since) heat->account(state_since, t, cur_state);

    // Apply the request.
    events.clear();
    lob::apply(book, logged->request, events);
    hasher.add(events);

    // MM accounting + trade capture.
    for (const lob::Event& e : events) {
      if (const auto* a = std::get_if<lob::Ack>(&e); a != nullptr && have_mm && a->owner == mm) mm_ids.push_back(a->order_id);
      if (const auto* tr = std::get_if<lob::Trade>(&e)) {
        rr.trades.push_back(TradeSample{t, tr->price, tr->qty, tr->aggressor_side});
        if (have_mm) {
          if (tr->maker_owner == mm) {
            const lob::Side side = lob::opposite(tr->aggressor_side);
            if (side == lob::Side::Buy) { cash -= tr->price * tr->qty; inventory += tr->qty; }
            else { cash += tr->price * tr->qty; inventory -= tr->qty; }
            fees += static_cast<double>(tr->qty) * cfg.maker_fee_ticks;
          }
          if (tr->taker_owner == mm) {
            const lob::Side side = tr->aggressor_side;
            if (side == lob::Side::Buy) { cash -= tr->price * tr->qty; inventory += tr->qty; }
            else { cash += tr->price * tr->qty; inventory -= tr->qty; }
            fees += static_cast<double>(tr->qty) * cfg.taker_fee_ticks;
          }
        }
      }
    }

    // Track mid + best-bid/ask extents after applying.
    if (const auto m = current_mid()) last_mid = *m;
    if (const auto bb = book.best_bid()) {
      rr.min_bid_over_run = rr.saw_bid ? std::min(rr.min_bid_over_run, *bb) : *bb;
      rr.saw_bid = true;
    }
    if (const auto ba = book.best_ask()) {
      rr.max_ask_over_run = rr.saw_ask ? std::max(rr.max_ask_over_run, *ba) : *ba;
      rr.saw_ask = true;
    }
    rr.last_time = t;

    // The book changed: snapshot the new state as in force from t onward.
    if (sample) full_resting(book, scratch, cur_state);
    state_since = t;
  }

  rr.event_hash = hasher.value();
  rr.mm_pnl_final = have_mm ? static_cast<double>(cash) + static_cast<double>(inventory) * last_mid.value_or(0.0) - fees : 0.0;

  if (!sample) return;

  // Emit a final ladder sample at last_time (so the series spans the full run) and close out
  // the heatmap. The final book state contributes nothing (zero width) at last_time.
  if (have_first) {
    while (ladder_open && next_ladder <= rr.last_time) {
      take_ladder(next_ladder);
      next_ladder += ladder_dt;
    }
    so.heatmap_cols = heat->finish(rr.last_time);
  }
  so.heatmap_t0 = rr.first_time;

  row_range(rr, pad_ticks, so.row_lo, so.row_hi);
  const lob::Price lo = so.row_lo;
  const lob::Price hi = so.row_hi;

  // v_max = max time-weighted mean over cells inside the row range; clipped share = fraction of
  // total resting (time-weighted) mass outside the row range.
  double in_mass = 0.0, out_mass = 0.0, vmax = 0.0;
  for (const auto& colmap : so.heatmap_cols) {
    for (const auto& [price, mean] : colmap) {
      if (price >= lo && price <= hi) {
        in_mass += mean;
        vmax = std::max(vmax, mean);
      } else {
        out_mass += mean;
      }
    }
  }
  so.v_max = vmax;
  const double total_mass = in_mass + out_mass;
  so.clipped_share = total_mass > 0.0 ? out_mass / total_mass : 0.0;
}

// Append a column array of int64 ticks as checked Int32, with missing-> kMissingI32.
void write_i32_series(BinWriter& w, std::vector<SeriesEntry>& series, const std::string& name,
                      const std::vector<std::int32_t>& vals) {
  w.align8();
  series.push_back({name, "i32", w.offset(), vals.size()});
  for (std::int32_t v : vals) w.i32(v);
}
void write_f64_series(BinWriter& w, std::vector<SeriesEntry>& series, const std::string& name,
                      const std::vector<double>& vals) {
  w.align8();
  series.push_back({name, "f64", w.offset(), vals.size()});
  for (double v : vals) w.f64(v);
}
void write_u8_series(BinWriter& w, std::vector<SeriesEntry>& series, const std::string& name,
                     const std::vector<std::uint8_t>& vals) {
  w.align8();
  series.push_back({name, "u8", w.offset(), vals.size()});
  for (std::uint8_t v : vals) w.u8(v);
}

std::int32_t opt_price_i32(const std::optional<lob::Price>& p, const char* what) {
  if (!p) return kMissingI32;
  return to_i32_checked(*p, what);
}

int write_bundle(const ExportOptions& opts, const Config& cfg, const json& summary_config, const RebuildResult& rr,
                 const SampleOutput& so, TimeNs ladder_dt, TimeNs heatmap_dt) {
  BinWriter w;
  std::vector<SeriesEntry> series;
  const std::size_t L = so.ladder.size();
  const bool have_mm = cfg.mm_owner.has_value();

  // --- ladder time axis
  {
    std::vector<double> times(L);
    for (std::size_t i = 0; i < L; ++i) times[i] = time_to_f64_checked(so.ladder[i].time);
    write_f64_series(w, series, "ladder_time_ns", times);
  }
  // --- top-of-book
  {
    std::vector<std::int32_t> bb(L), ba(L), sp(L);
    std::vector<double> mid(L);
    for (std::size_t i = 0; i < L; ++i) {
      const auto& s = so.ladder[i];
      bb[i] = opt_price_i32(s.best_bid, "best_bid");
      ba[i] = opt_price_i32(s.best_ask, "best_ask");
      sp[i] = s.spread ? to_i32_checked(*s.spread, "spread") : kMissingI32;
      mid[i] = s.mid ? *s.mid : std::numeric_limits<double>::quiet_NaN();
    }
    write_i32_series(w, series, "best_bid", bb);
    write_i32_series(w, series, "best_ask", ba);
    write_i32_series(w, series, "spread", sp);
    write_f64_series(w, series, "mid", mid);
  }
  // --- ladder levels
  for (std::size_t lvl = 0; lvl < opts.levels; ++lvl) {
    std::vector<std::int32_t> bp(L), bs(L), ap(L), as(L);
    for (std::size_t i = 0; i < L; ++i) {
      const auto& s = so.ladder[i];
      bp[i] = lvl < s.bid_px.size() ? to_i32_checked(s.bid_px[lvl], "bid_px") : kMissingI32;
      bs[i] = lvl < s.bid_sz.size() ? to_i32_checked(s.bid_sz[lvl], "bid_sz") : kMissingI32;
      ap[i] = lvl < s.ask_px.size() ? to_i32_checked(s.ask_px[lvl], "ask_px") : kMissingI32;
      as[i] = lvl < s.ask_sz.size() ? to_i32_checked(s.ask_sz[lvl], "ask_sz") : kMissingI32;
    }
    const std::string suffix = std::to_string(lvl);
    write_i32_series(w, series, "bid_px_" + suffix, bp);
    write_i32_series(w, series, "bid_sz_" + suffix, bs);
    write_i32_series(w, series, "ask_px_" + suffix, ap);
    write_i32_series(w, series, "ask_sz_" + suffix, as);
  }
  // --- MM series
  {
    std::vector<std::int32_t> mmbid(L), mmask(L), mminv(L);
    std::vector<double> mmcash(L), mmpnl(L);
    for (std::size_t i = 0; i < L; ++i) {
      const auto& s = so.ladder[i];
      if (have_mm) {
        mmbid[i] = opt_price_i32(s.mm_bid, "mm_bid");
        mmask[i] = opt_price_i32(s.mm_ask, "mm_ask");
        mminv[i] = to_i32_checked(s.mm_inventory, "mm_inventory");
        mmcash[i] = s.mm_cash;
        mmpnl[i] = s.mm_pnl;
      } else {
        mmbid[i] = kMissingI32;
        mmask[i] = kMissingI32;
        mminv[i] = kMissingI32;
        mmcash[i] = std::numeric_limits<double>::quiet_NaN();
        mmpnl[i] = std::numeric_limits<double>::quiet_NaN();
      }
    }
    write_i32_series(w, series, "mm_bid", mmbid);
    write_i32_series(w, series, "mm_ask", mmask);
    write_i32_series(w, series, "mm_inventory", mminv);
    write_f64_series(w, series, "mm_cash", mmcash);
    write_f64_series(w, series, "mm_pnl", mmpnl);
  }
  // --- fundamental (sampled on the ladder axis, from bbo.csv)
  {
    std::vector<double> fund(L);
    for (std::size_t i = 0; i < L; ++i) fund[i] = so.ladder[i].fundamental;
    write_f64_series(w, series, "fundamental", fund);
  }
  // --- trades (every trade, exact)
  {
    const std::size_t T = rr.trades.size();
    std::vector<double> tt(T);
    std::vector<std::int32_t> tp(T), tq(T), tsd(T);
    for (std::size_t i = 0; i < T; ++i) {
      tt[i] = time_to_f64_checked(rr.trades[i].time);
      tp[i] = to_i32_checked(rr.trades[i].price, "trade_price");
      tq[i] = to_i32_checked(rr.trades[i].qty, "trade_qty");
      tsd[i] = rr.trades[i].aggressor == lob::Side::Buy ? 0 : 1;
    }
    write_f64_series(w, series, "trade_time_ns", tt);
    write_i32_series(w, series, "trade_price", tp);
    write_i32_series(w, series, "trade_qty", tq);
    write_i32_series(w, series, "trade_aggressor", tsd);
  }
  // --- heatmap (rows x cols, row-major by TIME: column index outer? spec says row-major by time)
  const std::size_t rows = static_cast<std::size_t>(so.row_hi - so.row_lo + 1);
  const std::size_t cols = so.heatmap_cols.size();
  {
    // Stored row-major by time: for each column (time), one row of `rows` bytes, top price first.
    std::vector<std::uint8_t> hm;
    hm.reserve(rows * cols);
    for (std::size_t c = 0; c < cols; ++c) {
      const auto& colmap = so.heatmap_cols[c];
      for (lob::Price p = so.row_hi; p >= so.row_lo; --p) {
        auto it = colmap.find(p);
        const double mean = it == colmap.end() ? 0.0 : it->second;
        hm.push_back(encode_log_u8(mean, so.v_max));
      }
    }
    write_u8_series(w, series, "heatmap", hm);
  }

  // --- meta.json
  json meta = json::object();
  meta["schema_version"] = kSchemaVersion;
  meta["run_name"] = std::filesystem::path(opts.run_dir).filename().string();
  meta["seed"] = cfg.seed;
  meta["config"] = summary_config;
  meta["tick_display"] = cfg.tick_display;
  meta["verified_event_hash"] = cfg.summary_hash_hex;
  meta["time_range_ns"] = {rr.first_time, rr.last_time};
  meta["sampling"] = {{"ladder_dt_ns", ladder_dt},
                      {"heatmap_dt_ns", heatmap_dt},
                      {"levels", opts.levels}};
  meta["ladder_count"] = L;
  meta["trade_count"] = rr.trades.size();
  meta["heatmap"] = {{"rows", rows},
                     {"cols", cols},
                     {"price_hi", so.row_hi},
                     {"price_lo", so.row_lo},
                     {"dt_ns", heatmap_dt},
                     {"t0_ns", so.heatmap_t0},
                     {"v_max", so.v_max},
                     {"pad_ticks", opts.heatmap_pad_ticks},
                     {"clipped_share", so.clipped_share}};
  meta["byte_order"] = "little";
  meta["missing_i32"] = kMissingI32;
  json jseries = json::array();
  for (const auto& e : series)
    jseries.push_back({{"name", e.name}, {"type", e.type}, {"offset", e.offset}, {"count", e.count}});
  meta["series"] = std::move(jseries);
  meta["data_bytes"] = w.bytes().size();

  // --- write files (only now that everything succeeded)
  std::error_code ec;
  std::filesystem::create_directories(opts.out_dir, ec);
  if (ec) fail("cannot create output dir " + opts.out_dir + ": " + ec.message());
  const std::filesystem::path out(opts.out_dir);
  {
    std::ofstream mf(out / "meta.json", std::ios::binary | std::ios::trunc);
    if (!mf) fail("cannot write meta.json");
    mf << meta.dump(2) << "\n";
    mf.flush();
    if (!mf) fail("error writing meta.json");
  }
  {
    std::ofstream bf(out / "data.bin", std::ios::binary | std::ios::trunc);
    if (!bf) fail("cannot write data.bin");
    bf.write(reinterpret_cast<const char*>(w.bytes().data()), static_cast<std::streamsize>(w.bytes().size()));
    bf.flush();
    if (!bf) fail("error writing data.bin");
  }
  std::printf("viz: wrote %s/meta.json and data.bin (%zu bytes); hash %s verified, MM PnL cross-check passed\n",
              opts.out_dir.c_str(), w.bytes().size(), cfg.summary_hash_hex.c_str());
  return 0;
}

}  // namespace

std::vector<LadderSnapshot> sample_ladder(const std::string& orders_log_text, const std::vector<TimeNs>& times,
                                          std::size_t levels) {
  std::istringstream in(orders_log_text);
  sim::OrderLogReader reader(in);
  lob::OrderBook book(lob::BookConfig{reader.max_price()});
  lob::EventBuffer events;

  std::vector<LadderSnapshot> out(times.size());
  for (std::size_t i = 0; i < times.size(); ++i) out[i].time = times[i];

  // Snapshot the book into every requested time whose value is < next request time. We apply
  // requests in order and, right before applying a request at time t, settle every requested
  // time u with current_boundary <= u < t.
  bool have_current = false;
  TimeNs current = 0;
  std::vector<bool> done(times.size(), false);

  auto settle = [&](TimeNs strictly_before) {
    for (std::size_t i = 0; i < times.size(); ++i) {
      if (done[i]) continue;
      if (have_current && times[i] >= current && times[i] < strictly_before) {
        for (const auto& lv : book.depth(lob::Side::Buy, levels)) out[i].bids.emplace_back(lv.price, lv.total_qty);
        for (const auto& lv : book.depth(lob::Side::Sell, levels)) out[i].asks.emplace_back(lv.price, lv.total_qty);
        done[i] = true;
      }
    }
  };

  while (const auto logged = reader.next()) {
    const TimeNs t = logged->time;
    settle(t);
    events.clear();
    lob::apply(book, logged->request, events);
    have_current = true;
    current = t;
  }
  // Settle any remaining requested times at or after the last request's state.
  for (std::size_t i = 0; i < times.size(); ++i) {
    if (done[i]) continue;
    if (have_current && times[i] >= current) {
      for (const auto& lv : book.depth(lob::Side::Buy, levels)) out[i].bids.emplace_back(lv.price, lv.total_qty);
      for (const auto& lv : book.depth(lob::Side::Sell, levels)) out[i].asks.emplace_back(lv.price, lv.total_qty);
    }
  }
  return out;
}

int run_export(const ExportOptions& opts) {
  try {
    const std::filesystem::path run_dir(opts.run_dir);
    const json summary = json::parse(read_file(run_dir / "summary.json"));
    const Config cfg = read_config(summary);
    const std::map<TimeNs, double> fundamental = read_fundamental(run_dir / "bbo.csv");

    // Pass 1: rebuild and verify the hash, the trades and the PnL, without sampling (the steps do
    // not affect them). Its cost is bounded by the size of orders.log.
    TimeNs ladder_dt = opts.ladder_dt_ns;
    TimeNs heatmap_dt = opts.heatmap_dt_ns;
    if (ladder_dt <= 0 || heatmap_dt <= 0) fail("the sampling steps must be positive");

    RebuildResult rr;
    SampleOutput so;
    rebuild_and_sample(run_dir, cfg, fundamental, ladder_dt, heatmap_dt, opts.heatmap_pad_ticks, opts.levels,
                       false, rr, so);

    // Verify engine hash.
    char hexbuf[17];
    std::snprintf(hexbuf, sizeof hexbuf, "%016llx", static_cast<unsigned long long>(rr.event_hash));
    if (cfg.summary_hash_hex != hexbuf) {
      std::fprintf(stderr,
                   "viz: engine hash mismatch: rebuilt %s but summary.json says %s. No bundle written.\n",
                   hexbuf, cfg.summary_hash_hex.c_str());
      return 2;
    }

    // Verify the rebuilt trade stream against the run's trades.csv. The event hash does not cover
    // request timestamps, so a timestamp-only corruption of orders.log can still reproduce the
    // hash; cross-checking every trade's (time, price, qty, aggressor) against the recorded
    // trades.csv catches those, so any corruption that reaches a trade is rejected here.
    {
      const std::vector<CsvTrade> csv = read_trades_csv(run_dir / "trades.csv");
      if (csv.size() != rr.trades.size()) {
        std::fprintf(stderr, "viz: trade count mismatch: rebuilt %zu vs trades.csv %zu. No bundle written.\n",
                     rr.trades.size(), csv.size());
        return 2;
      }
      for (std::size_t i = 0; i < csv.size(); ++i) {
        const auto& a = rr.trades[i];
        const auto& b = csv[i];
        if (a.time != b.time || a.price != b.price || a.qty != b.qty || a.aggressor != b.aggressor) {
          std::fprintf(stderr, "viz: trade %zu mismatch vs trades.csv. No bundle written.\n", i);
          return 2;
        }
      }
    }

    // Verify MM PnL cross-check.
    if (cfg.mm_owner && cfg.have_summary_pnl) {
      const double rd = rel_diff(rr.mm_pnl_final, cfg.summary_pnl_total);
      if (rd > 1e-9) {
        std::fprintf(stderr,
                     "viz: MM PnL mismatch: rebuilt %.9f vs summary %.9f (relative %.3e > 1e-9). No bundle written.\n",
                     rr.mm_pnl_final, cfg.summary_pnl_total, rd);
        return 2;
      }
    }

    // Budget fit: double heatmap-dt first, then ladder-dt. The sample and column counts follow
    // from the time range alone, so the doubling is decided by arithmetic, not by re-sampling,
    // and the one sampling pass below never makes more samples than the budget allows (D-909).
    lob::Price row_lo = 0, row_hi = 0;
    row_range(rr, opts.heatmap_pad_ticks, row_lo, row_hi);
    const auto rows = static_cast<std::uint64_t>(row_hi - row_lo) + 1;
    const auto budget = static_cast<std::uint64_t>(opts.budget_bytes);
    const auto estimate = [&] {
      return estimate_bytes(ladder_count(rr, ladder_dt), rows, heatmap_cols(rr, heatmap_dt), rr.trades.size(),
                            opts.levels);
    };
    // Every time the sampling pass forms (first + k * dt, at most last + dt) must fit in 64 bits,
    // and every time in the bundle must be below 2^53 (BUNDLE.md); check both before sampling.
    if (rr.any && rr.last_time >= (std::int64_t{1} << 53)) {
      fail("time " + std::to_string(rr.last_time) + " is not exactly representable as Float64 (>= 2^53)");
    }
    const TimeNs max_dt = std::numeric_limits<TimeNs>::max() - std::max<TimeNs>(rr.last_time, 0);
    if (ladder_dt > max_dt || heatmap_dt > max_dt) fail("a sampling step is longer than the time range allows");
    const auto twice = [max_dt](TimeNs& dt) {
      if (dt > max_dt / 2) fail("the run cannot fit the size budget");
      dt *= 2;
    };
    for (int guard = 0; guard < 64; ++guard) {
      const std::uint64_t est = estimate();
      if (est <= budget) break;
      // Prefer coarser heatmap; alternate to ladder only after heatmap is clearly dominant.
      const std::uint64_t heatmap_bytes = sat_mul(rows, heatmap_cols(rr, heatmap_dt));
      if (sat_mul(heatmap_bytes, 2) >= est) twice(heatmap_dt);
      else twice(ladder_dt);
    }
    if (estimate() > budget) {
      fail("the run cannot fit the size budget of " + std::to_string(opts.budget_bytes) + " bytes");
    }
    const std::uint64_t want_ladder = ladder_count(rr, ladder_dt), want_cols = heatmap_cols(rr, heatmap_dt);
    rebuild_and_sample(run_dir, cfg, fundamental, ladder_dt, heatmap_dt, opts.heatmap_pad_ticks, opts.levels, true,
                       rr, so);
    if (so.ladder.size() != want_ladder || so.heatmap_cols.size() != want_cols) {
      fail("internal error: the sampling pass made " + std::to_string(so.ladder.size()) + " ladder samples and " +
           std::to_string(so.heatmap_cols.size()) + " heatmap columns, not " + std::to_string(want_ladder) + " and " +
           std::to_string(want_cols));
    }

    return write_bundle(opts, cfg, summary.at("config"), rr, so, ladder_dt, heatmap_dt);
  } catch (const Fatal& f) {
    std::fprintf(stderr, "viz: %s. No bundle written.\n", f.message.c_str());
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "viz: %s. No bundle written.\n", e.what());
    return 2;
  }
}

}  // namespace viz
