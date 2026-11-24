#include "lob/order_book.hpp"

// Test-the-tester bugs for the fuzz canaries (LOBSIM_INJECT_BUG, upgrade 1): never in a normal build.
#if (defined(LOBSIM_BUG_TAKER_PRICE) || defined(LOBSIM_BUG_MODIFY_KEEPS_PRIORITY) || \
     defined(LOBSIM_BUG_FOK_IGNORES_STP)) && !defined(LOBSIM_FUZZ_BUILD)
#error "LOBSIM_BUG_* injects a matching bug and is only allowed in a LOBSIM_FUZZ build"
#endif

#include <algorithm>
#include <stdexcept>
#include <string>

namespace lob {

OrderBook::OrderBook(BookConfig cfg) : cfg_(cfg) {
  // resize, not reserve: rest() grows the index with resize, so slots must exist and be null (D-261).
  if (cfg_.expected_order_ids > 0) index_.resize(cfg_.expected_order_ids + 1, nullptr);
}

// ---------------------------------------------------------------- helpers

namespace {

template <class Taker>
bool taker_crosses(const Taker& t, Price maker_price) {
  if (t.is_market) return true;
  return t.side == Side::Buy ? maker_price <= t.limit : maker_price >= t.limit;
}

}  // namespace

// Post-only "would cross": buy price >= best ask, or sell price <= best bid (3.3.5, D-023).
bool OrderBook::would_cross(Side side, Price price) const {
  if (side == Side::Buy) return !asks_.empty() && price >= asks_.begin()->first;
  return !bids_.empty() && price <= bids_.begin()->first;
}

void OrderBook::set_index(OrderId id, Order* o) noexcept {
  Order*& slot = index_[id];
  if (slot != nullptr) --indexed_;
  if (o != nullptr) ++indexed_;
  slot = o;
}

void OrderBook::unlink(Order* o) noexcept {
  Level* level = o->level;
  (o->prev != nullptr ? o->prev->next : level->head) = o->next;
  (o->next != nullptr ? o->next->prev : level->tail) = o->prev;
  level->total_qty -= o->remaining;
  --level->count;
  o->prev = nullptr;
  o->next = nullptr;
  o->level = nullptr;
}

void OrderBook::release(Order* o) noexcept {
  set_index(o->id, nullptr);
  pool_.destroy(o);  // `o` dangles from here on
}

void OrderBook::remove_resting(Order* o) {
  Level* level = o->level;
  const Side side = o->side;
  const Price price = level->price;
  unlink(o);
  release(o);
  if (level->count == 0) {
    // No order points at the level any more, so erasing its map node is safe.
    if (side == Side::Buy) bids_.erase(price);
    else asks_.erase(price);
  }
}

template <class SideMap>
void OrderBook::append(SideMap& side, Order* o) {
  Level& level = side.try_emplace(o->price, Level{o->price, 0, 0, nullptr, nullptr}).first->second;
  o->level = &level;  // map nodes never move, so this stays valid until the level is erased
  o->prev = level.tail;
  o->next = nullptr;
  (level.tail != nullptr ? level.tail->next : level.head) = o;
  level.tail = o;
  level.total_qty += o->remaining;
  ++level.count;
}

// Rests the taker's remainder at the tail of its level. A re-entering modify reuses its
// OrderId, so its index slot already exists; a new id may need the index to grow.
void OrderBook::rest(const Taker& t, bool post_only) {
  if (t.id >= index_.size()) index_.resize(std::max<std::size_t>(t.id + 1, index_.size() * 2));
  Order* o = pool_.create(
      Order{t.id, t.owner, t.side, t.limit, t.remaining, t.stp, post_only, nullptr, nullptr, nullptr});
  if (t.side == Side::Buy) append(bids_, o);
  else append(asks_, o);
  set_index(t.id, o);
}

// ---------------------------------------------------------------- matching

// FOK dry run (3.3.4, 3.4): a read-only walk of the opposite side in priority order,
// summing what the taker could fill. Allocates nothing and changes nothing.
template <class Makers>
bool OrderBook::fok_fillable_in(const Makers& makers, const Taker& t) const {
  Qty found = 0;
  for (const auto& [price, level] : makers) {
    if (!taker_crosses(t, price)) break;
    for (const Order* m = level.head; m != nullptr; m = m->next) {
      if (found >= t.remaining) return true;
      if (m->owner == t.owner) {
        if (t.stp == STP::CancelResting) continue;  // would be cancelled, adds nothing
#ifdef LOBSIM_BUG_FOK_IGNORES_STP
        found += m->remaining;  // injected bug: the dry run counts the taker's own order
        continue;
#else
        return false;                               // CancelTaking: the walk ends here
#endif
      }
      found += m->remaining;
    }
  }
  return found >= t.remaining;
}

bool OrderBook::fok_fillable(const Taker& t) const {
  return t.side == Side::Buy ? fok_fillable_in(asks_, t) : fok_fillable_in(bids_, t);
}

// Matches `t` against `makers` (3.3.2, 3.4), emitting Trades and STP cancels in match
// order. Pointer rules: the next maker is read before the current one can be released,
// and a level is erased only after its FIFO walk is over, when nothing points at it.
template <class Makers>
OrderBook::MatchResult OrderBook::match_in(Makers& makers, Taker& t, EventBuffer& out) {
  MatchResult result;
  while (t.remaining > 0 && !makers.empty()) {
    auto it = makers.begin();
    Level& level = it->second;
    if (!taker_crosses(t, level.price)) break;

    Order* m = level.head;
    while (m != nullptr && t.remaining > 0) {
      Order* const next = m->next;
      if (m->owner == t.owner) {
        if (t.stp == STP::CancelTaking) {
          result.stopped_by_stp = true;
          break;
        }
        out.push_back(Cancelled{m->id, m->owner, m->remaining, CancelReason::SelfTrade});
        unlink(m);
        release(m);
      } else {
        const Qty qty = std::min(t.remaining, m->remaining);
        Price trade_price = level.price;
#ifdef LOBSIM_BUG_TAKER_PRICE
        if (!t.is_market) trade_price = t.limit;  // injected bug: print at the taker's limit
#endif
        out.push_back(Trade{next_trade_id_++, trade_price, qty, m->id, t.id, m->owner, t.owner,
                            t.side});
        t.remaining -= qty;
        m->remaining -= qty;
        level.total_qty -= qty;
        result.filled += qty;
        if (m->remaining == 0) {
          unlink(m);
          release(m);
        }
      }
      m = next;
    }

    if (level.count == 0) makers.erase(it);
    if (result.stopped_by_stp) break;
  }
  return result;
}

OrderBook::MatchResult OrderBook::match(Taker& t, EventBuffer& out) {
  return t.side == Side::Buy ? match_in(asks_, t, out) : match_in(bids_, t, out);
}

// ---------------------------------------------------------------- submit

void OrderBook::submit(const NewOrder& req, EventBuffer& out) {
  do_submit(req, out);
#ifndef NDEBUG
  validate();
#endif
}

void OrderBook::do_submit(const NewOrder& req, EventBuffer& out) {
  auto reject = [&](RejectReason reason) {
    out.push_back(Reject{req.owner, RequestKind::New, req.client_order_id, 0, reason});
  };
  const bool is_market = req.type == OrderType::Market;

  // 3.2 validation, first failing check wins.
  if (req.qty <= 0) return reject(RejectReason::InvalidQty);
  if ((req.post_only && (is_market || req.tif != TIF::GTC)) || (is_market && req.tif != TIF::IOC))
    return reject(RejectReason::InvalidFlags);
  if (!is_market && !price_in_range(req.price)) return reject(RejectReason::InvalidPrice);
  if (req.post_only && would_cross(req.side, req.price))
    return reject(RejectReason::PostOnlyWouldCross);

  const OrderId id = next_order_id_++;
  out.push_back(Ack{id, req.owner, req.client_order_id});

  Taker t{id, req.owner, req.side, is_market, req.price, req.qty, req.stp};

  if (req.tif == TIF::FOK && !fok_fillable(t)) {
    out.push_back(Cancelled{id, req.owner, t.remaining, CancelReason::FOKUnfillable});
    return;
  }

  const MatchResult result = match(t, out);
  if (t.remaining == 0) return;

  if (req.tif == TIF::GTC && !result.stopped_by_stp) {
    rest(t, req.post_only);
    return;
  }

  // Remainder is cancelled; the reason follows D-021, D-030 and 3.4.
  CancelReason reason = CancelReason::IOCRemainder;
  if (result.stopped_by_stp) reason = CancelReason::SelfTrade;
  else if (is_market && result.filled == 0) reason = CancelReason::NoLiquidity;
  out.push_back(Cancelled{id, req.owner, t.remaining, reason});
}

// ---------------------------------------------------------------- cancel

void OrderBook::cancel(const Cancel& req, EventBuffer& out) {
  do_cancel(req, out);
#ifndef NDEBUG
  validate();
#endif
}

void OrderBook::do_cancel(const Cancel& req, EventBuffer& out) {
  auto reject = [&](RejectReason reason) {
    out.push_back(Reject{req.owner, RequestKind::Cancel, 0, req.order_id, reason});
  };
  Order* o = find(req.order_id);
  if (o == nullptr) return reject(RejectReason::UnknownOrder);
  if (o->owner != req.owner) return reject(RejectReason::NotOwner);

  out.push_back(Cancelled{o->id, o->owner, o->remaining, CancelReason::User});
  remove_resting(o);
}

// ---------------------------------------------------------------- modify

void OrderBook::modify(const Modify& req, EventBuffer& out) {
  do_modify(req, out);
#ifndef NDEBUG
  validate();
#endif
}

void OrderBook::do_modify(const Modify& req, EventBuffer& out) {
  auto reject = [&](RejectReason reason) {
    out.push_back(Reject{req.owner, RequestKind::Modify, 0, req.order_id, reason});
  };
  Order* o = find(req.order_id);
  if (o == nullptr) return reject(RejectReason::UnknownOrder);
  if (o->owner != req.owner) return reject(RejectReason::NotOwner);
  if (req.new_qty <= 0) return reject(RejectReason::InvalidQty);
  if (!price_in_range(req.new_price)) return reject(RejectReason::InvalidPrice);
  if (req.new_price == o->price && req.new_qty == o->remaining) return reject(RejectReason::NoChange);
  if (o->post_only && would_cross(o->side, req.new_price))
    return reject(RejectReason::PostOnlyWouldCross);

  // Same price, smaller qty: reduce in place, keep priority.
#ifdef LOBSIM_BUG_MODIFY_KEEPS_PRIORITY
  if (req.new_price == o->price) {  // injected bug: an increase keeps its place too
#else
  if (req.new_price == o->price && req.new_qty < o->remaining) {
#endif
    o->level->total_qty -= o->remaining - req.new_qty;
    o->remaining = req.new_qty;
    out.push_back(Modified{o->id, o->owner, o->price, req.new_qty, true});
    return;
  }

  // Anything else loses priority: the order leaves the book (its node goes back to the
  // pool and its level may be erased) and re-enters as a new arrival with the same
  // OrderId, keeping stp and post_only. Everything needed afterwards is copied first.
  const OrderId id = o->id;
  const OwnerId owner = o->owner;
  const Side side = o->side;
  const STP stp = o->stp;
  const bool post_only = o->post_only;
  remove_resting(o);  // `o` dangles from here on
  out.push_back(Modified{id, owner, req.new_price, req.new_qty, false});

  Taker t{id, owner, side, false, req.new_price, req.new_qty, stp};
  const MatchResult result = match(t, out);
  if (t.remaining == 0) return;
  if (result.stopped_by_stp) {
    out.push_back(Cancelled{id, owner, t.remaining, CancelReason::SelfTrade});
    return;
  }
  rest(t, post_only);
}

// ---------------------------------------------------------------- book builder

Result<OrderId> OrderBook::insert_passive(Side side, Price price, Qty qty, OwnerId owner) {
  if (qty <= 0) return {Status::InvalidQty, 0};
  if (!price_in_range(price)) return {Status::InvalidPrice, 0};
  if (would_cross(side, price)) return {Status::WouldCross, 0};
  const OrderId id = next_order_id_++;
  rest(Taker{id, owner, side, false, price, qty, STP::CancelTaking}, false);
#ifndef NDEBUG
  validate();
#endif
  return {Status::Ok, id};
}

Status OrderBook::reduce(OrderId id, Qty qty) {
  if (qty <= 0) return Status::InvalidQty;
  Order* o = find(id);
  if (o == nullptr) return Status::UnknownOrder;
  Status status = Status::Ok;
  if (qty >= o->remaining) {
    if (qty > o->remaining) status = Status::Oversized;  // D-152: it is removed all the same
    remove_resting(o);
  } else {
    o->level->total_qty -= qty;
    o->remaining -= qty;
  }
#ifndef NDEBUG
  validate();
#endif
  return status;
}

Status OrderBook::remove(OrderId id) {
  Order* o = find(id);
  if (o == nullptr) return Status::UnknownOrder;
  remove_resting(o);
#ifndef NDEBUG
  validate();
#endif
  return Status::Ok;
}

template <class Makers>
void OrderBook::preview_in(const Makers& makers, Side taker_side, Price limit, Qty qty,
                           std::vector<Fill>& fills) const {
  Qty left = qty;
  for (const auto& [price, level] : makers) {
    if (left <= 0) break;
    if (taker_side == Side::Buy ? price > limit : price < limit) break;
    for (const Order* m = level.head; m != nullptr && left > 0; m = m->next) {
      const Qty take = std::min(left, m->remaining);
      fills.push_back(Fill{m->id, price, take});
      left -= take;
    }
  }
}

std::vector<Fill> OrderBook::preview_match(Side taker_side, Price limit, Qty qty) const {
  std::vector<Fill> fills;
  if (qty <= 0) return fills;
  if (taker_side == Side::Buy) preview_in(asks_, taker_side, limit, qty, fills);
  else preview_in(bids_, taker_side, limit, qty, fills);
  return fills;
}

// ---------------------------------------------------------------- queries

std::optional<Price> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

std::optional<LevelView> OrderBook::best_level(Side side) const {
  const Level* level = nullptr;
  if (side == Side::Buy && !bids_.empty()) level = &bids_.begin()->second;
  if (side == Side::Sell && !asks_.empty()) level = &asks_.begin()->second;
  if (level == nullptr) return std::nullopt;
  return LevelView{level->price, level->total_qty};
}

std::vector<LevelView> OrderBook::depth(Side side, std::size_t n) const {
  std::vector<LevelView> levels;
  auto collect = [&](const auto& book_side) {
    for (const auto& [price, level] : book_side) {
      if (levels.size() == n) break;
      levels.push_back(LevelView{price, level.total_qty});
    }
  };
  if (side == Side::Buy) collect(bids_);
  else collect(asks_);
  return levels;
}

std::optional<OrderView> OrderBook::order(OrderId id) const {
  const Order* o = find(id);
  if (o == nullptr) return std::nullopt;
  return OrderView{o->id, o->owner, o->side, o->price, o->remaining, o->stp, o->post_only};
}

std::vector<OrderView> OrderBook::level_orders(Side side, Price price) const {
  std::vector<OrderView> orders;
  auto collect = [&](const auto& book_side) {
    const auto it = book_side.find(price);
    if (it == book_side.end()) return;
    for (const Order* o = it->second.head; o != nullptr; o = o->next)
      orders.push_back(OrderView{o->id, o->owner, o->side, o->price, o->remaining, o->stp, o->post_only});
  };
  if (side == Side::Buy) collect(bids_);
  else collect(asks_);
  return orders;
}

std::size_t OrderBook::resting_orders() const { return indexed_; }

// ---------------------------------------------------------------- validate

namespace {

[[noreturn]] void fail(const std::string& what) { throw std::logic_error("OrderBook: " + what); }

}  // namespace

// Messages are built only on failure: validate() runs after every message in Debug.
template <class SideMap>
void OrderBook::validate_side(const SideMap& side, Side which, std::size_t& orders) const {
  for (const auto& entry : side) {
    const Price key = entry.first;
    const Level& level = entry.second;
    auto where = [&] { return std::string(to_string(which)) + " level " + std::to_string(key); };
    auto who = [&](const Order* o) { return where() + ", order " + std::to_string(o->id); };

    if (level.price != key) fail(where() + ": level price " + std::to_string(level.price) + " differs from its key");
    if (!price_in_range(key)) fail(where() + ": price outside [1, max_price]");
    if (level.count == 0 || level.head == nullptr) fail(where() + ": empty level left in the book");
    if (level.head->prev != nullptr) fail(where() + ": list head has a prev link");
    if (level.tail == nullptr || level.tail->next != nullptr) fail(where() + ": list tail has a next link");

    Qty sum = 0;
    std::uint32_t len = 0;
    for (const Order* o = level.head; o != nullptr; o = o->next) {
      if (++len > level.count)
        fail(where() + ": count " + std::to_string(level.count) + " is less than the list length");
      if (o->next != nullptr ? o->next->prev != o : o != level.tail) fail(who(o) + ": next/prev links disagree");
      if (o->level != &level) fail(who(o) + ": level pointer is not its level");
      if (o->side != which) fail(who(o) + ": side differs from its level's side");
      if (o->price != level.price) fail(who(o) + ": price differs from its level's price");
      if (o->remaining <= 0) fail(who(o) + ": remaining <= 0");
      if (o->id >= index_.size() || index_[o->id] != o) fail(who(o) + ": id index does not point at it");
      sum += o->remaining;
    }
    if (len != level.count)
      fail(where() + ": count " + std::to_string(level.count) + " but list length " + std::to_string(len));
    if (sum != level.total_qty)
      fail(where() + ": total_qty " + std::to_string(level.total_qty) + " but orders sum to " + std::to_string(sum));
    orders += len;
  }
}

// SPEC 3.8. Every resting order is reached through the level lists and must be pointed
// at by its own id-index slot; `indexed_` is the exact number of non-null slots (D-044),
// so equal counts mean the index holds nothing else.
void OrderBook::validate() const {
  if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first)
    fail("book is crossed: best bid " + std::to_string(bids_.begin()->first) + " >= best ask " +
         std::to_string(asks_.begin()->first));
  std::size_t orders = 0;
  validate_side(bids_, Side::Buy, orders);
  validate_side(asks_, Side::Sell, orders);
  if (indexed_ != orders)
    fail("id index has " + std::to_string(indexed_) + " non-null entries but " + std::to_string(orders) +
         " orders rest");
  if (pool_.live() != orders)
    fail("pool holds " + std::to_string(pool_.live()) + " live orders but " + std::to_string(orders) + " rest");
}

}  // namespace lob
