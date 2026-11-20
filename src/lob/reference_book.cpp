#include "lob/reference_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace lob {

ReferenceBook::ReferenceBook(BookConfig cfg) : cfg_(cfg) {}

// ---------------------------------------------------------------- helpers

bool ReferenceBook::has_priority_over(const Resting& a, const Resting& b) const {
  if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
  return a.priority_seq < b.priority_seq;
}

std::size_t ReferenceBook::find_order(OrderId id) const {
  for (std::size_t i = 0; i < orders_.size(); ++i)
    if (orders_[i].id == id) return i;
  return orders_.size();
}

std::size_t ReferenceBook::find_best(Side side) const {
  std::size_t best = orders_.size();
  for (std::size_t i = 0; i < orders_.size(); ++i) {
    if (orders_[i].side != side) continue;
    if (best == orders_.size() || has_priority_over(orders_[i], orders_[best])) best = i;
  }
  return best;
}

// Post-only "would cross": buy price >= best ask, or sell price <= best bid (3.3.5, D-023).
bool ReferenceBook::would_cross(Side side, Price price) const {
  std::size_t best = find_best(opposite(side));
  if (best == orders_.size()) return false;
  return side == Side::Buy ? price >= orders_[best].price : price <= orders_[best].price;
}

bool ReferenceBook::crosses(const Taker& t, Price maker_price) const {
  if (t.is_market) return true;
  return t.side == Side::Buy ? maker_price <= t.limit : maker_price >= t.limit;
}

// ---------------------------------------------------------------- matching

// FOK dry run (3.3.4, 3.4): walk the opposite side in priority order, summing what the
// taker could fill. Nothing is changed.
bool ReferenceBook::fok_fillable(const Taker& t) const {
  std::vector<Resting> makers;
  for (const Resting& o : orders_)
    if (o.side == opposite(t.side) && crosses(t, o.price)) makers.push_back(o);
  std::sort(makers.begin(), makers.end(),
            [this](const Resting& a, const Resting& b) { return has_priority_over(a, b); });

  Qty found = 0;
  for (const Resting& m : makers) {
    if (found >= t.remaining) break;
    if (m.owner == t.owner) {
      if (t.stp == STP::CancelResting) continue;  // would be cancelled, adds nothing
      break;                                      // CancelTaking: the walk ends here
    }
    found += m.remaining;
  }
  return found >= t.remaining;
}

// Matches `t` against the opposite side (3.3.2, 3.4). Emits Trades and STP cancels in
// match order; the caller decides what happens to any remainder.
ReferenceBook::MatchResult ReferenceBook::match(Taker& t, EventBuffer& out) {
  MatchResult result;
  while (t.remaining > 0) {
    std::size_t i = find_best(opposite(t.side));
    if (i == orders_.size() || !crosses(t, orders_[i].price)) break;
    Resting& maker = orders_[i];

    if (maker.owner == t.owner) {
      if (t.stp == STP::CancelTaking) {
        result.stopped_by_stp = true;
        break;
      }
      out.push_back(Cancelled{maker.id, maker.owner, maker.remaining, CancelReason::SelfTrade});
      orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }

    Qty qty = std::min(t.remaining, maker.remaining);
    out.push_back(Trade{next_trade_id_++, maker.price, qty, maker.id, t.id, maker.owner, t.owner,
                        t.side});
    t.remaining -= qty;
    maker.remaining -= qty;
    result.filled += qty;
    if (maker.remaining == 0) orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
  }
  return result;
}

void ReferenceBook::rest(const Taker& t, bool post_only) {
  orders_.push_back(Resting{t.id, t.owner, t.side, t.limit, t.remaining, t.stp, post_only,
                            next_priority_seq_++});
}

// ---------------------------------------------------------------- submit

void ReferenceBook::submit(const NewOrder& req, EventBuffer& out) {
  do_submit(req, out);
#ifndef NDEBUG
  validate();
#endif
}

