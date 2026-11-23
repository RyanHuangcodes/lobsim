#include "replay/replayer.hpp"

// Test-the-tester bugs for the replay canaries (LOBSIM_REPLAY_BUG, upgrade 2): never in a normal build.
#if (defined(LOBSIM_REPLAY_BUG_DIRECTION_IS_AGGRESSOR) || defined(LOBSIM_REPLAY_BUG_APPLY_HIDDEN) || \
     defined(LOBSIM_REPLAY_BUG_NO_SEED)) && !defined(LOBSIM_REPLAY_BUG_BUILD)
#error "LOBSIM_REPLAY_BUG_* injects a replay bug and is only allowed in a LOBSIM_REPLAY_BUG build"
#endif

#include <algorithm>
#include <fstream>
#include <istream>
#include <limits>
#include <map>
#include <utility>

// The reports (summary.json, divergences.csv, the text summaries) are in replayer_report.cpp.
namespace replay {
namespace {

constexpr std::size_t kRecent = 20;
constexpr std::size_t kAllLevels = std::numeric_limits<std::size_t>::max();

lob::Side side_of(int direction) { return direction > 0 ? lob::Side::Buy : lob::Side::Sell; }

// Level i of our top-L list equals level i of the oracle side (an empty oracle level is size 0).
bool same_level(const std::vector<lob::LevelView>& ours, const std::vector<BookLevel>& oracle, std::size_t i) {
  if (i >= ours.size()) return oracle[i].empty();
  return !oracle[i].empty() && ours[i].price == oracle[i].price && ours[i].total_qty == oracle[i].size;
}

bool in_window(lob::Side side, lob::Price p, std::optional<lob::Price> bound) {
  if (!bound) return true;
  return side == lob::Side::Sell ? p <= *bound : p >= *bound;
}

}  // namespace

const char* to_string(Mode m) noexcept {
  switch (m) {
    case Mode::Strict: return "strict";
    case Mode::Report: return "report";
    case Mode::Resync: return "resync";
  }
  return "?";
}

std::optional<Mode> parse_mode(std::string_view s) {
  if (s == "strict") return Mode::Strict;
  if (s == "report") return Mode::Report;
  if (s == "resync") return Mode::Resync;
  return std::nullopt;
}

// ---------------------------------------------------------------- input

Replayer::Source::Source(std::istream& messages, std::istream& orderbook, int levels, std::size_t max_lookahead,
                         std::optional<std::int64_t> stop_before_ns)
    : messages_(messages),
      orderbook_(orderbook),
      levels_(levels),
      max_lookahead_(max_lookahead),
      stop_before_ns_(stop_before_ns) {}

bool Replayer::Source::read_one() {
  if (ended_) return false;
  Entry e;
  const bool got_msg = static_cast<bool>(std::getline(messages_, e.text));
  const bool got_book = static_cast<bool>(std::getline(orderbook_, e.book_text));
  if (messages_.bad() || orderbook_.bad()) throw InputError("read error after line " + std::to_string(line_));
  if (!got_msg && !got_book) {
    ended_ = true;
    return false;
  }
  if (got_msg != got_book) {
    throw InputError(std::string("the ") + (got_msg ? "orderbook" : "message") + " file ends after line " +
                     std::to_string(line_) + " but the " + (got_msg ? "message" : "orderbook") +
                     " file goes on: both must have one line per message");
  }
  e.msg = parse_message_line(e.text);
  if (stop_before_ns_ && e.msg.ok() && e.msg.value->time_ns >= *stop_before_ns_) {
    // The time bound: this line is only looked at, and nothing after it is read.
    ended_ = true;
    bound_line_ = line_ + 1;
    bound_time_ns_ = e.msg.value->time_ns;
    return false;
  }
  e.line = ++line_;
  e.book = parse_book_line(e.book_text, levels_);
  ahead_.push_back(std::move(e));
  return true;
}

const Entry* Replayer::Source::peek(std::size_t i) {
  while (ahead_.size() <= i) {
    if (ahead_.size() >= max_lookahead_) {
      throw InputError("more than " + std::to_string(max_lookahead_) + " messages would have to be buffered after line " +
                       std::to_string(line_ - ahead_.size()));
    }
    if (!read_one()) return nullptr;
  }
  return &ahead_[i];
}

std::optional<Entry> Replayer::Source::next() {
  if (peek(0) == nullptr) return std::nullopt;
  Entry e = std::move(ahead_.front());
  ahead_.pop_front();
  return e;
}

// ---------------------------------------------------------------- replay

Replayer::Replayer(int levels, ReplayOptions options)
    : levels_(levels), options_(options), book_(lob::BookConfig{kReplayMaxPrice}) {
  if (levels < 1) throw std::invalid_argument("the level count must be at least 1");
  if (levels > kMaxLevels) {
    throw std::invalid_argument("the level count " + std::to_string(levels) + " is above the limit of " +
                                std::to_string(kMaxLevels));
  }
  if (options.compare_depth < 1 || options.compare_depth > levels) {
    throw std::invalid_argument("--compare-depth must be between 1 and the file's level count " + std::to_string(levels));
  }
  if (options.trust_depth < 1) throw std::invalid_argument("--trust-depth must be at least 1");
  if (options.max_lookahead < 1) throw std::invalid_argument("the lookahead limit must be at least 1");
  summary_.mode = options.mode;
  summary_.levels = levels;
  summary_.compare_depth = options.compare_depth;
  summary_.trust_depth = options.trust_depth;
  const auto n = static_cast<std::size_t>(levels);
  summary_.exact_rows.assign(n, 0);
  summary_.resync.removed.assign(n, 0);
  summary_.resync.corrected.assign(n, 0);
  std::vector<int> depths{1, 3, levels, options.trust_depth};
  std::sort(depths.begin(), depths.end());
  depths.erase(std::unique(depths.begin(), depths.end()), depths.end());
  summary_.matching.headline_trust_depth = options.trust_depth;
  for (const int t : depths) summary_.matching.by_trust_depth.push_back(MatchCounts{t});
}

const Summary& Replayer::run(std::istream& messages, std::istream& orderbook) {
  Source source(messages, orderbook, levels_, options_.max_lookahead, stop_before_ns_);
  Counters& c = summary_.counters;
  while (std::optional<Entry> e = source.next()) {
    ++summary_.lines;
    ++c.messages;
    remember(*e);
    if (e->msg.ok()) ++c.by_type[e->msg.value->type];
    else ++c.parse_rejected;
    if (e->msg.time_truncated) ++c.time_truncated;
    if (e->book.oracle_malformed) ++c.oracle_malformed;

    if (e->line == 1) {
      // Row 1 is the book after message 1: the opening book is seeded from it (spec 4.2).
      const std::size_t columns = count_fields(e->book_text);
      if (columns != 4 * static_cast<std::size_t>(levels_)) {
        throw InputError("orderbook line 1 has " + std::to_string(columns) + " columns, but the file name says " +
                         std::to_string(levels_) + " levels (" + std::to_string(4 * levels_) + " columns)");
      }
      if (!e->book.ok()) throw InputError("orderbook line 1 (the opening book): " + e->book.error);
      if (e->msg.ok()) ++c.consumed_by_seed;
#ifndef LOBSIM_REPLAY_BUG_NO_SEED  // injected bug: the opening book is never seeded
      seed(*e->book.value);
#endif
    } else if (e->msg.ok()) {
      // The matching check runs before the first message of a burst is applied (spec 7.1).
      if (burst_left_ > 0) --burst_left_;
      else if (e->msg.value->type == 4) check_burst(*e, source);
      apply(*e->msg.value);
    }
    compare(*e);
    if (observer_ != nullptr) notify(*e);
    if (summary_.strict_failure) break;
  }
  run_end_.lines = summary_.lines;
  if (summary_.strict_failure) {
    run_end_.reason = StopReason::StrictFailure;
  } else if (source.bound_line() != 0) {
    run_end_.reason = StopReason::TimeBound;
    run_end_.bound_line = source.bound_line();
    run_end_.bound_time_ns = source.bound_time_ns();
  }
  if (observer_ != nullptr) observer_->on_end(run_end_);
  return summary_;
}

void Replayer::notify(Entry& e) {
  if (!observing_) {
    observing_ = e.msg.ok() && e.msg.value->time_ns >= observe_from_ns_;
  }
  if (observing_) {
    ObservedMessage m;
    m.line = e.line;
    m.msg = e.msg.ok() ? &*e.msg.value : nullptr;
    m.before = published_row(previous_book_);  // a default Parsed (no row) before line 1
    m.after = published_row(e.book);
    if (m.msg != nullptr && m.msg->type == 4 && e.line != 1) {
      // check_burst ran at the burst's first message; burst_left_ counts the members still to come.
      m.burst_size = burst_size_;
      m.burst_position = burst_size_ - 1 - burst_left_;
    }
    observer_->on_message(m);
  }
  previous_book_ = std::move(e.book);
}

PublishedRow published_row(const Parsed<BookRow>& parsed) {
  PublishedRow p;
  if (!parsed.ok()) return p;
  p.row = &*parsed.value;
  p.malformed = parsed.oracle_malformed;
  if (p.malformed) return p;
  // A well-formed side holds its non-empty levels first, so it shows all L levels exactly when
  // the L-th one is not empty.
  const std::vector<BookLevel>& asks = parsed.value->asks;
  const std::vector<BookLevel>& bids = parsed.value->bids;
  if (!asks.empty() && !asks.back().empty()) p.ask_bound = asks.back().price;
  if (!bids.empty() && !bids.back().empty()) p.bid_bound = bids.back().price;
  return p;
}

void Replayer::remember(const Entry& e) {
  recent_.push_back("line " + std::to_string(e.line) + ": " + e.text);
  if (recent_.size() > kRecent) recent_.pop_front();
}

void Replayer::seed(const BookRow& row) {
  Counters& c = summary_.counters;
  const auto place = [&](lob::Side side, const std::vector<BookLevel>& levels) {
    for (const BookLevel& l : levels) {
      if (l.empty()) continue;
      const lob::Result<lob::OrderId> r = book_.insert_passive(side, l.price, l.size);
      if (r.ok()) {
        synthetic_.insert(r.value);
        ++c.seed_orders;
      } else {
        ++c.seed_rejected;
      }
    }
  };
  place(lob::Side::Sell, row.asks);
  place(lob::Side::Buy, row.bids);
}

void Replayer::apply(const LobsterMsg& original) {
  Counters& c = summary_.counters;
#ifdef LOBSIM_REPLAY_BUG_DIRECTION_IS_AGGRESSOR
  // Injected bug: an execution's Direction is read as the aggressor's side, so the resting side is the other one.
  LobsterMsg flipped = original;
  if (flipped.type == 4) flipped.direction = -flipped.direction;
  const LobsterMsg& m = flipped;
#else
  const LobsterMsg& m = original;
#endif
  switch (m.type) {
    case 1: {
      if (by_external_.count(m.order_id) != 0) {
        ++c.dup_id;
        return;
      }
      const lob::Result<lob::OrderId> r = book_.insert_passive(side_of(m.direction), m.price, m.size);
      if (r.ok()) {
        by_external_.emplace(m.order_id, r.value);
        external_of_.emplace(r.value, m.order_id);
        ++c.submitted;
      } else if (r.status == lob::Status::WouldCross) {
        ++c.submit_would_cross;
      } else {
        ++c.submit_bad_price;  // the parser guarantees size > 0 and price > 0: only max_price is left
      }
      return;
    }
#ifdef LOBSIM_REPLAY_BUG_APPLY_HIDDEN
    case 5:  // injected bug: a hidden execution is applied like an execution
#endif
    case 2:
    case 3:
    case 4: {
      const auto it = by_external_.find(m.order_id);
      if (it != by_external_.end()) apply_known(m, it->second);
      else attribute(m);
      return;
    }
#ifndef LOBSIM_REPLAY_BUG_APPLY_HIDDEN
    case 5: ++c.hidden_exec; return;
#endif
    case 6: ++c.cross_trade; return;
    case 7: ++c.halt; return;
    default: return;  // the parser accepts 1..7 only
  }
}

// Validated before anything changes: a message that disagrees with the order its id names is
// counted and skipped.
void Replayer::apply_known(const LobsterMsg& m, lob::OrderId id) {
  Counters& c = summary_.counters;
  const std::optional<lob::OrderView> o = book_.order(id);
  if (!o) return;  // unreachable: entries leave the maps when their order leaves the book
  if (o->side != side_of(m.direction)) {
    ++c.direction_mismatch;
    return;
  }
  if (o->price != m.price) {
    ++c.price_mismatch;
    return;
  }
  if (m.type == 3) {
    if (m.size != o->remaining) ++c.delete_size_mismatch;
    book_.remove(id);
    ++c.deleted;
  } else {
    if (book_.reduce(id, m.size) == lob::Status::Oversized) ++c.oversized_reduce;
    ++(m.type == 2 ? c.cancelled : c.executed);
  }
  forget_if_gone(id);
}

// Seed attribution (spec 4.3): the oldest synthetic order at exactly this price on the
// Direction side that can absorb the size.
void Replayer::attribute(const LobsterMsg& m) {
  for (const lob::OrderView& o : book_.level_orders(side_of(m.direction), m.price)) {
    if (synthetic_.count(o.id) == 0 || o.remaining < m.size) continue;
    book_.reduce(o.id, m.size);
    forget_if_gone(o.id);
    ++summary_.counters.seed_attributed;
    return;
  }
  ++summary_.counters.unknown_id;
}

void Replayer::forget_if_gone(lob::OrderId id) {
  if (book_.order(id)) return;
  synthetic_.erase(id);
  const auto it = external_of_.find(id);
  if (it == external_of_.end()) return;
  by_external_.erase(it->second);
  external_of_.erase(it);
}

// ---------------------------------------------------------------- oracle

void Replayer::compare(const Entry& e) {
  Counters& c = summary_.counters;
  if (!e.book.ok()) {
    ++c.oracle_rejected;
    previous_exact_depth_ = 0;
    if (options_.mode == Mode::Strict) fail_strict(e, 0);
    return;
  }
  const BookRow& row = *e.book.value;
  const auto L = static_cast<std::size_t>(levels_);
  const std::vector<lob::LevelView> asks = book_.depth(lob::Side::Sell, L);
  const std::vector<lob::LevelView> bids = book_.depth(lob::Side::Buy, L);
  if (!asks.empty() && !bids.empty() && bids.front().price >= asks.front().price) ++c.crossed_state;

  std::size_t exact = 0;
  while (exact < L && same_level(asks, row.asks, exact) && same_level(bids, row.bids, exact)) ++exact;
  ++summary_.rows_compared;
  for (std::size_t d = 0; d < exact; ++d) ++summary_.exact_rows[d];
  previous_exact_depth_ = static_cast<int>(exact);

  if (exact < L) {
    ++summary_.divergent_rows;
    if (summary_.divergent_rows <= kMaxDivergences) {
      const auto add = [&](char side, const std::vector<lob::LevelView>& ours, const std::vector<BookLevel>& oracle) {
        if (same_level(ours, oracle, exact)) return;
        Divergence d{e.line, static_cast<int>(exact) + 1, side, std::nullopt, oracle[exact]};
        if (exact < ours.size()) d.ours = ours[exact];
        summary_.divergences.push_back(d);
      };
      add('A', asks, row.asks);
      add('B', bids, row.bids);
    }
  }

  if (options_.mode == Mode::Strict && static_cast<int>(exact) < options_.compare_depth) {
    fail_strict(e, static_cast<int>(exact));
    return;
  }
  if (options_.mode == Mode::Resync && exact < L) {
    if (e.book.oracle_malformed) ++summary_.resync.skipped_malformed;
    else resync(row);
  }
}

// Spec 4.4, resync. Per side, the window is every price at least as good as the oracle's L-th
// level, or the whole side when the oracle shows fewer than L levels. Levels outside it are
// never touched. Removals first (both sides), then size corrections, so that a synthetic insert
// never meets a level that is about to be removed.
void Replayer::resync(const BookRow& row) {
  struct Plan {
    lob::Side side;
    const std::vector<BookLevel>* oracle;
    std::vector<lob::LevelView> before;  // every level of the side, before the correction
  };
  Plan plans[2] = {{lob::Side::Sell, &row.asks, book_.depth(lob::Side::Sell, kAllLevels)},
                   {lob::Side::Buy, &row.bids, book_.depth(lob::Side::Buy, kAllLevels)}};
  ResyncStats& rs = summary_.resync;
  bool changed = false;

  for (const Plan& p : plans) {
    std::map<lob::Price, lob::Qty> oracle;
    for (const BookLevel& l : *p.oracle) {
      if (!l.empty()) oracle.emplace(l.price, l.size);
    }
    std::optional<lob::Price> bound;
    if (oracle.size() == static_cast<std::size_t>(levels_)) bound = p.oracle->back().price;
    for (std::size_t rank = 1; rank <= p.before.size(); ++rank) {
      const lob::LevelView& lv = p.before[rank - 1];
      if (!in_window(p.side, lv.price, bound)) break;  // sorted best first: the rest is outside too
      if (oracle.count(lv.price) != 0) continue;
      remove_level(p.side, lv.price);
      if (rank <= static_cast<std::size_t>(levels_)) ++rs.removed[rank - 1];
      else ++rs.removed_beyond_l;
      changed = true;
    }
  }

  for (const Plan& p : plans) {
    for (std::size_t d = 0; d < p.oracle->size(); ++d) {
      const BookLevel& o = (*p.oracle)[d];
      if (o.empty()) continue;
      const auto it = std::find_if(p.before.begin(), p.before.end(),
                                   [&](const lob::LevelView& lv) { return lv.price == o.price; });
      const lob::Qty have = it == p.before.end() ? 0 : it->total_qty;
      if (have == o.size) continue;
      if (have < o.size) {
        const lob::Result<lob::OrderId> r = book_.insert_passive(p.side, o.price, o.size - have);
        if (r.ok()) synthetic_.insert(r.value);
        else ++rs.failed;
      } else {
        trim_level(p.side, o.price, have - o.size);
      }
      ++rs.corrected[d];
      changed = true;
    }
  }
  if (changed) ++rs.rows;
}

void Replayer::remove_level(lob::Side side, lob::Price price) {
  for (const lob::OrderView& o : book_.level_orders(side, price)) {
    book_.remove(o.id);
    forget_if_gone(o.id);
  }
}

// Takes `excess` away from the newest orders of the level first, so older orders keep their place.
void Replayer::trim_level(lob::Side side, lob::Price price, lob::Qty excess) {
  const std::vector<lob::OrderView> orders = book_.level_orders(side, price);
  for (auto it = orders.rbegin(); it != orders.rend() && excess > 0; ++it) {
    const lob::Qty take = std::min(excess, it->remaining);
    book_.reduce(it->id, take);
    forget_if_gone(it->id);
    excess -= take;
  }
}

void Replayer::fail_strict(const Entry& e, int exact_depth) {
  StrictFailure f;
  f.line = e.line;
  f.exact_depth = exact_depth;
  f.oracle_unparsable = !e.book.ok();
  f.oracle_error = e.book.error;
  const auto L = static_cast<std::size_t>(levels_);
  f.ours_asks = book_.depth(lob::Side::Sell, L);
  f.ours_bids = book_.depth(lob::Side::Buy, L);
  f.oracle = e.book.value;
  f.recent.assign(recent_.begin(), recent_.end());
  summary_.strict_failure = std::move(f);
}

// ---------------------------------------------------------------- files

Summary replay_files(const std::string& messages_path, const std::string& orderbook_path,
                     const ReplayOptions& options) {
  const std::optional<int> levels = levels_from_filename(orderbook_path);
  if (!levels) {
    throw InputError("cannot read the level count from the orderbook file name " + orderbook_path +
                     " (expected ..._orderbook_<L>.csv)");
  }
  if (*levels > kMaxLevels) {
    throw InputError("the orderbook file name says " + std::to_string(*levels) + " levels, at most " +
                     std::to_string(kMaxLevels) + " are accepted");
  }
  if (const std::optional<int> m = levels_from_filename(messages_path); m && *m != *levels) {
    throw InputError("the message file name says " + std::to_string(*m) + " levels but the orderbook file name says " +
                     std::to_string(*levels));
  }
  std::ifstream messages(messages_path, std::ios::binary);
  if (!messages) throw InputError("cannot read " + messages_path);
  std::ifstream orderbook(orderbook_path, std::ios::binary);
  if (!orderbook) throw InputError("cannot read " + orderbook_path);
  Replayer r(*levels, options);
  return r.run(messages, orderbook);
}

}  // namespace replay
