// Differential fuzz test (SPEC 4): one seeded stream of valid and invalid requests is fed
// to OrderBook and ReferenceBook. After every message the two event lists must be equal,
// validate() must pass on both, and quantity conservation must hold; every few messages
// the query APIs are compared too. Exits non-zero on the first difference, printing the
// seed, message index, request and both event lists.
//
//   fuzz_diff                          200 seeds x 10,000 messages (what ctest runs)
//   fuzz_diff --first-seed A --seeds K seeds A..A+K-1 x 10,000 messages
//   fuzz_diff --long                   4 seeds x 1,000,000 messages
//   fuzz_diff --seed N --messages M    reproduce one stream
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "lob/reference_book.hpp"
#include "lob/types.hpp"
#include "support/conservation.hpp"
#include "support/splitmix64.hpp"

namespace {

using namespace lob;
using lob::test::ConservationChecker;
using lob::test::SplitMix64;

// A small max_price so both ends of the valid range are reached and orders cross often.
constexpr Price kMaxPrice = 40;
constexpr OwnerId kOwners = 4;
constexpr std::uint64_t kNoPos = std::numeric_limits<std::uint64_t>::max();

// ------------------------------------------------------------------ printing

std::string str(const Request& req) {
  using std::to_string;
  struct V {
    std::string operator()(const NewOrder& r) const {
      return "NewOrder{owner=" + to_string(r.owner) + ", client_order_id=" + to_string(r.client_order_id) +
             ", side=" + lob::to_string(r.side) + ", type=" + lob::to_string(r.type) +
             ", tif=" + lob::to_string(r.tif) + ", price=" + to_string(r.price) + ", qty=" + to_string(r.qty) +
             ", post_only=" + (r.post_only ? "true" : "false") + ", stp=" + lob::to_string(r.stp) + "}";
    }
    std::string operator()(const Cancel& r) const {
      return "Cancel{owner=" + to_string(r.owner) + ", order_id=" + to_string(r.order_id) + "}";
    }
    std::string operator()(const Modify& r) const {
      return "Modify{owner=" + to_string(r.owner) + ", order_id=" + to_string(r.order_id) +
             ", new_price=" + to_string(r.new_price) + ", new_qty=" + to_string(r.new_qty) + "}";
    }
  };
  return std::visit(V{}, req);
}

std::string str(const EventBuffer& events) {
  if (events.empty()) return "    (none)\n";
  std::string s;
  for (const Event& e : events) s += "    " + to_string(e) + "\n";
  return s;
}

std::string str(const std::optional<Price>& p) { return p ? std::to_string(*p) : "none"; }
std::string str(const LevelView& l) { return "(" + std::to_string(l.price) + ", " + std::to_string(l.total_qty) + ")"; }
std::string str(const std::optional<LevelView>& l) { return l ? str(*l) : "none"; }
std::string str(const std::vector<LevelView>& v) {
  std::string s = "[";
  for (std::size_t i = 0; i < v.size(); ++i) s += (i ? " " : "") + str(v[i]);
  return s + "]";
}
std::string str(const std::optional<OrderView>& o) {
  if (!o) return "none";
  return "OrderView{id=" + std::to_string(o->id) + ", owner=" + std::to_string(o->owner) +
         ", side=" + to_string(o->side) + ", price=" + std::to_string(o->price) +
         ", remaining=" + std::to_string(o->remaining) + ", stp=" + to_string(o->stp) +
         ", post_only=" + (o->post_only ? "true" : "false") + "}";
}

// ------------------------------------------------------------------ coverage

struct Coverage {
  std::array<std::uint64_t, 5> events{};   // Event alternative index
  std::array<std::uint64_t, 7> rejects{};  // RejectReason
  std::array<std::uint64_t, 5> cancels{};  // CancelReason
  std::uint64_t modified_kept{0}, modified_lost{0};
  std::uint64_t fok_filled{0}, fok_unfillable{0};
  std::uint64_t stp_taking_new{0}, stp_resting_new{0};
  std::uint64_t stp_taking_modify{0}, stp_resting_modify{0};
  std::uint64_t stp_taking_fok{0}, stp_resting_fok{0};
  std::uint64_t book_emptied{0};
  std::size_t max_resting{0};
  std::uint64_t messages{0};