void ReferenceBook::do_submit(const NewOrder& req, EventBuffer& out) {
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

  MatchResult result = match(t, out);
  if (t.remaining == 0) return;

  if (req.tif == TIF::GTC && !result.stopped_by_stp) {
    rest(t, req.post_only);
    return;
  }

  // Remainder is cancelled; the reason follows D-021 and 3.4.
  CancelReason reason = CancelReason::IOCRemainder;
  if (result.stopped_by_stp) reason = CancelReason::SelfTrade;
  else if (is_market && result.filled == 0) reason = CancelReason::NoLiquidity;
  out.push_back(Cancelled{id, req.owner, t.remaining, reason});
}

// ---------------------------------------------------------------- cancel

void ReferenceBook::cancel(const Cancel& req, EventBuffer& out) {
  do_cancel(req, out);
#ifndef NDEBUG
  validate();
#endif
}

void ReferenceBook::do_cancel(const Cancel& req, EventBuffer& out) {
  auto reject = [&](RejectReason reason) {
    out.push_back(Reject{req.owner, RequestKind::Cancel, 0, req.order_id, reason});
  };
  std::size_t i = find_order(req.order_id);
  if (i == orders_.size()) return reject(RejectReason::UnknownOrder);
  if (orders_[i].owner != req.owner) return reject(RejectReason::NotOwner);

  out.push_back(Cancelled{orders_[i].id, orders_[i].owner, orders_[i].remaining, CancelReason::User});
  orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
}

// ---------------------------------------------------------------- modify

void ReferenceBook::modify(const Modify& req, EventBuffer& out) {
  do_modify(req, out);
#ifndef NDEBUG
  validate();
#endif
}

void ReferenceBook::do_modify(const Modify& req, EventBuffer& out) {
  auto reject = [&](RejectReason reason) {
    out.push_back(Reject{req.owner, RequestKind::Modify, 0, req.order_id, reason});
  };
  std::size_t i = find_order(req.order_id);
  if (i == orders_.size()) return reject(RejectReason::UnknownOrder);
  Resting& o = orders_[i];
  if (o.owner != req.owner) return reject(RejectReason::NotOwner);
  if (req.new_qty <= 0) return reject(RejectReason::InvalidQty);
  if (!price_in_range(req.new_price)) return reject(RejectReason::InvalidPrice);
  if (req.new_price == o.price && req.new_qty == o.remaining) return reject(RejectReason::NoChange);
  if (o.post_only && would_cross(o.side, req.new_price))
    return reject(RejectReason::PostOnlyWouldCross);

  // Same price, smaller qty: reduce in place, keep priority.
  if (req.new_price == o.price && req.new_qty < o.remaining) {
    o.remaining = req.new_qty;
    out.push_back(Modified{o.id, o.owner, o.price, req.new_qty, true});
    return;
  }

  // Anything else loses priority: remove and re-enter as a new arrival (same OrderId),
  // keeping stp and post_only. It may trade on re-entry as a taker.
  const Resting old = o;
  orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
  out.push_back(Modified{old.id, old.owner, req.new_price, req.new_qty, false});

  Taker t{old.id, old.owner, old.side, false, req.new_price, req.new_qty, old.stp};
  MatchResult result = match(t, out);
  if (t.remaining == 0) return;
  if (result.stopped_by_stp) {
    out.push_back(Cancelled{t.id, t.owner, t.remaining, CancelReason::SelfTrade});
    return;
  }
  rest(t, old.post_only);
}

// ---------------------------------------------------------------- book builder

