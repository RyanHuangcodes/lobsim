// lobsim_export_lobster: turn a simulator run into a LOBSTER message file and orderbook file.
//
//   lobsim_export_lobster --run <sim output dir> --levels L [--filter-window] --out <dir> [--ticker NAME]
//
// Reads <run>/orders.log, applies every request to an OrderBook, and turns the request's events
// into LOBSTER messages in event order (external id = engine OrderId, Direction = side of the
// order the message is about, which for an execution is the resting order):
//   Trade                          -> 4 on the maker: its qty, the trade price
//   Cancelled of a resting order   -> 3 with qty_cancelled (decided by whether the order rests in
//                                     the mirror book, not by the reason)
//   Modify that keeps priority     -> 2 with the reduction
//   Modify that loses priority     -> 3 for the old remaining first; then its 4s; then 1 if it rests
//   new order that rests           -> 1 with its resting qty, after its 4s
// A mirror book, driven only by insert_passive / reduce / remove, applies every message; the
// orderbook row after a message is the mirror's top L (empty levels: price 9999999999 for asks,
// -9999999999 for bids, size 0). After each request the mirror must equal the engine's book at
// every level, or the export stops with an error.
//
// --filter-window imitates LOBSTER's price-range filter: a message is written only if its price
// is inside the top-L window of the true book on its side before or after it (window: every price
// at least as good as the L-th level, the whole side if it has fewer than L levels). Rows show the
// true book after each written message.
//
// Times: 34200 s (9:30) plus the simulated time. Exit codes: 0 done, 1 bad arguments or an error.
//
// --lobster-like [--window-start-s S] [--hidden-rate R] [--halt-at-s H] [--seed N] makes the
// files look like a real LOBSTER day (all quirks together; --filter-window is switched on):
//   * only messages at sim time >= S (default 300) are written; the book built before S keeps
//     resting, so row 0 shows liquidity no message created;
//   * a Poisson stream (R per second, default 0.2) of type-5 messages with ids from
//     kHiddenIdBase up, a price between the true best bid and best ask inclusive, size 1-100, a
//     random direction; its row repeats the previous row;
//   * one type-7 message at sim time H (default: halfway through the window): price -1, size 0,
//     id 0, direction 1, row repeated (our own stand-in encoding of a halt);
//   * time = 34200 s + (sim time - S), 9 decimals; empty levels are price 0, size 0.
// It also writes README_SYNTHETIC.txt and <ticker>_injected.json (the injected counts) and
// refuses to write into a directory named data/lobster. To isolate one quirk at a time,
// --no-filter-window keeps every message, --no-halt leaves the halt out and --hidden-rate 0 the
// hidden executions (all three only with --lobster-like).
//
// --price-scale N (default 1) multiplies every real price written (messages and orderbook rows) by N, so
// simulator prices land in the units of the data they are compared with (N = the data's tick in LOBSTER
// price units). The empty-level placeholders (9999999999, -9999999999, 0) and the halt's price -1 are not
// scaled. An N for which the largest price the run can reach, times N, would reach 9999999999 is refused.
// --audit lines keep the simulator's own (unscaled) prices.
//
// --audit <file> (off by default) writes the exporter's ground truth: one CSV line per message it
// derived, written or not, in order: seq,sim_time_ns,type,id,size,price,direction,status,line.
// status is written, filtered (outside the window before and after), before_window or injected
// (a type 5 or 7 of --lobster-like); line is the message file line of a written or injected
// message, and for the others the number of lines written before it.
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "replay/lobster.hpp"
#include "sim/order_log.hpp"
#include "sim/rng.hpp"