  // `fok_blocked_by_own`: before this FOK (CancelTaking) arrived, an own order crossed its
  // limit while other owners' crossing quantity alone would have filled it.
  void count(const Request& req, const EventBuffer& ev, bool fok_blocked_by_own) {
    ++messages;
    for (const Event& e : ev) {
      ++events[e.index()];
      if (const auto* r = std::get_if<Reject>(&e)) ++rejects[static_cast<std::size_t>(r->reason)];
      if (const auto* c = std::get_if<Cancelled>(&e)) ++cancels[static_cast<std::size_t>(c->reason)];
      if (const auto* m = std::get_if<Modified>(&e)) ++(m->priority_kept ? modified_kept : modified_lost);
    }
    if (ev.empty()) return;

    if (const auto* n = std::get_if<NewOrder>(&req)) {
      const auto* ack = std::get_if<Ack>(&ev.front());
      if (ack == nullptr) return;
      bool unfillable = false;
      for (const Event& e : ev) {
        const auto* c = std::get_if<Cancelled>(&e);
        if (c == nullptr) continue;
        if (c->reason == CancelReason::FOKUnfillable) unfillable = true;
        if (c->reason != CancelReason::SelfTrade) continue;
        if (c->order_id == ack->order_id) {
          ++stp_taking_new;
        } else {
          ++stp_resting_new;
          if (n->tif == TIF::FOK) ++stp_resting_fok;
        }
      }
      if (n->tif == TIF::FOK) {
        ++(unfillable ? fok_unfillable : fok_filled);
        if (unfillable && fok_blocked_by_own) ++stp_taking_fok;
      }
    } else if (const auto* m = std::get_if<Modify>(&req)) {
      const auto* mod = std::get_if<Modified>(&ev.front());
      if (mod == nullptr || mod->priority_kept) return;
      for (const Event& e : ev) {
        const auto* c = std::get_if<Cancelled>(&e);
        if (c == nullptr || c->reason != CancelReason::SelfTrade) continue;
        ++(c->order_id == m->order_id ? stp_taking_modify : stp_resting_modify);
      }
    }
  }

  // Prints the counts; returns false if `enforce` and anything required never occurred.
  bool report(bool enforce) const {
    bool ok = true;
    auto line = [&](const char* name, std::uint64_t n) {
      std::printf("  %-34s %12llu%s\n", name, static_cast<unsigned long long>(n),
                  n == 0 ? "   <-- NEVER OCCURRED" : "");
      if (n == 0) ok = false;
    };
    std::printf("coverage over %llu messages:\n", static_cast<unsigned long long>(messages));
    const char* event_names[] = {"event Ack", "event Reject", "event Trade", "event Cancelled", "event Modified"};
    for (std::size_t i = 0; i < events.size(); ++i) line(event_names[i], events[i]);
    for (std::size_t i = 0; i < rejects.size(); ++i)
      line(("reject " + std::string(to_string(static_cast<RejectReason>(i)))).c_str(), rejects[i]);
    for (std::size_t i = 0; i < cancels.size(); ++i)
      line(("cancel " + std::string(to_string(static_cast<CancelReason>(i)))).c_str(), cancels[i]);
    line("Modified priority_kept=true", modified_kept);
    line("Modified priority_kept=false", modified_lost);
    line("FOK filled", fok_filled);
    line("FOK unfillable", fok_unfillable);
    line("STP CancelTaking, new order", stp_taking_new);
    line("STP CancelResting, new order", stp_resting_new);
    line("STP CancelTaking, modify re-entry", stp_taking_modify);
    line("STP CancelResting, modify re-entry", stp_resting_modify);
    line("STP CancelTaking, FOK dry run", stp_taking_fok);
    line("STP CancelResting, FOK execution", stp_resting_fok);
    line("book emptied (resting -> 0)", book_emptied);
    std::printf("  %-34s %12zu%s\n", "max resting orders", max_resting,
                max_resting < 200 ? "   <-- BELOW 200" : "");
    if (max_resting < 200) ok = false;
    if (!enforce) return true;
    if (!ok) std::printf("FAIL: coverage incomplete\n");
    return ok;
  }
};

// ------------------------------------------------------------------ generator

// Produces the request stream. It reads the fast book (order(), best prices) to aim
// requests at live orders and near the touch; both books receive the same stream, so
// that read only shapes the input and never decides a verdict.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : rng_(seed) {}

