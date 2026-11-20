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
#include <charconv>
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

namespace {

constexpr std::int64_t kOpenNs = 34'200'000'000'000;  // 9:30:00 in nanoseconds after midnight
constexpr std::size_t kAllLevels = std::numeric_limits<std::size_t>::max();
constexpr std::int64_t kEmptyAsk = 9'999'999'999;
constexpr std::int64_t kEmptyBid = -9'999'999'999;

int usage(const std::string& why) {
  std::fprintf(stderr, "lobsim_export_lobster: %s\n", why.c_str());
  std::fprintf(stderr,
               "usage: lobsim_export_lobster --run <sim output dir> --levels L [--filter-window] --out <dir> "
               "[--ticker NAME]\n");
  return 1;
}

class Exporter {
 public:
  Exporter(lob::Price max_price, int levels, bool filter, std::ostream& messages, std::ostream& orderbook)
      : engine_(lob::BookConfig{max_price}),
        mirror_(lob::BookConfig{max_price}),
        levels_(static_cast<std::size_t>(levels)),
        filter_(filter),
        messages_(messages),
        orderbook_(orderbook) {}

  void request(const sim::LoggedRequest& r) {
    ++requests_;
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

 private:
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
    const bool keep = before || in_window(side, price);
    if (!keep) {
      ++dropped_;
      return;
    }
    const replay::LobsterMsg m{kOpenNs + time_, type, static_cast<std::int64_t>(id), size, price,
                               side == lob::Side::Buy ? 1 : -1};
    messages_ << replay::format_message_line(m) << '\n';
    const std::vector<lob::LevelView> asks = mirror_.depth(lob::Side::Sell, levels_);
    const std::vector<lob::LevelView> bids = mirror_.depth(lob::Side::Buy, levels_);
    std::string row;
    for (std::size_t i = 0; i < levels_; ++i) {
      if (i > 0) row += ',';
      row += i < asks.size() ? std::to_string(asks[i].price) + ',' + std::to_string(asks[i].total_qty)
                             : std::to_string(kEmptyAsk) + ",0";
      row += ',';
      row += i < bids.size() ? std::to_string(bids[i].price) + ',' + std::to_string(bids[i].total_qty)
                             : std::to_string(kEmptyBid) + ",0";
    }
    orderbook_ << row << '\n';
    ++written_;
    ++by_type_[type];
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
};

}  // namespace

int main(int argc, char** argv) {
  std::string run_dir, out_dir, ticker = "SIM";
  int levels = 0;
  bool filter = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--filter-window") {
      filter = true;
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
    sim::OrderLogReader reader(log);
    Exporter exporter(reader.max_price(), levels, filter, messages, orderbook);
    while (const auto r = reader.next()) exporter.request(*r);
    messages.flush();
    orderbook.flush();
    if (!messages || !orderbook) throw std::runtime_error("error writing into " + out_dir);
    const std::uint64_t* t = exporter.by_type();
    std::printf("requests             %llu\n", static_cast<unsigned long long>(exporter.requests()));
    std::printf("messages written     %llu (type 1 %llu, 2 %llu, 3 %llu, 4 %llu)\n",
                static_cast<unsigned long long>(exporter.written()), static_cast<unsigned long long>(t[1]),
                static_cast<unsigned long long>(t[2]), static_cast<unsigned long long>(t[3]),
                static_cast<unsigned long long>(t[4]));
    std::printf("messages filtered    %llu%s\n", static_cast<unsigned long long>(exporter.dropped()),
                filter ? " (outside the top-L window before and after)" : "");
    std::printf("wrote                %s\n                     %s\n", msg_path.string().c_str(),
                book_path.string().c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_export_lobster: %s\n", e.what());
    return 1;
  }
  return 0;
}