namespace {

constexpr std::int64_t kOpenNs = 34'200'000'000'000;  // 9:30:00 in nanoseconds after midnight
constexpr std::size_t kAllLevels = std::numeric_limits<std::size_t>::max();
constexpr std::int64_t kEmptyAsk = 9'999'999'999;
constexpr std::int64_t kEmptyBid = -9'999'999'999;
constexpr std::int64_t kNsPerSecond = 1'000'000'000;
// Ids of hidden executions: 2^40 + 0, 1, 2, ... The simulator's own order ids are small counters,
// so the export stops with an error if one ever reaches this range.
constexpr std::int64_t kHiddenIdBase = std::int64_t{1} << 40;

// Everything --lobster-like adds (spec D3). All times are simulator nanoseconds.
struct LikeOptions {
  bool on{false};
  std::int64_t window_start_ns{300 * kNsPerSecond};
  double hidden_rate{0.2};
  std::optional<std::int64_t> halt_at_ns;  // default: halfway through the window
  bool halt{true};                          // false: --no-halt
  std::uint64_t seed{20120621};
};

int usage(const std::string& why) {
  std::fprintf(stderr, "lobsim_export_lobster: %s\n", why.c_str());
  std::fprintf(stderr,
               "usage: lobsim_export_lobster --run <sim output dir> --levels L [--filter-window] --out <dir> "
               "[--ticker NAME]\n"
               "       [--lobster-like [--window-start-s S] [--hidden-rate R] [--halt-at-s H] [--seed N]\n"
               "                       [--no-filter-window] [--no-halt]] [--audit FILE] [--price-scale N]\n");
  return 1;
}

class Exporter {
 public:
  Exporter(lob::Price max_price, int levels, bool filter, std::ostream& messages, std::ostream& orderbook,
           const LikeOptions& like = {}, std::int64_t window_end_ns = 0, std::ostream* audit = nullptr,
           std::int64_t price_scale = 1)
      : engine_(lob::BookConfig{max_price}),
        mirror_(lob::BookConfig{max_price}),
        levels_(static_cast<std::size_t>(levels)),
        filter_(filter),
        messages_(messages),
        orderbook_(orderbook),
        like_(like),
        rng_(like.seed),
        audit_(audit),
        price_scale_(price_scale) {
    if (audit_ != nullptr) *audit_ << "seq,sim_time_ns,type,id,size,price,direction,status,line\n";
    if (like_.on) {
      halt_done_ = !like_.halt;
      halt_ns_ = like_.halt_at_ns ? *like_.halt_at_ns
                                  : like_.window_start_ns + (window_end_ns - like_.window_start_ns) / 2;
      window_end_ns_ = window_end_ns;
      schedule_hidden(like_.window_start_ns);
    }
  }

  // Emits the injected messages (hidden executions, the halt) due before the end of the run.
  void finish() {
    if (like_.on) inject_until(std::numeric_limits<std::int64_t>::max());
  }

  void request(const sim::LoggedRequest& r) {
    ++requests_;
    if (like_.on) inject_until(r.time - 1);  // injected messages at the same instant go after the real ones
    time_ = r.time;
    events_.clear();
    lob::apply(engine_, r.request, events_);
    std::optional<lob::OrderId> subject;  // the order this request may leave resting
    for (const lob::Event& ev : events_) {
      if (const auto* a = std::get_if<lob::Ack>(&ev)) {
        subject = a->order_id;
      } else if (const auto* m = std::get_if<lob::Modified>(&ev)) {
        const lob::OrderView o = resting(m->order_id, "a modified order");
        if (m->priority_kept) {
          emit(2, m->order_id, o.remaining - m->new_qty, o.price, o.side);
        } else {
          emit(3, m->order_id, o.remaining, o.price, o.side);
          subject = m->order_id;
        }
      } else if (const auto* t = std::get_if<lob::Trade>(&ev)) {
        const lob::OrderView o = resting(t->maker_order_id, "a maker");
        if (o.price != t->price) fail("trade " + std::to_string(t->trade_id) + " is not at its maker's price");
        emit(4, t->maker_order_id, t->qty, t->price, o.side);
      } else if (const auto* c = std::get_if<lob::Cancelled>(&ev)) {
        if (const auto o = mirror_order(c->order_id)) {
          if (o->remaining != c->qty_cancelled) {
            fail("order " + std::to_string(c->order_id) + " is cancelled for " + std::to_string(c->qty_cancelled) +
                 " but has " + std::to_string(o->remaining) + " resting");
          }
          emit(3, c->order_id, c->qty_cancelled, o->price, o->side);
        }
      }
    }
    if (subject && !mirror_order(*subject)) {
      if (const auto o = engine_.order(*subject)) emit(1, *subject, o->remaining, o->price, o->side);
    }
    for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
      if (engine_.depth(side, kAllLevels) != mirror_.depth(side, kAllLevels)) {
        fail(std::string("after this request the exported book differs from the engine's on the ") +
             (side == lob::Side::Buy ? "bid" : "ask") + " side");
      }
    }
  }