  Request next(const OrderBook& book, std::uint64_t msg) {
    if (regime_left_ == 0) pick_regime(book);
    --regime_left_;
    if (rng_.chance(8)) center_ = std::clamp<Price>(center_ + (rng_.chance(50) ? 1 : -1), 1, kMaxPrice);
    if (regime_ == Regime::Grow && book.resting_orders() > 450) regime_left_ = 0;

    unsigned new_pct = 55, cancel_pct = 20;  // remainder: modify
    if (regime_ == Regime::Grow) new_pct = 88, cancel_pct = 5;
    if (regime_ == Regime::Drain) new_pct = 55, cancel_pct = 38;
    const auto roll = static_cast<unsigned>(rng_.below(100));
    if (roll < new_pct) return new_order(book, msg);
    if (roll < new_pct + cancel_pct) return cancel(book);
    return modify(book);
  }

  // Keeps the live-order list in step with the book after each message.
  void observe(const EventBuffer& events, const OrderBook& book) {
    for (const Event& e : events) {
      std::visit(
          [&](const auto& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, Ack>) {
              max_id_ = std::max(max_id_, ev.order_id);
              sync(ev.order_id, book);
            } else if constexpr (std::is_same_v<T, Trade>) {
              sync(ev.maker_order_id, book);
              sync(ev.taker_order_id, book);
            } else if constexpr (std::is_same_v<T, Cancelled> || std::is_same_v<T, Modified>) {
              sync(ev.order_id, book);
            }
          },
          e);
    }
  }

  // True when a CancelTaking FOK would fill from other owners alone but an own order
  // crosses its limit: the FOK fails only because STP ends the dry run.
  bool fok_blocked_by_own(const NewOrder& r, const OrderBook& book) const {
    if (r.tif != TIF::FOK || r.type != OrderType::Limit || r.stp != STP::CancelTaking || r.qty <= 0) return false;
    bool own = false;
    Qty others = 0;
    for (OrderId id : live_) {
      const auto o = book.order(id);
      if (!o || o->side == r.side) continue;
      const bool crosses = r.side == Side::Buy ? o->price <= r.price : o->price >= r.price;
      if (!crosses) continue;
      if (o->owner == r.owner) own = true;
      else others += o->remaining;
    }
    return own && others >= r.qty;
  }

  const std::vector<OrderId>& live() const { return live_; }
  OrderId max_id() const { return max_id_; }

 private:
  enum class Regime { Grow, Churn, Drain };

  void pick_regime(const OrderBook& book) {
    const auto r = rng_.below(100);
    if (book.resting_orders() < 150 && r < 35) {
      regime_ = Regime::Grow;
      regime_left_ = static_cast<std::uint64_t>(rng_.range(300, 1200));
    } else if (r < 65) {
      regime_ = Regime::Churn;
      regime_left_ = static_cast<std::uint64_t>(rng_.range(200, 1500));
    } else if (r < 85 || book.resting_orders() > 0) {
      regime_ = Regime::Drain;
      regime_left_ = static_cast<std::uint64_t>(rng_.range(150, 600));
    } else {
      regime_ = Regime::Grow;
      regime_left_ = static_cast<std::uint64_t>(rng_.range(300, 1200));
    }
    // Now and then park the action at one end of the price range.
    if (rng_.chance(15)) center_ = rng_.chance(50) ? rng_.range(1, 4) : rng_.range(kMaxPrice - 3, kMaxPrice);
    else if (rng_.chance(30)) center_ = rng_.range(8, kMaxPrice - 8);
  }

  void sync(OrderId id, const OrderBook& book) {
    const bool resting = book.order(id).has_value();
    if (id >= pos_.size()) pos_.resize(std::max<std::size_t>(id + 1, pos_.size() * 2), kNoPos);
    const bool listed = pos_[id] != kNoPos;
    if (resting && !listed) {
      pos_[id] = live_.size();
      live_.push_back(id);
    } else if (!resting && listed) {
      const std::uint64_t at = pos_[id];
      live_[at] = live_.back();
      pos_[live_[at]] = at;
      live_.pop_back();
      pos_[id] = kNoPos;
    }
  }

  OwnerId owner() { return static_cast<OwnerId>(rng_.range(1, kOwners)); }
  STP stp() { return rng_.chance(50) ? STP::CancelTaking : STP::CancelResting; }
  Side side() { return rng_.chance(50) ? Side::Buy : Side::Sell; }

  Price clamp_price(Price p) const { return std::clamp<Price>(p, 1, kMaxPrice); }

  Price invalid_price() {
    switch (rng_.below(7)) {
      case 0: return 0;
      case 1: return -1;
      case 2: return -rng_.range(2, 100);
      case 3: return kMaxPrice + 1;
      case 4: return kMaxPrice + rng_.range(2, 100);
      case 5: return std::numeric_limits<Price>::min();
      default: return std::numeric_limits<Price>::max();
    }
  }

  Qty qty() {
    const auto r = rng_.below(100);
    if (r < 3) return 0;
    if (r < 5) return rng_.chance(80) ? -rng_.range(1, 10) : std::numeric_limits<Qty>::min();
    if (r < 15) return rng_.range(10, 60);
    return rng_.range(1, 10);
  }

  // Priced around the centre; a few percent outside [1, max_price].
  Price price_near(int width) {
    if (rng_.chance(4)) return invalid_price();
    return clamp_price(center_ + rng_.range(-width, width));
  }

  // A buy below / sell above the centre: usually rests without trading.
  Price passive_price(Side s) {
    if (rng_.chance(3)) return invalid_price();
    const Price d = rng_.range(1, 15);
    return clamp_price(s == Side::Buy ? center_ - d : center_ + d);
  }

  // Near the opposite best, so "would cross" is true about half of the time.
  Price touch_price(const OrderBook& book, Side s, int behind, int through) {
    const auto best = s == Side::Buy ? book.best_ask() : book.best_bid();
    if (!best) return price_near(3);
    const Price d = rng_.range(-behind, through);
    return clamp_price(s == Side::Buy ? *best + d : *best - d);
  }

  // Every random draw is its own statement or sits inside a braced initializer (which
  // is evaluated left to right): function arguments are evaluated in an unspecified
  // order, which would make the stream depend on the compiler.
  NewOrder make(Side s, OrderType type, TIF tif, Price price, Qty q, bool post_only, std::uint64_t msg) {
    const OwnerId who = owner();
    const STP mode = stp();
    return NewOrder{who, msg + 1, s, type, tif, price, q, post_only, mode};
  }

  NewOrder new_order(const OrderBook& book, std::uint64_t msg) {
    const Side s = side();
    const auto r = rng_.below(100);
    if (regime_ == Regime::Grow) {
      if (r < 85) {
        const Price p = passive_price(s);
        return make(s, OrderType::Limit, TIF::GTC, p, qty(), false, msg);
      }
      if (r < 93) {
        const Price p = price_near(3);
        return make(s, OrderType::Limit, TIF::GTC, p, qty(), false, msg);
      }
      const Price p = touch_price(book, s, 3, 1);
      return make(s, OrderType::Limit, TIF::GTC, p, qty(), true, msg);
    }
    if (regime_ == Regime::Drain) {
      if (r < 25) {
        const Price p = passive_price(s);
        return make(s, OrderType::Limit, TIF::GTC, p, rng_.range(1, 5), false, msg);
      }
      NewOrder o;
      if (r < 70) {
        const Price junk = rng_.chance(50) ? 0 : invalid_price();
        o = make(s, OrderType::Market, TIF::IOC, junk, rng_.range(5, 200), false, msg);
      } else {
        o = make(s, OrderType::Limit, TIF::IOC, s == Side::Buy ? kMaxPrice : 1, rng_.range(5, 200), false, msg);
      }
      if (rng_.chance(70)) o.stp = STP::CancelResting;
      return o;
    }
    // Churn.
    if (r < 33) {
      const Price p = price_near(3);
      return make(s, OrderType::Limit, TIF::GTC, p, qty(), false, msg);
    }
    if (r < 47) {
      const Price p = price_near(3);
      return make(s, OrderType::Limit, TIF::IOC, p, qty(), false, msg);
    }
    if (r < 62) {
      const Qty q = rng_.chance(90) ? rng_.range(1, 25) : qty();
      return make(s, OrderType::Limit, TIF::FOK, touch_price(book, s, 1, 3), q, false, msg);
    }
    if (r < 74) {
      const Price junk = rng_.chance(50) ? 0 : (rng_.chance(50) ? invalid_price() : price_near(5));
      return make(s, OrderType::Market, TIF::IOC, junk, qty(), false, msg);
    }
    if (r < 90) {
      const Price p = rng_.chance(5) ? invalid_price() : touch_price(book, s, 3, 1);
      return make(s, OrderType::Limit, TIF::GTC, p, qty(), true, msg);
    }
    // Invalid flag combinations (and some with a bad qty or price as well).
    const Price p = price_near(3);
    NewOrder o = make(s, OrderType::Limit, TIF::GTC, p, qty(), false, msg);
    switch (rng_.below(6)) {
      case 0: o.type = OrderType::Market; o.tif = TIF::IOC; o.post_only = true; break;
      case 1: o.tif = TIF::IOC; o.post_only = true; break;
      case 2: o.tif = TIF::FOK; o.post_only = true; break;
      case 3: o.type = OrderType::Market; o.tif = TIF::GTC; break;
      case 4: o.type = OrderType::Market; o.tif = TIF::FOK; break;
      default: o.type = OrderType::Market; o.tif = TIF::GTC; o.post_only = true; break;
    }
    return o;
  }

  std::optional<OrderId> random_live() {
    if (live_.empty()) return std::nullopt;
    return live_[rng_.below(live_.size())];
  }

  // Any id in [1, max_id] that is not resting now: filled, cancelled or never rested.
  OrderId dead_id() {
    if (max_id_ == 0) return 1;
    for (int tries = 0; tries < 8; ++tries) {
      const OrderId id = 1 + rng_.below(max_id_);
      if (id >= pos_.size() || pos_[id] == kNoPos) return id;
    }
    return 1 + rng_.below(max_id_);
  }

  OrderId unassigned_id() {
    switch (rng_.below(4)) {
      case 0: return 0;
      case 1: return max_id_ + 1 + rng_.below(50);
      case 2: return std::numeric_limits<OrderId>::max();
      default: return max_id_ + 1000000;
    }
  }

  // The target of a cancel/modify and the owner sending it.
  std::pair<OrderId, OwnerId> target(const OrderBook& book) {
    const auto r = rng_.below(100);
    if (r < 75) {
      const auto id = random_live();
      if (const auto o = id ? book.order(*id) : std::nullopt) {
        const OwnerId real = o->owner;
        if (r < 65) return {*id, real};
        OwnerId other = static_cast<OwnerId>(rng_.range(1, kOwners));
        if (other == real) other = real % kOwners + 1;
        return {*id, rng_.chance(80) ? other : 99};
      }
    }
    if (r < 88) return {dead_id(), owner()};
    return {unassigned_id(), owner()};
  }

  Cancel cancel(const OrderBook& book) {
    const auto [id, who] = target(book);
    return Cancel{who, id};
  }

  Modify modify(const OrderBook& book) {
    // Post-only orders moved through the touch, so that PostOnlyWouldCross is common.
    if (rng_.chance(12)) {
      for (int tries = 0; tries < 6 && !live_.empty(); ++tries) {
        const auto o = book.order(live_[rng_.below(live_.size())]);
        if (!o || !o->post_only) continue;
        return Modify{o->owner, o->id, touch_price(book, o->side, 1, 2), rng_.chance(50) ? o->remaining : qty()};
      }
    }
    const auto [id, who] = target(book);
    const auto o = book.order(id);
    if (!o) return Modify{who, id, price_near(3), qty()};

    const Price p = o->price;
    const Qty rem = o->remaining;
    switch (rng_.below(10)) {
      case 0:
      case 1:  // reduce in place (or NoChange when there is nothing to reduce)
        return Modify{who, id, p, rem > 1 ? rng_.range(1, rem - 1) : rem};
      case 2:  // increase at the same price
        return Modify{who, id, p, rem + rng_.range(1, 10)};
      case 3:  // no change
        return Modify{who, id, p, rem};
      case 4:  // new, usually non-crossing price
        return Modify{who, id, clamp_price(o->side == Side::Buy ? p - rng_.range(1, 3) : p + rng_.range(1, 3)),
                      rng_.chance(50) ? rem : rng_.range(1, 12)};
      case 5:
      case 6:  // to or through the opposite best: trades, STP or PostOnlyWouldCross
        return Modify{who, id, touch_price(book, o->side, 1, 3), rng_.chance(50) ? rem : rng_.range(1, 30)};
      case 7:  // new price and qty anywhere near the centre
        return Modify{who, id, price_near(4), rng_.range(1, 20)};
      case 8:  // bad qty
        return Modify{who, id, rng_.chance(70) ? p : invalid_price(), rng_.chance(70) ? 0 : -rng_.range(1, 5)};
      default:  // bad price
        return Modify{who, id, invalid_price(), rng_.chance(80) ? rem : rng_.range(1, 5)};
    }
  }

  SplitMix64 rng_;
  Regime regime_{Regime::Churn};
  std::uint64_t regime_left_{0};
  Price center_{kMaxPrice / 2};
  std::vector<OrderId> live_;
  std::vector<std::uint64_t> pos_;  // by OrderId: index into live_, or kNoPos
  OrderId max_id_{0};
};

