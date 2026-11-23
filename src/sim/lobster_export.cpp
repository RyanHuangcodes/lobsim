#include "sim/lobster_export.hpp"

#include <algorithm>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "replay/lobster.hpp"

namespace sim {

namespace {
constexpr std::size_t kAllLevels = std::numeric_limits<std::size_t>::max();
constexpr std::int64_t kEmptyAsk = 9'999'999'999;
constexpr std::int64_t kEmptyBid = -9'999'999'999;
}  // namespace

LobsterExporter::LobsterExporter(lob::Price max_price, int levels, bool filter, std::ostream& messages,
                                 std::ostream& orderbook, const LikeOptions& like, std::int64_t window_end_ns,
                                 std::ostream* audit, std::int64_t price_scale)
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

void LobsterExporter::finish() {
  if (like_.on) inject_until(std::numeric_limits<std::int64_t>::max());
}

void LobsterExporter::request(const sim::LoggedRequest& r) {
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

// The next hidden execution after `from_ns`: an exponential gap in whole nanoseconds. The draw is
// the only floating point; it never reaches a file.
void LobsterExporter::schedule_hidden(std::int64_t from_ns) {
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
void LobsterExporter::inject_until(std::int64_t limit_ns) {
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

void LobsterExporter::write_injected(std::int64_t at_ns, int type, std::int64_t id, std::int64_t size, std::int64_t price,
                                     int direction) {
  time_ = at_ns;
  const replay::LobsterMsg m{kLobsterOpenNs + (at_ns - like_.window_start_ns), type, id,
                             size, type == 7 ? price : price * price_scale_, direction};
  messages_ << replay::format_message_line(m) << '\n';
  if (last_row_.empty()) last_row_ = book_row();
  orderbook_ << last_row_ << '\n';
  ++written_;
  ++by_type_[type];
  write_audit(type, id, size, price, direction, "injected");
}

// One line of --audit: the message and what became of it (see the file comment).
void LobsterExporter::write_audit(int type, std::int64_t id, std::int64_t size, std::int64_t price, int direction,
                                  const char* status) {
  if (audit_ == nullptr) return;
  *audit_ << audit_seq_++ << ',' << time_ << ',' << type << ',' << id << ',' << size << ',' << price << ','
          << direction << ',' << status << ',' << written_ << '\n';
}

std::string LobsterExporter::book_row() const {
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

[[noreturn]] void LobsterExporter::fail(const std::string& what) const {
  throw std::runtime_error("request " + std::to_string(requests_) + " (time " + std::to_string(time_) +
                           " ns): " + what);
}

std::optional<lob::OrderView> LobsterExporter::mirror_order(lob::OrderId engine_id) const {
  const auto it = mirror_id_.find(engine_id);
  if (it == mirror_id_.end()) return std::nullopt;
  return mirror_.order(it->second);
}

lob::OrderView LobsterExporter::resting(lob::OrderId engine_id, const char* what) const {
  const auto o = mirror_order(engine_id);
  if (!o) fail(std::string(what) + ", order " + std::to_string(engine_id) + ", is not resting in the exported book");
  return *o;
}

// Inside the true book's top-L window on `side` (see the file comment).
bool LobsterExporter::in_window(lob::Side side, lob::Price price) const {
  const std::vector<lob::LevelView> top = mirror_.depth(side, levels_);
  if (top.size() < levels_) return true;
  return side == lob::Side::Sell ? price <= top.back().price : price >= top.back().price;
}

void LobsterExporter::emit(int type, lob::OrderId id, lob::Qty size, lob::Price price, lob::Side side) {
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
  const std::int64_t shown_ns = like_.on ? kLobsterOpenNs + (time_ - like_.window_start_ns) : kLobsterOpenNs + time_;
  const replay::LobsterMsg m{shown_ns, type, static_cast<std::int64_t>(id), size, price * price_scale_, direction};
  messages_ << replay::format_message_line(m) << '\n';
  last_row_ = book_row();
  orderbook_ << last_row_ << '\n';
  ++written_;
  ++by_type_[type];
  write_audit(type, static_cast<std::int64_t>(id), size, price, direction, "written");
}

void LobsterExporter::apply_to_mirror(int type, lob::OrderId id, lob::Qty size, lob::Price price, lob::Side side) {
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

ExportResult export_order_log(std::istream& log, const ExportSpec& spec, std::ostream& messages,
                              std::ostream& orderbook, std::ostream* audit) {
  const LikeOptions& like = spec.like;
  std::int64_t window_end_ns = 0;
  if (like.on) {  // first pass: the time of the last request ends the window
    OrderLogReader pre(log);
    while (const auto r = pre.next()) window_end_ns = std::max<std::int64_t>(window_end_ns, r->time);
    if (like.window_start_ns > window_end_ns) throw std::runtime_error("--window-start-s is after the last request");
    if (like.halt_at_ns && (*like.halt_at_ns < like.window_start_ns || *like.halt_at_ns > window_end_ns)) {
      throw std::runtime_error("--halt-at-s is outside the window");
    }
    log.clear();
    log.seekg(0);
  }
  OrderLogReader reader(log);
  if (reader.max_price() > 0 && spec.price_scale > (kEmptyAsk - 1) / static_cast<std::int64_t>(reader.max_price())) {
    throw std::runtime_error("--price-scale " + std::to_string(spec.price_scale) +
                             " is too large: the run's largest price " + std::to_string(reader.max_price()) +
                             " times it must stay below " + std::to_string(kEmptyAsk));
  }
  LobsterExporter exporter(reader.max_price(), spec.levels, spec.filter, messages, orderbook, like, window_end_ns,
                           audit, spec.price_scale);
  while (const auto r = reader.next()) exporter.request(*r);
  exporter.finish();
  ExportResult out;
  out.requests = exporter.requests();
  out.written = exporter.written();
  out.dropped = exporter.dropped();
  for (int i = 0; i < 8; ++i) out.by_type[i] = exporter.by_type()[i];
  out.hidden_written = exporter.hidden_written();
  out.hidden_skipped = exporter.hidden_skipped();
  out.halts_written = exporter.halts_written();
  out.before_window = exporter.before_window();
  out.halt_ns = exporter.halt_ns();
  out.window_end_ns = window_end_ns;
  return out;
}

}  // namespace sim