  std::uint64_t requests() const { return requests_; }
  std::uint64_t written() const { return written_; }
  std::uint64_t dropped() const { return dropped_; }
  const std::uint64_t* by_type() const { return by_type_; }
  std::uint64_t hidden_written() const { return hidden_written_; }
  std::uint64_t hidden_skipped() const { return hidden_skipped_; }
  std::uint64_t halts_written() const { return halts_written_; }
  std::uint64_t before_window() const { return before_window_; }
  std::int64_t halt_ns() const { return halt_ns_; }

 private:
  // The next hidden execution after `from_ns`: an exponential gap in whole nanoseconds. The draw is
  // the only floating point; it never reaches a file.
  void schedule_hidden(std::int64_t from_ns) {
    if (like_.hidden_rate <= 0.0) {
      next_hidden_ns_ = std::numeric_limits<std::int64_t>::max();
      return;
    }
    const double gap = rng_.exponential(like_.hidden_rate) * 1e9;
    const std::int64_t gap_ns = gap >= 4e18 ? std::int64_t{4'000'000'000'000'000'000} : static_cast<std::int64_t>(gap);
    next_hidden_ns_ = from_ns + gap_ns;
  }

  // Writes every pending injected message with time <= `limit_ns` (and inside the window), in time
  // order; a hidden execution goes before the halt if they share an instant.
  void inject_until(std::int64_t limit_ns) {
    for (;;) {
      const bool hidden_due = next_hidden_ns_ <= limit_ns && next_hidden_ns_ <= window_end_ns_;
      const bool halt_due = !halt_done_ && halt_ns_ <= limit_ns && halt_ns_ <= window_end_ns_;
      if (!hidden_due && !halt_due) return;
      if (halt_due && (!hidden_due || halt_ns_ < next_hidden_ns_)) {
        halt_done_ = true;
        write_injected(halt_ns_, 7, 0, 0, -1, 1);
        ++halts_written_;
      } else {
        const std::int64_t t = next_hidden_ns_;
        schedule_hidden(t);
        const std::int64_t size = rng_.uniform_int(1, 100);
        const int direction = rng_.uniform_int(0, 1) == 0 ? -1 : 1;
        const std::vector<lob::LevelView> ask = mirror_.depth(lob::Side::Sell, 1);
        const std::vector<lob::LevelView> bid = mirror_.depth(lob::Side::Buy, 1);
        if (ask.empty() || bid.empty()) {
          ++hidden_skipped_;
          continue;
        }
        const lob::Price price = rng_.uniform_int(bid.front().price, ask.front().price);
        write_injected(t, 5, kHiddenIdBase + static_cast<std::int64_t>(hidden_written_), size, price, direction);
        ++hidden_written_;
      }
    }
  }

  void write_injected(std::int64_t at_ns, int type, std::int64_t id, std::int64_t size, std::int64_t price,
                      int direction) {
    time_ = at_ns;
    const replay::LobsterMsg m{kOpenNs + (at_ns - like_.window_start_ns), type, id,
                               size, type == 7 ? price : price * price_scale_, direction};
    messages_ << replay::format_message_line(m) << '\n';
    if (last_row_.empty()) last_row_ = book_row();
    orderbook_ << last_row_ << '\n';
    ++written_;
    ++by_type_[type];
    write_audit(type, id, size, price, direction, "injected");
  }

  // One line of --audit: the message and what became of it (see the file comment).
  void write_audit(int type, std::int64_t id, std::int64_t size, std::int64_t price, int direction,
                   const char* status) {
    if (audit_ == nullptr) return;
    *audit_ << audit_seq_++ << ',' << time_ << ',' << type << ',' << id << ',' << size << ',' << price << ','
            << direction << ',' << status << ',' << written_ << '\n';
  }

  std::string book_row() const {
    const std::vector<lob::LevelView> asks = mirror_.depth(lob::Side::Sell, levels_);
    const std::vector<lob::LevelView> bids = mirror_.depth(lob::Side::Buy, levels_);
    const std::string empty_ask = like_.on ? "0,0" : std::to_string(kEmptyAsk) + ",0";
    const std::string empty_bid = like_.on ? "0,0" : std::to_string(kEmptyBid) + ",0";
    std::string row;
    for (std::size_t i = 0; i < levels_; ++i) {
      if (i > 0) row += ',';
      row += i < asks.size() ? std::to_string(asks[i].price * price_scale_) + ',' + std::to_string(asks[i].total_qty)
                             : empty_ask;
      row += ',';
      row += i < bids.size() ? std::to_string(bids[i].price * price_scale_) + ',' + std::to_string(bids[i].total_qty)
                             : empty_bid;
    }
    return row;
  }