Result<OrderId> ReferenceBook::insert_passive(Side side, Price price, Qty qty, OwnerId owner) {
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

Status ReferenceBook::reduce(OrderId id, Qty qty) {
  if (qty <= 0) return Status::InvalidQty;
  const std::size_t i = find_order(id);
  if (i == orders_.size()) return Status::UnknownOrder;
  Status status = Status::Ok;
  if (qty >= orders_[i].remaining) {
    if (qty > orders_[i].remaining) status = Status::Oversized;  // D-152: it is removed all the same
    orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
  } else {
    orders_[i].remaining -= qty;
  }
#ifndef NDEBUG
  validate();
#endif
  return status;
}

Status ReferenceBook::remove(OrderId id) {
  const std::size_t i = find_order(id);
  if (i == orders_.size()) return Status::UnknownOrder;
  orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
#ifndef NDEBUG
  validate();
#endif
  return Status::Ok;
}

std::vector<Fill> ReferenceBook::preview_match(Side taker_side, Price limit, Qty qty) const {
  std::vector<Fill> fills;
  if (qty <= 0) return fills;
  std::vector<Resting> makers;
  for (const Resting& o : orders_)
    if (o.side == opposite(taker_side)) makers.push_back(o);
  std::sort(makers.begin(), makers.end(),
            [this](const Resting& a, const Resting& b) { return has_priority_over(a, b); });
  Qty left = qty;
  for (const Resting& m : makers) {
    if (left <= 0) break;
    if (taker_side == Side::Buy ? m.price > limit : m.price < limit) break;
    const Qty take = std::min(left, m.remaining);
    fills.push_back(Fill{m.id, m.price, take});
    left -= take;
  }
  return fills;
}

// ---------------------------------------------------------------- queries

std::optional<Price> ReferenceBook::best_bid() const {
  std::size_t i = find_best(Side::Buy);
  if (i == orders_.size()) return std::nullopt;
  return orders_[i].price;
}

std::optional<Price> ReferenceBook::best_ask() const {
  std::size_t i = find_best(Side::Sell);
  if (i == orders_.size()) return std::nullopt;
  return orders_[i].price;
}

std::optional<LevelView> ReferenceBook::best_level(Side side) const {
  std::size_t best = find_best(side);
  if (best == orders_.size()) return std::nullopt;
  LevelView level{orders_[best].price, 0};
  for (const Resting& o : orders_)
    if (o.side == side && o.price == level.price) level.total_qty += o.remaining;
  return level;
}

std::vector<LevelView> ReferenceBook::depth(Side side, std::size_t n) const {
  std::vector<Resting> mine;
  for (const Resting& o : orders_)
    if (o.side == side) mine.push_back(o);
  std::sort(mine.begin(), mine.end(),
            [this](const Resting& a, const Resting& b) { return has_priority_over(a, b); });

  std::vector<LevelView> levels;
  for (const Resting& o : mine) {
    if (!levels.empty() && levels.back().price == o.price) {
      levels.back().total_qty += o.remaining;
    } else {
      if (levels.size() == n) break;
      levels.push_back(LevelView{o.price, o.remaining});
    }
  }
  return levels;
}

std::optional<OrderView> ReferenceBook::order(OrderId id) const {
  std::size_t i = find_order(id);
  if (i == orders_.size()) return std::nullopt;
  const Resting& o = orders_[i];
  return OrderView{o.id, o.owner, o.side, o.price, o.remaining, o.stp, o.post_only};
}

std::vector<OrderView> ReferenceBook::level_orders(Side side, Price price) const {
  std::vector<Resting> mine;
  for (const Resting& o : orders_)
    if (o.side == side && o.price == price) mine.push_back(o);
  std::sort(mine.begin(), mine.end(),
            [](const Resting& a, const Resting& b) { return a.priority_seq < b.priority_seq; });
  std::vector<OrderView> views;
  views.reserve(mine.size());
  for (const Resting& o : mine)
    views.push_back(OrderView{o.id, o.owner, o.side, o.price, o.remaining, o.stp, o.post_only});
  return views;
}

std::size_t ReferenceBook::resting_orders() const { return orders_.size(); }

// ---------------------------------------------------------------- validate

void ReferenceBook::validate() const {
  auto fail = [](const std::string& what) { throw std::logic_error("ReferenceBook: " + what); };

  std::vector<OrderId> ids;
  ids.reserve(orders_.size());
  for (const Resting& o : orders_) {
    if (o.remaining <= 0) fail("order " + std::to_string(o.id) + " has remaining <= 0");
    if (!price_in_range(o.price)) fail("order " + std::to_string(o.id) + " has price outside [1, max_price]");
    ids.push_back(o.id);
  }
  // Duplicate ids, found by sorting a copy: O(n log n) instead of comparing every pair.
  std::sort(ids.begin(), ids.end());
  auto dup = std::adjacent_find(ids.begin(), ids.end());
  if (dup != ids.end()) fail("duplicate order id " + std::to_string(*dup));
  std::size_t bid = find_best(Side::Buy);
  std::size_t ask = find_best(Side::Sell);
  if (bid != orders_.size() && ask != orders_.size() && orders_[bid].price >= orders_[ask].price)
    fail("book is crossed");
}

}  // namespace lob