// ------------------------------------------------------------------ comparison

template <class T>
std::string differ(const char* what, const T& fast, const T& ref) {
  if (fast == ref) return {};
  return std::string(what) + " differs: OrderBook " + str(fast) + ", ReferenceBook " + str(ref);
}

std::string compare_order(OrderId id, const OrderBook& f, const ReferenceBook& r) {
  return differ(("order(" + std::to_string(id) + ")").c_str(), f.order(id), r.order(id));
}

std::string compare_queries(const OrderBook& f, const ReferenceBook& r, const Generator& g, SplitMix64& qrng,
                            bool full_depth) {
  std::string d;
  if (!(d = differ("best_bid()", f.best_bid(), r.best_bid())).empty()) return d;
  if (!(d = differ("best_ask()", f.best_ask(), r.best_ask())).empty()) return d;
  if (!(d = differ("best_level(Buy)", f.best_level(Side::Buy), r.best_level(Side::Buy))).empty()) return d;
  if (!(d = differ("best_level(Sell)", f.best_level(Side::Sell), r.best_level(Side::Sell))).empty()) return d;
  if (!(d = differ("depth(Buy, 5)", f.depth(Side::Buy, 5), r.depth(Side::Buy, 5))).empty()) return d;
  if (!(d = differ("depth(Sell, 5)", f.depth(Side::Sell, 5), r.depth(Side::Sell, 5))).empty()) return d;
  if (full_depth) {
    constexpr std::size_t all = std::numeric_limits<std::size_t>::max();
    if (!(d = differ("depth(Buy, all)", f.depth(Side::Buy, all), r.depth(Side::Buy, all))).empty()) return d;
    if (!(d = differ("depth(Sell, all)", f.depth(Side::Sell, all), r.depth(Side::Sell, all))).empty()) return d;
  }
  if (f.resting_orders() != r.resting_orders())
    return "resting_orders() differs: OrderBook " + std::to_string(f.resting_orders()) + ", ReferenceBook " +
           std::to_string(r.resting_orders());
  const auto& live = g.live();
  for (int i = 0; i < 3 && !live.empty(); ++i)
    if (!(d = compare_order(live[qrng.below(live.size())], f, r)).empty()) return d;
  if (g.max_id() > 0)
    for (int i = 0; i < 2; ++i)
      if (!(d = compare_order(1 + qrng.below(g.max_id()), f, r)).empty()) return d;
  if (!(d = compare_order(g.max_id() + 1 + qrng.below(10), f, r)).empty()) return d;
  return compare_order(0, f, r);
}