  [[noreturn]] void fail(const std::string& what) const {
    throw std::runtime_error("request " + std::to_string(requests_) + " (time " + std::to_string(time_) +
                             " ns): " + what);
  }

  std::optional<lob::OrderView> mirror_order(lob::OrderId engine_id) const {
    const auto it = mirror_id_.find(engine_id);
    if (it == mirror_id_.end()) return std::nullopt;
    return mirror_.order(it->second);
  }

  lob::OrderView resting(lob::OrderId engine_id, const char* what) const {
    const auto o = mirror_order(engine_id);
    if (!o) fail(std::string(what) + ", order " + std::to_string(engine_id) + ", is not resting in the exported book");
    return *o;
  }

  // Inside the true book's top-L window on `side` (see the file comment).
  bool in_window(lob::Side side, lob::Price price) const {
    const std::vector<lob::LevelView> top = mirror_.depth(side, levels_);
    if (top.size() < levels_) return true;
    return side == lob::Side::Sell ? price <= top.back().price : price >= top.back().price;
  }

  void emit(int type, lob::OrderId id, lob::Qty size, lob::Price price, lob::Side side) {
    const bool before = !filter_ || in_window(side, price);
    apply_to_mirror(type, id, size, price, side);
    const int direction = side == lob::Side::Buy ? 1 : -1;
    if (like_.on && time_ < like_.window_start_ns) {
      ++before_window_;
      write_audit(type, static_cast<std::int64_t>(id), size, price, direction, "before_window");
      return;
    }
    const bool keep = before || in_window(side, price);
    if (!keep) {
      ++dropped_;
      write_audit(type, static_cast<std::int64_t>(id), size, price, direction, "filtered");
      return;
    }
    if (like_.on && static_cast<std::int64_t>(id) >= kHiddenIdBase) fail("order id " + std::to_string(id) + " is in the hidden-id range");
    const std::int64_t shown_ns = like_.on ? kOpenNs + (time_ - like_.window_start_ns) : kOpenNs + time_;
    const replay::LobsterMsg m{shown_ns, type, static_cast<std::int64_t>(id), size, price * price_scale_, direction};
    messages_ << replay::format_message_line(m) << '\n';
    last_row_ = book_row();
    orderbook_ << last_row_ << '\n';
    ++written_;
    ++by_type_[type];
    write_audit(type, static_cast<std::int64_t>(id), size, price, direction, "written");
  }

  void apply_to_mirror(int type, lob::OrderId id, lob::Qty size, lob::Price price, lob::Side side) {
    if (type == 1) {
      const lob::Result<lob::OrderId> r = mirror_.insert_passive(side, price, size);
      if (!r.ok()) fail("type 1 for order " + std::to_string(id) + ": insert_passive returned " + lob::to_string(r.status));
      mirror_id_[id] = r.value;
      return;
    }
    const auto it = mirror_id_.find(id);
    if (it == mirror_id_.end()) fail("type " + std::to_string(type) + " for order " + std::to_string(id) + ", which is not resting");
    const lob::Status st = type == 3 ? mirror_.remove(it->second) : mirror_.reduce(it->second, size);
    if (st != lob::Status::Ok) {
      fail("type " + std::to_string(type) + " for order " + std::to_string(id) + " returned " + lob::to_string(st));
    }
    if (!mirror_.order(it->second)) mirror_id_.erase(it);
  }

