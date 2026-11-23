#include "feed/encoder.hpp"

// The feed canary (LOBSIM_FEED_BUG, upgrade 6): never in a normal build.
#if defined(LOBSIM_FEED_BUG_ADD_BEFORE_EXEC) && !defined(LOBSIM_FEED_BUG_BUILD)
#error "LOBSIM_FEED_BUG_* injects a feed bug and is only allowed in a LOBSIM_FEED_BUG build"
#endif

#include <limits>
#include <optional>
#include <variant>

namespace feed {

Encoder::Encoder(std::FILE* out, std::size_t expected_orders, std::size_t buffer_bytes)
    : out_(out), buf_(buffer_bytes < 2 * kFlushBelow ? 2 * kFlushBelow : buffer_bytes) {
  mirror_.reserve(expected_orders + 1);
}

std::byte* Encoder::room() {
  if (buf_.size() - pos_ < kFlushBelow) {
    if (out_ != nullptr) flush();
    else buf_.resize(buf_.size() * 2);  // memory mode only: a request with more output than the buffer
  }
  return buf_.data() + pos_;
}

void Encoder::flush() {
  if (out_ == nullptr || pos_ == 0) return;
  if (std::fwrite(buf_.data(), 1, pos_, out_) != pos_) throw std::runtime_error("cannot write the feed file");
  pos_ = 0;
}

Encoder::Resting& Encoder::slot(lob::OrderId id) {
  if (id >= mirror_.size()) mirror_.resize(id + 1);
  return mirror_[id];
}

std::uint64_t Encoder::timestamp(std::int64_t sim_ns) const {
  if (sim_ns < 0 || static_cast<std::uint64_t>(sim_ns) >= kTimestampLimit - kOpenNs) {
    throw EncodeError(EncodeError::Kind::Overflow, requests_, "time " + std::to_string(sim_ns) + " ns + 9:30 does not fit in 48 bits");
  }
  return kOpenNs + static_cast<std::uint64_t>(sim_ns);
}

std::uint32_t Encoder::shares(lob::Qty q) const {
  if (q <= 0 || q > std::numeric_limits<std::uint32_t>::max()) {
    throw EncodeError(EncodeError::Kind::Overflow, requests_, "quantity " + std::to_string(q) + " does not fit in 32-bit shares");
  }
  return static_cast<std::uint32_t>(q);
}

std::uint32_t Encoder::price(lob::Price p) const {
  constexpr lob::Price kMaxTicks = std::numeric_limits<std::uint32_t>::max() / 100;
  if (p <= 0 || p > kMaxTicks) {
    throw EncodeError(EncodeError::Kind::Overflow, requests_, "price " + std::to_string(p) + " ticks does not fit in a 32-bit Price(4)");
  }
  return static_cast<std::uint32_t>(p * 100);
}

void Encoder::emit_s(char code) {
  pos_ += write(room(), MsgS{Prefix{kLocate, 0, ts_}, code});
  ++counts_.S;
  counts_.bytes += kPrefix + kLenS;
}

void Encoder::emit_a(lob::OrderId id, lob::Side side, lob::Qty qty, lob::Price px) {
  const MsgA m{Prefix{kLocate, 0, ts_}, id, side == lob::Side::Buy ? 'B' : 'S', shares(qty), kStock, price(px)};
  pos_ += write(room(), m);
  ++counts_.A;
  counts_.bytes += kPrefix + kLenA;
  slot(id) = Resting{px, qty, side, true};
}

void Encoder::emit_e(lob::OrderId maker, lob::Qty qty, std::uint64_t match) {
  if (!live(maker)) throw EncodeError(EncodeError::Kind::Inconsistent, requests_, "trade " + std::to_string(match) + " on order " + std::to_string(maker) + ", which is not resting");
  Resting& r = mirror_[maker];
  if (qty > r.remaining) throw EncodeError(EncodeError::Kind::Inconsistent, requests_, "trade " + std::to_string(match) + " is larger than order " + std::to_string(maker));
  pos_ += write(room(), MsgE{Prefix{kLocate, 0, ts_}, maker, shares(qty), match});
  ++counts_.E;
  counts_.bytes += kPrefix + kLenE;
  r.remaining -= qty;
  if (r.remaining == 0) r.live = false;  // executed in full: gone, no D (D-308)
}

void Encoder::emit_x(lob::OrderId id, lob::Qty qty) {
  pos_ += write(room(), MsgX{Prefix{kLocate, 0, ts_}, id, shares(qty)});
  ++counts_.X;
  counts_.bytes += kPrefix + kLenX;
  mirror_[id].remaining -= qty;
}

void Encoder::emit_d(lob::OrderId id) {
  pos_ += write(room(), MsgD{Prefix{kLocate, 0, ts_}, id});
  ++counts_.D;
  counts_.bytes += kPrefix + kLenD;
  mirror_[id].live = false;
}

void Encoder::start(std::int64_t sim_ns) {
  ts_ = timestamp(sim_ns);
  emit_s('O');
}

void Encoder::finish(std::int64_t sim_ns) {
  ts_ = timestamp(sim_ns);
  emit_s('C');
  flush();
}

void Encoder::on_request(std::int64_t sim_ns, const lob::Request& req, std::span<const lob::Event> events) {
  ++requests_;
  ts_ = timestamp(sim_ns);  // one check covers every message of the request

  // The order this request may leave resting (D-306): a GTC limit order, or a priority-losing modify.
  std::optional<lob::OrderId> subject;
  lob::Side side = lob::Side::Buy;
  lob::Price px = 0;
  lob::Qty want = 0;
  lob::Qty taken = 0;
  bool killed = false;
  const auto* n = std::get_if<lob::NewOrder>(&req);
  const bool gtc_limit = n != nullptr && n->type == lob::OrderType::Limit && n->tif == lob::TIF::GTC;

#ifdef LOBSIM_FEED_BUG_ADD_BEFORE_EXEC
  // Injected bug: the taker's A is published before its executions. The remainder is worked out
  // first so the A carries the right size; only its position in the stream is wrong.
  lob::Qty early = 0;
  {
    std::optional<lob::OrderId> s;
    lob::Qty w = 0, t = 0;
    bool k = false;
    for (const lob::Event& ev : events) {
      if (const auto* a = std::get_if<lob::Ack>(&ev); a != nullptr && gtc_limit) s = a->order_id, w = n->qty;
      else if (const auto* m = std::get_if<lob::Modified>(&ev); m != nullptr && !m->priority_kept) s = m->order_id, w = m->new_qty;
      else if (const auto* tr = std::get_if<lob::Trade>(&ev); tr != nullptr && s && tr->taker_order_id == *s) t += tr->qty;
      else if (const auto* c = std::get_if<lob::Cancelled>(&ev); c != nullptr && s && c->order_id == *s) k = true;
    }
    if (s && !k) early = w - t;
  }
  const auto publish_early = [&] {
    if (early > 0) emit_a(*subject, side, early, px);
  };
#endif

  for (const lob::Event& ev : events) {
    if (const auto* a = std::get_if<lob::Ack>(&ev)) {
      if (gtc_limit) {
        subject = a->order_id;
        side = n->side;
        px = n->price;
        want = n->qty;
#ifdef LOBSIM_FEED_BUG_ADD_BEFORE_EXEC
        publish_early();
#endif
      }
    } else if (const auto* m = std::get_if<lob::Modified>(&ev)) {
      if (!live(m->order_id)) throw EncodeError(EncodeError::Kind::Inconsistent, requests_, "modify of order " + std::to_string(m->order_id) + ", which is not resting");
      Resting& r = mirror_[m->order_id];
      if (m->priority_kept) {
        emit_x(m->order_id, r.remaining - m->new_qty);
      } else {
        side = r.side;
        emit_d(m->order_id);
        subject = m->order_id;
        px = m->new_price;
        want = m->new_qty;
#ifdef LOBSIM_FEED_BUG_ADD_BEFORE_EXEC
        publish_early();
#endif
      }
    } else if (const auto* t = std::get_if<lob::Trade>(&ev)) {
      emit_e(t->maker_order_id, t->qty, t->trade_id);
      if (subject && t->taker_order_id == *subject) taken += t->qty;
    } else if (const auto* c = std::get_if<lob::Cancelled>(&ev)) {
      // The subject first: a priority-losing modify has already marked its own entry dead.
      if (subject && c->order_id == *subject) killed = true;  // it never rested: nothing to publish
      else if (live(c->order_id)) emit_d(c->order_id);
    }
    // Reject: nothing.
  }

#ifndef LOBSIM_FEED_BUG_ADD_BEFORE_EXEC
  if (subject && !killed && want - taken > 0) emit_a(*subject, side, want - taken, px);
#else
  static_cast<void>(want);  // the injected bug published the A already
  static_cast<void>(killed);
#endif
}

}  // namespace feed