// ------------------------------------------------------------------ one seed

struct Options {
  std::uint64_t first_seed{1};
  std::uint64_t seeds{200};
  std::uint64_t messages{10'000};
  bool enforce_coverage{true};
  // Conservation runs on every message of every seed through check_fast(). On every
  // `full_scan_every`-th seed (0 = never) check(), which rescans every id ever seen, runs
  // beside it and the two verdicts must agree on every message.
  std::uint64_t full_scan_every{20};
};

bool run_seed(std::uint64_t seed, const Options& opt, Coverage& cov) {
  OrderBook fast(BookConfig{kMaxPrice});
  ReferenceBook ref(BookConfig{kMaxPrice});
  Generator gen(seed);
  SplitMix64 qrng(seed ^ 0x5eed5eed5eed5eedULL);  // query sampling never perturbs the stream
  ConservationChecker checker;
  ConservationChecker full_checker;
  const bool full_scan = opt.full_scan_every != 0 && (seed - opt.first_seed) % opt.full_scan_every == 0;
  EventHasher hash_fast, hash_ref;
  EventBuffer ev_fast, ev_ref;
  ev_fast.reserve(512);
  ev_ref.reserve(512);
  const auto lookup = [&fast](OrderId id) { return fast.order(id); };
  std::size_t prev_resting = 0;

  for (std::uint64_t i = 0; i < opt.messages; ++i) {
    const Request req = gen.next(fast, i);
    const auto* n = std::get_if<NewOrder>(&req);
    const bool fok_blocked = n != nullptr && gen.fok_blocked_by_own(*n, fast);

    ev_fast.clear();
    ev_ref.clear();
    std::string err;
    try {
      apply(fast, req, ev_fast);
    } catch (const std::exception& e) {
      err = std::string("OrderBook threw: ") + e.what();
    }
    try {
      apply(ref, req, ev_ref);
    } catch (const std::exception& e) {
      if (err.empty()) err = std::string("ReferenceBook threw: ") + e.what();
    }
    if (err.empty() && ev_fast != ev_ref) err = "event lists differ";
    if (err.empty()) {
      try {
        fast.validate();
      } catch (const std::exception& e) {
        err = std::string("OrderBook::validate: ") + e.what();
      }
    }
    if (err.empty()) {
      try {
        ref.validate();
      } catch (const std::exception& e) {
        err = std::string("ReferenceBook::validate: ") + e.what();
      }
    }
    if (err.empty()) {
      const auto bad = checker.check_fast(req, ev_fast, lookup, fast.resting_orders());
      for (const std::string& b : bad) err += (err.empty() ? "conservation: " : "; ") + b;
      if (full_scan) {
        const auto full = full_checker.check(req, ev_fast, lookup);
        for (const std::string& b : full) err += (err.empty() ? "conservation (full scan): " : "; ") + b;
        if (full.empty() != bad.empty()) err += "; check() and check_fast() disagree";
      }
    }
    if (err.empty() && i % 3 == 0) err = compare_queries(fast, ref, gen, qrng, i % 50 == 0);

    if (!err.empty()) {
      std::printf("MISMATCH seed=%llu message=%llu: %s\n  request: %s\n  OrderBook events:\n%s"
                  "  ReferenceBook events:\n%s  reproduce: fuzz_diff --seed %llu --messages %llu\n",
                  static_cast<unsigned long long>(seed), static_cast<unsigned long long>(i), err.c_str(),
                  str(req).c_str(), str(ev_fast).c_str(), str(ev_ref).c_str(),
                  static_cast<unsigned long long>(seed), static_cast<unsigned long long>(i + 1));
      return false;
    }

    hash_fast.add(ev_fast);
    hash_ref.add(ev_ref);
    cov.count(req, ev_fast, fok_blocked);
    const std::size_t resting = fast.resting_orders();
    cov.max_resting = std::max(cov.max_resting, resting);
    if (prev_resting > 0 && resting == 0) ++cov.book_emptied;
    prev_resting = resting;
    gen.observe(ev_fast, fast);
  }

  // Final sweep: every id ever assigned, and full depth.
  std::string err = compare_queries(fast, ref, gen, qrng, true);
  for (OrderId id = 1; err.empty() && id <= gen.max_id(); ++id) err = compare_order(id, fast, ref);
  if (err.empty() && hash_fast.value() != hash_ref.value()) err = "event hashes differ";
  if (!err.empty()) {
    std::printf("MISMATCH seed=%llu after %llu messages: %s\n", static_cast<unsigned long long>(seed),
                static_cast<unsigned long long>(opt.messages), err.c_str());
    return false;
  }
  return true;
}

bool parse_u64(const char* s, std::uint64_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || *end != '\0') return false;
  out = v;
  return true;
}