  lob::OrderBook engine_;
  lob::OrderBook mirror_;
  std::unordered_map<lob::OrderId, lob::OrderId> mirror_id_;  // engine id -> mirror id, resting orders only
  std::size_t levels_;
  bool filter_;
  std::ostream& messages_;
  std::ostream& orderbook_;
  lob::EventBuffer events_;
  sim::TimeNs time_{0};
  std::uint64_t requests_{0};
  std::uint64_t written_{0};
  std::uint64_t dropped_{0};
  std::uint64_t by_type_[8]{};
  LikeOptions like_;
  sim::Xoshiro256StarStar rng_;
  std::string last_row_;
  std::int64_t halt_ns_{0};
  std::int64_t window_end_ns_{0};
  std::int64_t next_hidden_ns_{0};
  bool halt_done_{false};
  std::uint64_t hidden_written_{0};
  std::uint64_t hidden_skipped_{0};
  std::uint64_t halts_written_{0};
  std::uint64_t before_window_{0};
  std::ostream* audit_;
  std::int64_t price_scale_;
  std::uint64_t audit_seq_{0};
};

void write_synthetic_notes(const std::filesystem::path& dir, const std::string& ticker, int levels, bool filter,
                           const LikeOptions& like, std::int64_t window_end_ns, const Exporter& e) {
  const auto seconds = [](std::int64_t ns) {  // integer math, 9 decimals
    const std::string f = std::to_string(ns % kNsPerSecond);
    return std::to_string(ns / kNsPerSecond) + "." + std::string(9 - f.size(), '0') + f;
  };
  const std::string window_s = seconds(window_end_ns - like.window_start_ns);
  const auto u = [](std::uint64_t v) { return std::to_string(v); };
  std::ofstream txt(dir / "README_SYNTHETIC.txt", std::ios::binary | std::ios::trunc);
  txt << "SYNTHETIC DATA - NOT MARKET DATA\n"
         "================================\n"
         "These files were generated by lobsim_export_lobster --lobster-like from a simulator run.\n"
         "Ticker " << ticker << " is a made-up name and the date in the file names is only a format placeholder.\n"
         "Do not treat any number computed from them as a result about a real market.\n\n"
         "Generator: lobsim_export_lobster --run <sim output dir> --levels " << levels << " --lobster-like"
         " --window-start-s " << like.window_start_ns / kNsPerSecond << " --hidden-rate " << like.hidden_rate
      << (like.halt_at_ns ? " --halt-at-s " + std::to_string(*like.halt_at_ns / kNsPerSecond) : std::string())
      << " --seed " << like.seed << (filter ? "" : " --no-filter-window") << (like.halt ? "" : " --no-halt")
      << " --ticker " << ticker << " --out <dir>\n\n"
         "Conventions (our own choices; real LOBSTER files may differ):\n"
         "  * Empty levels in the orderbook file are price 0 and size 0.\n"
         "  * A halt is one type-7 message: price -1, size 0, id 0, direction 1; its orderbook row repeats the\n"
         "    previous row.\n"
         "  * Hidden executions are type-5 messages with ids " << kHiddenIdBase << " and up (2^40 + k, k = 0, 1, ...),\n"
         "    never used by any other message; the price is between the best bid and best ask of the true book,\n"
         "    the size 1-100, the direction random; the orderbook row repeats the previous row.\n"
         "  * Times are 34200 s plus the seconds since the window start, 9 decimals.\n"
         "  * The export starts at simulated second " << like.window_start_ns / kNsPerSecond
      << "; orders placed before it rest in the book without a message of their own.\n"
      << (filter ? "  * --filter-window is on: messages outside the top-" + std::to_string(levels) +
                       " window of the true book are left out.\n\n"
                 : std::string("  * --no-filter-window: every message from the window start on is written.\n\n"))
      <<
         "Injected counts:\n"
         "  hidden executions written  " << u(e.hidden_written()) << "\n"
         "  hidden draws skipped       " << u(e.hidden_skipped()) << " (the book was not two-sided)\n"
         "  halts written              " << u(e.halts_written())
      << (like.halt ? " (at simulated time " + seconds(e.halt_ns()) + " s)" : std::string(" (--no-halt)")) << "\n"
         "  messages filtered          " << u(e.dropped()) << " (outside the top-" << levels << " window)\n"
         "  messages before the window " << u(e.before_window()) << "\n"
         "  messages written           " << u(e.written()) << " (including the injected ones)\n"
         "  window start               " << like.window_start_ns / kNsPerSecond << " s of simulated time\n"
         "  window duration            " << window_s << " s\n";
  std::ofstream js(dir / (ticker + "_injected.json"), std::ios::binary | std::ios::trunc);
  js << "{\n  \"synthetic\": true,\n  \"hidden_exec_written\": " << u(e.hidden_written())
     << ",\n  \"hidden_skipped\": " << u(e.hidden_skipped()) << ",\n  \"halt_written\": " << u(e.halts_written())
     << ",\n  \"messages_filtered\": " << u(e.dropped()) << ",\n  \"messages_before_window\": " << u(e.before_window())
     << ",\n  \"messages_written\": " << u(e.written()) << ",\n  \"hidden_id_base\": " << kHiddenIdBase
     << ",\n  \"window_start_s\": " << like.window_start_ns / kNsPerSecond << ",\n  \"window_duration_s\": \"" << window_s
     << "\"\n}\n";
  txt.flush();
  js.flush();
  if (!txt || !js) throw std::runtime_error("error writing the synthetic notes into " + dir.string());
}

}  // namespace

