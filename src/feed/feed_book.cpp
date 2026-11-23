#include "feed/feed_book.hpp"

#include <optional>

namespace feed {
namespace {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

void mix(std::uint64_t& h, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xff;
    h *= kFnvPrime;
  }
}

std::string ref_text(std::uint64_t ref) { return "order ref " + std::to_string(ref); }

}  // namespace

FeedBook::FeedBook(std::size_t expected_refs, std::uint64_t max_ref)
    : book_(lob::BookConfig{kMaxTicks}), max_ref_(max_ref) {
  ref_to_book_.reserve(expected_refs + 1);
}

bool FeedBook::fail(char type, const std::string& what) {
  error_ = "message " + std::to_string(messages_) + " (type " + std::string(1, type) + "): " + what;
  return false;
}

bool FeedBook::enter(char type) {
  ++messages_;
  if (closed_) return fail(type, "after the end-of-messages event (S C)");
  if (!open_ && type != 'S') return fail(type, "before the start-of-messages event (S O)");
  return true;
}

bool FeedBook::on(const MsgS& m) {
  if (!enter('S')) return false;
  if (!open_) {
    if (m.event_code != 'O') return fail('S', "the first message is not the start-of-messages event (S O)");
    open_ = true;
    return true;
  }
  if (m.event_code == 'O') return fail('S', "a second start-of-messages event");
  if (m.event_code == 'C') closed_ = true;
  return true;  // other system events (start of market hours and so on) change no order
}

bool FeedBook::on(const MsgA& m) {
  if (!enter('A')) return false;
  if (lookup(m.ref) != 0) return fail('A', ref_text(m.ref) + " is already resting");
  if (m.ref >= max_ref_) {
    return fail('A', ref_text(m.ref) + " is beyond the dense reference index (" + std::to_string(max_ref_) + ")");
  }
  if (m.side != 'B' && m.side != 'S') return fail('A', "side is neither B nor S");
  if (m.price % 100 != 0) return fail('A', "price " + std::to_string(m.price) + " is not a whole number of cents");
  const lob::Side side = m.side == 'B' ? lob::Side::Buy : lob::Side::Sell;
  const lob::Result<lob::OrderId> r = book_.insert_passive(side, static_cast<lob::Price>(m.price / 100), m.shares);
  if (!r.ok()) {
    if (r.status == lob::Status::WouldCross) {
      return fail('A', ref_text(m.ref) + " at " + std::to_string(m.price / 100) +
                           " ticks would cross the book (published before its executions?)");
    }
    return fail('A', ref_text(m.ref) + ": insert_passive returned " + lob::to_string(r.status));
  }
  if (m.ref >= ref_to_book_.size()) ref_to_book_.resize(m.ref + 1, 0);
  ref_to_book_[m.ref] = r.value;
  return true;
}

bool FeedBook::on(const MsgE& m) {
  if (!enter('E')) return false;
  const lob::OrderId id = lookup(m.ref);
  if (id == 0) return fail('E', ref_text(m.ref) + " is not resting");
  const std::optional<lob::OrderView> o = book_.order(id);
  if (m.shares == 0) return fail('E', "0 executed shares");
  if (m.shares > o->remaining) {
    return fail('E', std::to_string(m.shares) + " executed shares, but " + ref_text(m.ref) + " has " +
                         std::to_string(o->remaining));
  }
  if (m.shares == o->remaining) {
    book_.remove(id);
    ref_to_book_[m.ref] = 0;
  } else {
    book_.reduce(id, m.shares);
  }
  executed_shares_ += m.shares;
  return true;
}

bool FeedBook::on(const MsgX& m) {
  if (!enter('X')) return false;
  const lob::OrderId id = lookup(m.ref);
  if (id == 0) return fail('X', ref_text(m.ref) + " is not resting");
  const std::optional<lob::OrderView> o = book_.order(id);
  if (m.shares == 0) return fail('X', "0 cancelled shares");
  if (m.shares >= o->remaining) {
    return fail('X', std::to_string(m.shares) + " cancelled shares, but " + ref_text(m.ref) + " has only " +
                         std::to_string(o->remaining) + " (a full cancel is a D)");
  }
  book_.reduce(id, m.shares);
  return true;
}

bool FeedBook::on(const MsgD& m) {
  if (!enter('D')) return false;
  const lob::OrderId id = lookup(m.ref);
  if (id == 0) return fail('D', ref_text(m.ref) + " is not resting");
  book_.remove(id);
  ref_to_book_[m.ref] = 0;
  return true;
}

bool FeedBook::on_unknown(std::uint8_t type, std::span<const std::byte>) {
  return enter(static_cast<char>(type));  // skipped, but still between S O and S C
}

bool FeedBook::finish() {
  if (failed()) return false;
  if (!closed_) {
    error_ = "the feed ends after message " + std::to_string(messages_) + " without the end-of-messages event (S C)";
    return false;
  }
  return true;
}

std::uint64_t book_digest(const lob::OrderBook& book) {
  std::uint64_t h = kFnvOffset;
  for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
    const std::vector<lob::LevelView> levels = book.depth(side, std::numeric_limits<std::size_t>::max());
    mix(h, levels.size());
    for (const lob::LevelView& l : levels) {
      const std::vector<lob::OrderView> orders = book.level_orders(side, l.price);
      mix(h, static_cast<std::uint64_t>(l.price));
      mix(h, static_cast<std::uint64_t>(l.total_qty));
      mix(h, orders.size());
      for (const lob::OrderView& o : orders) mix(h, static_cast<std::uint64_t>(o.remaining));
    }
  }
  return h;
}

}  // namespace feed