int usage() {
  std::fprintf(stderr,
               "usage: fuzz_diff [--long] [--seed N --messages M] [--first-seed A --seeds K] [--messages M]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  bool single = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](std::uint64_t& out) { return i + 1 < argc && parse_u64(argv[++i], out); };
    if (a == "--long") {
      opt.first_seed = 1'000'001;
      opt.seeds = 4;
      opt.messages = 1'000'000;
      // check() rescans every id ever seen on every message: quadratic over 10^6 messages.
      opt.full_scan_every = 0;
    } else if (a == "--seed") {
      if (!value(opt.first_seed)) return usage();
      opt.seeds = 1;
      single = true;
    } else if (a == "--messages") {
      if (!value(opt.messages)) return usage();
    } else if (a == "--first-seed") {
      if (!value(opt.first_seed)) return usage();
    } else if (a == "--seeds") {
      if (!value(opt.seeds)) return usage();
    } else {
      return usage();
    }
  }
  if (single) opt.enforce_coverage = false;  // one short stream need not reach every path

  std::printf("fuzz_diff: seeds %llu..%llu, %llu messages each, max_price %lld, %u owners, "
              "full conservation scan every %llu seeds\n",
              static_cast<unsigned long long>(opt.first_seed),
              static_cast<unsigned long long>(opt.first_seed + opt.seeds - 1),
              static_cast<unsigned long long>(opt.messages), static_cast<long long>(kMaxPrice), kOwners,
              static_cast<unsigned long long>(opt.full_scan_every));
  std::fflush(stdout);

  const auto t0 = std::chrono::steady_clock::now();
  Coverage cov;
  for (std::uint64_t s = 0; s < opt.seeds; ++s) {
    if (!run_seed(opt.first_seed + s, opt, cov)) return 1;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("all %llu seeds identical (%llu messages) in %.1f s\n", static_cast<unsigned long long>(opt.seeds),
              static_cast<unsigned long long>(cov.messages), secs);
  return cov.report(opt.enforce_coverage) ? 0 : 1;
}