int main(int argc, char** argv) {
  std::string run_dir, out_dir, ticker = "SIM";
  int levels = 0;
  std::string audit_path;
  bool filter = false;
  bool no_filter = false;
  std::int64_t price_scale = 1;
  LikeOptions like;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--filter-window") {
      filter = true;
      continue;
    }
    if (a == "--lobster-like") {
      like.on = true;
      continue;
    }
    if (a == "--no-filter-window") {
      no_filter = true;
      continue;
    }
    if (a == "--no-halt") {
      like.halt = false;
      continue;
    }
    if (i + 1 >= argc) return usage(a + " needs a value");
    const std::string_view v = argv[++i];
    if (a == "--run") {
      run_dir = v;
    } else if (a == "--out") {
      out_dir = v;
    } else if (a == "--ticker") {
      ticker = v;
    } else if (a == "--audit") {
      audit_path = v;
      if (audit_path.empty()) return usage("--audit needs a file name");
    } else if (a == "--window-start-s" || a == "--halt-at-s") {
      std::int64_t secs = 0;
      const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), secs);
      if (v.empty() || ec != std::errc{} || p != v.data() + v.size() || secs < 0 || secs > 1'000'000) {
        return usage(a + " must be a whole number of seconds, 0 to 1000000");
      }
      if (a == "--window-start-s") {
        like.window_start_ns = secs * kNsPerSecond;
      } else {
        like.halt_at_ns = secs * kNsPerSecond;
      }
    } else if (a == "--hidden-rate") {
      const std::string text(v);
      char* end = nullptr;
      like.hidden_rate = std::strtod(text.c_str(), &end);
      if (text.empty() || end != text.c_str() + text.size() || !(like.hidden_rate >= 0.0) || like.hidden_rate > 1e6) {
        return usage("--hidden-rate must be a number from 0 to 1000000 (per second)");
      }
    } else if (a == "--price-scale") {
      const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), price_scale);
      if (v.empty() || ec != std::errc{} || p != v.data() + v.size() || price_scale < 1) {
        return usage("--price-scale must be a positive integer");
      }
    } else if (a == "--seed") {
      const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), like.seed);
      if (v.empty() || ec != std::errc{} || p != v.data() + v.size()) return usage("--seed must be a non-negative integer");
    } else if (a == "--levels") {
      const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), levels);
      if (v.empty() || ec != std::errc{} || p != v.data() + v.size() || levels < 1) {
        return usage("--levels must be a positive integer");
      }
    } else {
      return usage("unknown argument " + a);
    }
  }
  if (run_dir.empty() || out_dir.empty() || levels < 1) return usage("--run, --levels and --out are required");
  if (ticker.empty() || ticker.find('/') != std::string::npos) return usage("--ticker must be a non-empty name without '/'");

  if ((no_filter || !like.halt) && !like.on) return usage("--no-filter-window and --no-halt need --lobster-like");
  if (no_filter && filter) return usage("--filter-window and --no-filter-window contradict each other");
  if (!like.halt && like.halt_at_ns) return usage("--halt-at-s and --no-halt contradict each other");
  if (like.on) {
    filter = !no_filter;
    const std::filesystem::path norm = std::filesystem::absolute(out_dir).lexically_normal();
    std::filesystem::path leaf = norm.filename().empty() ? norm.parent_path() : norm;  // drop a trailing '/'
    if (leaf.filename() == "lobster" && leaf.parent_path().filename() == "data") {
      std::fprintf(stderr,
                   "lobsim_export_lobster: refusing to write --lobster-like (synthetic) files into %s; "
                   "synthetic data must never be mistaken for real data (use data/synthetic)\n",
                   out_dir.c_str());
      return 1;
    }
  }
  const std::filesystem::path log_path = std::filesystem::path(run_dir) / "orders.log";
  std::ifstream log(log_path, std::ios::binary);
  if (!log) {
    std::fprintf(stderr, "lobsim_export_lobster: cannot read %s\n", log_path.string().c_str());
    return 1;
  }
  const std::string stem = ticker + "_2012-06-21_34200000_57600000_";
  const std::filesystem::path dir(out_dir);
  const std::filesystem::path msg_path = dir / (stem + "message_" + std::to_string(levels) + ".csv");
  const std::filesystem::path book_path = dir / (stem + "orderbook_" + std::to_string(levels) + ".csv");
  try {
    std::filesystem::create_directories(dir);
    std::ofstream messages(msg_path, std::ios::binary | std::ios::trunc);
    std::ofstream orderbook(book_path, std::ios::binary | std::ios::trunc);
    if (!messages || !orderbook) throw std::runtime_error("cannot write into " + out_dir);
    std::int64_t window_end_ns = 0;
    if (like.on) {  // first pass: the time of the last request ends the window
      std::ifstream first(log_path, std::ios::binary);
      sim::OrderLogReader pre(first);
      while (const auto r = pre.next()) window_end_ns = std::max<std::int64_t>(window_end_ns, r->time);
      if (like.window_start_ns > window_end_ns) throw std::runtime_error("--window-start-s is after the last request");
      if (like.halt_at_ns && (*like.halt_at_ns < like.window_start_ns || *like.halt_at_ns > window_end_ns)) {
        throw std::runtime_error("--halt-at-s is outside the window");
      }
    }
    std::ofstream audit;
    if (!audit_path.empty()) {
      audit.open(audit_path, std::ios::binary | std::ios::trunc);
      if (!audit) throw std::runtime_error("cannot write " + audit_path);
    }
    sim::OrderLogReader reader(log);
    if (reader.max_price() > 0 && price_scale > (kEmptyAsk - 1) / static_cast<std::int64_t>(reader.max_price())) {
      throw std::runtime_error("--price-scale " + std::to_string(price_scale) + " is too large: the run's largest price " +
                               std::to_string(reader.max_price()) + " times it must stay below " +
                               std::to_string(kEmptyAsk));
    }
    Exporter exporter(reader.max_price(), levels, filter, messages, orderbook, like, window_end_ns,
                      audit_path.empty() ? nullptr : &audit, price_scale);
    while (const auto r = reader.next()) exporter.request(*r);
    exporter.finish();
    messages.flush();
    orderbook.flush();
    if (!messages || !orderbook) throw std::runtime_error("error writing into " + out_dir);
    if (!audit_path.empty()) {
      audit.flush();
      if (!audit) throw std::runtime_error("error writing " + audit_path);
    }
    const std::uint64_t* t = exporter.by_type();
    std::printf("requests             %llu\n", static_cast<unsigned long long>(exporter.requests()));
    std::printf("messages written     %llu (type 1 %llu, 2 %llu, 3 %llu, 4 %llu)\n",
                static_cast<unsigned long long>(exporter.written()), static_cast<unsigned long long>(t[1]),
                static_cast<unsigned long long>(t[2]), static_cast<unsigned long long>(t[3]),
                static_cast<unsigned long long>(t[4]));
    std::printf("messages filtered    %llu%s\n", static_cast<unsigned long long>(exporter.dropped()),
                filter ? " (outside the top-L window before and after)" : "");
    if (like.on) {
      std::printf("before the window    %llu\n", static_cast<unsigned long long>(exporter.before_window()));
      std::printf("hidden executions    %llu (draws skipped, book not two-sided: %llu)\n",
                  static_cast<unsigned long long>(exporter.hidden_written()),
                  static_cast<unsigned long long>(exporter.hidden_skipped()));
      std::printf("halts                %llu\n", static_cast<unsigned long long>(exporter.halts_written()));
      write_synthetic_notes(dir, ticker, levels, filter, like, window_end_ns, exporter);
    }
    std::printf("wrote                %s\n                     %s\n", msg_path.string().c_str(),
                book_path.string().c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_export_lobster: %s\n", e.what());
    return 1;
  }
  return 0;
}
