// The market maker inside the real simulator, watched by a probe that sees ground truth.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "sim/agents/as_market_maker.hpp"
#include "sim/observer.hpp"
#include "sim/simulator.hpp"
#include "support/sim_support.hpp"

using namespace sim;
using lob::Side;

namespace {

struct ProbeFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

constexpr std::size_t si(Side s) { return s == Side::Buy ? 0 : 1; }
constexpr TimeNs kUs = 1'000;
constexpr TimeNs kMs = 1'000'000;

// Checks the market maker from outside, at every step:
//   1. per side, no request is sent while the reply to the previous one is undelivered;
//   2. at the exchange the market maker never has more than one resting order per side;
//   3. true inventory (engine trades) stays within q_max + quote_size - 1;
//   4. whenever nothing is in flight between the market maker and the exchange (checked
//      at every Sample and at the end), its beliefs equal the truth: inventory, working
//      orders (id, price, remaining), no pending request.
// It also checks that the true book is never crossed. Read-only: it never schedules,
// sends or draws. The first violation throws ProbeFailure, which ends Simulator::run():
// a broken agent can flood the exchange with requests, and the run must stop at once.
class MmProbe final : public Observer {
 public:
  MmProbe(const Simulator& sim, AgentIndex mm_index)
      : sim_(sim),
        index_(mm_index),
        owner_(mm_index + 1),
        q_max_(sim.config().mm->q_max),
        quote_size_(sim.config().mm->quote_size) {}

  void attach(const AvellanedaStoikovMM& mm) { mm_ = &mm; }

  void on_agent_send(TimeNs now, AgentIndex agent, const lob::Request& request) override {
    if (agent != index_) return;
    ++requests_in_flight_;
    Outstanding o;
    Side side = Side::Buy;
    if (const auto* n = std::get_if<lob::NewOrder>(&request)) {
      side = n->side;
      o = {lob::RequestKind::New, n->client_order_id, 0};
      new_orders_[n->client_order_id] = {n->side, n->price, n->qty};
    } else {
      const lob::OrderId id = std::visit([](const auto& r) -> lob::OrderId {
        if constexpr (requires { r.order_id; }) return r.order_id;
        else return 0;
      }, request);
      const auto it = order_side_.find(id);
      if (it == order_side_.end()) {
        violate(now, "modify or cancel for an order the market maker never had");
        return;
      }
      side = it->second;
      o = {std::holds_alternative<lob::Modify>(request) ? lob::RequestKind::Modify : lob::RequestKind::Cancel, 0, id};
      if (o.kind == lob::RequestKind::Modify) {
        const auto rit = resting_[si(side)].find(id);
        if (rit != resting_[si(side)].end()) remaining_at_modify_send_[id] = rit->second.remaining;
        else remaining_at_modify_send_.erase(id);
      }
    }
    if (outstanding_[si(side)])
      violate(now, std::string("pending rule: second request on the ") + lob::to_string(side) +
                       " side before the reply");
    outstanding_[si(side)] = o;
  }

  void on_request(TimeNs, const lob::Request& request) override {
    if (std::visit([](const auto& r) { return r.owner; }, request) == owner_) --requests_in_flight_;
  }

  void on_engine_event(TimeNs now, const lob::Event& event) override {
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, lob::Trade>) {
            if (e.maker_owner == owner_) {
              ++replies_in_flight_;
              if (outstanding_[si(e.aggressor_side)]) ++fill_while_other_side_pending_;
              fill(now, lob::opposite(e.aggressor_side), e.maker_order_id, e.price, e.qty);
            }
            if (e.taker_owner == owner_ && e.taker_owner != e.maker_owner) {
              ++replies_in_flight_;
              ++taker_fills_;
              fill(now, e.aggressor_side, e.taker_order_id, e.price, e.qty);
            }
          } else {
            if (e.owner != owner_) return;
            ++replies_in_flight_;
            if constexpr (std::is_same_v<T, lob::Reject>) {
              if (e.reason == lob::RejectReason::UnknownOrder) {
                if (e.request_kind == lob::RequestKind::Modify) ++modify_raced_by_full_fill_;
                else if (e.request_kind == lob::RequestKind::Cancel) ++cancel_raced_by_full_fill_;
              }
            } else if constexpr (std::is_same_v<T, lob::Ack>) {
              const NewInfo& n = new_orders_.at(e.client_order_id);
              order_side_[e.order_id] = n.side;
              resting_[si(n.side)][e.order_id] = {n.price, n.qty};
            } else if constexpr (std::is_same_v<T, lob::Modified>) {
              auto& r = resting_[si(order_side_.at(e.order_id))];
              // The order's true remaining now (before this event) against what it was when
              // the modify was sent: smaller means a partial fill the agent had not seen.
              const auto sent = remaining_at_modify_send_.find(e.order_id);
              const auto cur = r.find(e.order_id);
              if (sent != remaining_at_modify_send_.end() && cur != r.end() && cur->second.remaining < sent->second)
                ++modify_after_unseen_partial_fill_;
              r[e.order_id] = {e.new_price, e.new_qty};
            } else if constexpr (std::is_same_v<T, lob::Cancelled>) {
              resting_[si(order_side_.at(e.order_id))].erase(e.order_id);
            }
          }
        },
        event);
  }

  void on_book(TimeNs now, const BBO& top, bool) override {
    for (const auto& side : resting_)
      if (side.size() > 1) violate(now, "more than one resting market-maker order on one side");
    if (top.two_sided() && top.bid >= top.ask) violate(now, "the true book is crossed");
  }

  void on_private_delivered(TimeNs now, AgentIndex agent, const lob::Event& event) override {
    if (agent != index_) return;
    --replies_in_flight_;
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, lob::Trade>) {
            return;  // not a reply
          } else if constexpr (std::is_same_v<T, lob::Ack>) {
            answer(now, lob::RequestKind::New, e.client_order_id, 0);
          } else if constexpr (std::is_same_v<T, lob::Reject>) {
            answer(now, e.request_kind, e.request_kind == lob::RequestKind::New ? e.client_order_id : 0,
                   e.request_kind == lob::RequestKind::New ? 0 : e.order_id);
          } else if constexpr (std::is_same_v<T, lob::Modified>) {
            answer(now, lob::RequestKind::Modify, 0, e.order_id);
          } else {
            if (e.reason != lob::CancelReason::User) violate(now, "market-maker order cancelled by the exchange");
            else answer(now, lob::RequestKind::Cancel, 0, e.order_id);
          }
        },
        event);
  }

  void on_sample(TimeNs now) override { compare(now, false); }
  void on_end(TimeNs end) override { compare(end, true); }

  // Results.
  std::uint64_t violations() const { return violations_; }
  const std::string& first_violation() const { return messages_; }
  lob::Qty true_inventory() const { return q_; }
  lob::Qty max_abs_inventory() const { return max_abs_q_; }
  std::uint64_t fills(Side s) const { return fills_[si(s)]; }
  std::uint64_t taker_fills() const { return taker_fills_; }
  // Race paths, counted from engine events and sends (ground truth).
  std::uint64_t modify_raced_by_full_fill() const { return modify_raced_by_full_fill_; }
  std::uint64_t cancel_raced_by_full_fill() const { return cancel_raced_by_full_fill_; }
  std::uint64_t modify_after_unseen_partial_fill() const { return modify_after_unseen_partial_fill_; }
  std::uint64_t fill_while_other_side_pending() const { return fill_while_other_side_pending_; }
  std::uint64_t quiet_checks() const { return quiet_checks_; }
  bool checked_at_end() const { return checked_at_end_; }
  // The market maker's true resting orders at the exchange, from engine events.
  std::size_t resting(Side s) const { return resting_[si(s)].size(); }

  // Compares the probe's resting orders with the exchange's own book.
  void expect_matches_book() const {
    for (const auto& side : resting_)
      for (const auto& [id, r] : side) {
        const auto v = sim_.exchange().book().order(id);
        ASSERT_TRUE(v.has_value()) << "order " << id;
        EXPECT_EQ(v->owner, owner_);
        EXPECT_EQ(v->price, r.price);
        EXPECT_EQ(v->remaining, r.remaining);
      }
  }

 private:
  struct Outstanding {
    lob::RequestKind kind{};
    std::uint64_t client_order_id{0};
    lob::OrderId order_id{0};
  };
  struct NewInfo {
    Side side;
    lob::Price price;
    lob::Qty qty;
  };
  struct Resting {
    lob::Price price;
    lob::Qty remaining;
  };

  [[noreturn]] void violate(TimeNs now, const std::string& what) {
    ++violations_;
    messages_ = "t=" + std::to_string(now) + " ns: " + what;
    throw ProbeFailure(messages_);
  }

  void fill(TimeNs now, Side side, lob::OrderId id, lob::Price, lob::Qty qty) {
    ++fills_[si(side)];
    q_ += side == Side::Buy ? qty : -qty;
    max_abs_q_ = std::max(max_abs_q_, q_ < 0 ? -q_ : q_);
    if (max_abs_q_ > q_max_ + quote_size_ - 1)
      violate(now, "|q| = " + std::to_string(max_abs_q_) + " exceeds q_max + quote_size - 1");
    auto& r = resting_[si(side)];
    const auto it = r.find(id);
    if (it == r.end()) {
      violate(now, "fill of an order that is not resting");
      return;
    }
    it->second.remaining -= qty;
    if (it->second.remaining <= 0) r.erase(it);
  }

  void answer(TimeNs now, lob::RequestKind kind, std::uint64_t cid, lob::OrderId id) {
    for (auto& o : outstanding_) {
      if (o && o->kind == kind && o->client_order_id == cid && o->order_id == id) {
        o.reset();
        return;
      }
    }
    violate(now, std::string("a reply (") + lob::to_string(kind) + ") with no outstanding request");
  }

  void compare(TimeNs now, bool at_end) {
    if (mm_ == nullptr || requests_in_flight_ != 0 || replies_in_flight_ != 0) return;
    ++quiet_checks_;
    if (at_end) checked_at_end_ = true;
    if (mm_->inventory() != q_) violate(now, "believed inventory differs from the truth");
    for (const Side s : {Side::Buy, Side::Sell}) {
      if (outstanding_[si(s)]) violate(now, "nothing in flight but a request is unanswered");
      if (mm_->pending_kind(s) != AvellanedaStoikovMM::PendingKind::None)
        violate(now, std::string("nothing in flight but the agent has a pending request on ") + lob::to_string(s));
      const auto& truth = resting_[si(s)];
      const auto& belief = mm_->working(s);
      if (truth.empty() != !belief.has_value()) {
        violate(now, std::string("working order presence differs on ") + lob::to_string(s));
      } else if (belief) {
        const auto& [id, r] = *truth.begin();
        if (belief->id != id || belief->price != r.price || belief->remaining != r.remaining)
          violate(now, std::string("working order differs on ") + lob::to_string(s));
      }
    }
  }

  const Simulator& sim_;
  AgentIndex index_;
  lob::OwnerId owner_;
  lob::Qty q_max_, quote_size_;
  const AvellanedaStoikovMM* mm_{nullptr};

  std::array<std::optional<Outstanding>, 2> outstanding_{};
  std::map<std::uint64_t, NewInfo> new_orders_;  // client_order_id -> what was asked
  std::map<lob::OrderId, Side> order_side_;
  std::array<std::map<lob::OrderId, Resting>, 2> resting_{};
  std::int64_t requests_in_flight_{0}, replies_in_flight_{0};
  lob::Qty q_{0}, max_abs_q_{0};
  std::array<std::uint64_t, 2> fills_{};
  std::uint64_t taker_fills_{0};
  std::map<lob::OrderId, lob::Qty> remaining_at_modify_send_;  // true remaining when the modify was sent
  std::uint64_t modify_raced_by_full_fill_{0}, cancel_raced_by_full_fill_{0};
  std::uint64_t modify_after_unseen_partial_fill_{0}, fill_while_other_side_pending_{0};
  std::uint64_t violations_{0}, quiet_checks_{0};
  bool checked_at_end_{false};
  std::string messages_;
};

// A test-only trader built to provoke the market maker's races. From its own seeded
// stream it hits and lifts the quotes with bursts of small market orders (leaning one way
// for a while, so the inventory limit is reached again and again), places limit orders
// a few ticks around its local mid (moving the top of book, and sometimes crossing it),
// and cancels its own orders. Its orders use STP CancelResting: a market order removes the
// aggressor's own orders in its way and goes on to hit the market maker. It stops acting
// at `stop_at`, so the run ends quiet.
class Aggressor final : public Agent {
 public:
  Aggressor(double rate, TimeNs stop_at, lob::Price initial) : rate_(rate), stop_at_(stop_at), ref_(initial) {}

  void on_start(Context& ctx) override { next(ctx); }
  void on_wakeup(Context& ctx) override {
    if (ctx.now() >= stop_at_) return;
    next(ctx);
    Xoshiro256StarStar& g = ctx.rng();
    if (g.uniform01() < 0.03) lean_ = lean_ == Side::Buy ? Side::Sell : Side::Buy;
    const double u = g.uniform01();
    if (u < 0.45) {
      const std::int64_t n = g.uniform_int(1, 3);
      for (std::int64_t k = 0; k < n; ++k) {
        const Side side = g.uniform01() < 0.8 ? lean_ : lob::opposite(lean_);
        const lob::Qty qty = g.uniform_int(1, 4);
        lob::NewOrder o = simtest::market(side, qty, next_cid_++);
        o.stp = lob::STP::CancelResting;  // its own resting orders never shield the quotes
        ctx.send(o);
      }
    } else if (u < 0.9) {
      if (bbo_.two_sided()) ref_ = (bbo_.bid + bbo_.ask) / 2;
      const Side side = g.uniform_int(0, 1) == 0 ? Side::Buy : Side::Sell;
      const lob::Price off = g.uniform_int(-2, 4);  // mostly behind the mid, sometimes through it
      const lob::Price price = std::max<lob::Price>(1, side == Side::Buy ? ref_ - off : ref_ + off);
      lob::NewOrder o = simtest::limit(side, price, g.uniform_int(1, 6), next_cid_++);
      o.stp = lob::STP::CancelResting;
      ctx.send(o);
    } else if (!open_.empty()) {
      const auto it = std::next(open_.begin(), g.uniform_int(0, static_cast<std::int64_t>(open_.size()) - 1));
      ctx.send(lob::Cancel{0, *it});
    }
  }
  void on_market_data(Context&, const MarketData& m) override {
    if (const auto* b = std::get_if<BBO>(&m)) bbo_ = *b;
  }
  // Fills leave stale ids in open_ (market orders too); a cancel of one is answered
  // UnknownOrder, which removes it.
  void on_private(Context&, const lob::Event& e) override {
    if (const auto* a = std::get_if<lob::Ack>(&e)) open_.push_back(a->order_id);
    else if (const auto* c = std::get_if<lob::Cancelled>(&e)) erase(c->order_id);
    else if (const auto* r = std::get_if<lob::Reject>(&e)) erase(r->order_id);
  }

 private:
  void next(Context& ctx) { ctx.schedule_wakeup(ctx.now() + seconds_to_ns(ctx.rng().exponential(rate_))); }
  void erase(lob::OrderId id) { open_.erase(std::remove(open_.begin(), open_.end(), id), open_.end()); }

  double rate_;
  TimeNs stop_at_;
  lob::Price ref_;
  BBO bbo_{};
  Side lean_{Side::Buy};
  std::uint64_t next_cid_{1};
  std::vector<lob::OrderId> open_;
};

struct StressResult {
  std::uint64_t violations, quiet_checks;
  bool checked_at_end;
  lob::Qty max_abs_q;
  std::uint64_t buy_fills, sell_fills, modifies, cancels, unknown_rejects, anomalies;
  std::string messages;
  std::uint64_t modify_raced_by_full_fill, cancel_raced_by_full_fill, modify_after_unseen_partial_fill,
      fill_while_other_side_pending;
};

// Market maker (agent 0) + aggressor (agent 1), q_max 10, quote_size 5, large jitter.
StressResult stress(std::uint64_t seed) {
  SimConfig cfg = simtest::empty_config(60.0);
  cfg.seed = seed;
  cfg.sample_interval_s = 0.01;  // a belief-versus-truth check every 10 ms when quiet
  MarketMakerConfig mc;
  mc.gamma = 0.01;  // 0.3 tick of skew per lot at the start: every fill moves the quotes
  mc.kappa = 1.5;
  mc.sigma_ticks_per_sqrt_s = 1.0;
  mc.quote_size = 5;
  mc.q_max = 10;
  mc.requote_interval_ms = 20;
  mc.latency = LatencySpec{100 * kUs, 100 * kUs, 100 * kUs, 400 * kUs};
  cfg.mm = mc;
  finalize(cfg);

  Simulator s(cfg);
  EXPECT_EQ(s.market_maker_index(), std::optional<AgentIndex>{0});
  s.add_agent(std::make_unique<Aggressor>(400.0, cfg.duration_ns - 3'000 * kMs, cfg.initial_price),
              LatencySpec{20 * kUs, 20 * kUs, 20 * kUs, 300 * kUs}, StreamRole::Test, 0);
  const auto& mm = dynamic_cast<const AvellanedaStoikovMM&>(s.agent(0));
  MmProbe probe(s, 0);
  probe.attach(mm);
  s.add_observer(&probe);
  try {
    s.run();
  } catch (const ProbeFailure&) {
    // reported through probe.violations() and probe.first_violation()
  }
  if (probe.violations() == 0) probe.expect_matches_book();
  EXPECT_EQ(probe.taker_fills(), 0u) << "post-only quotes are always the maker";
  return {probe.violations(),
          probe.quiet_checks(),
          probe.checked_at_end(),
          probe.max_abs_inventory(),
          probe.fills(Side::Buy),
          probe.fills(Side::Sell),
          mm.modifies_sent(),
          mm.cancels_sent(),
          mm.rejects(lob::RejectReason::UnknownOrder),
          mm.anomalies(),
          probe.first_violation(),
          probe.modify_raced_by_full_fill(),
          probe.cancel_raced_by_full_fill(),
          probe.modify_after_unseen_partial_fill(),
          probe.fill_while_other_side_pending()};
}

}  // namespace

TEST(MarketMakerStress, ProbeChecksHoldUnderRacesAcrossSeeds) {
  for (const std::uint64_t seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    const StressResult r = stress(seed);
    EXPECT_EQ(r.violations, 0u) << r.messages;
    EXPECT_EQ(r.anomalies, 0u);
    EXPECT_LE(r.max_abs_q, 10 + 5 - 1);
    EXPECT_TRUE(r.checked_at_end) << "the run must end with nothing in flight";
    // The scenario really is a stress test: the limit is reached, fills happen on both
    // sides, and modifies race fills (UnknownOrder replies).
    EXPECT_GE(r.max_abs_q, 10);
    EXPECT_GT(r.buy_fills, 100u);
    EXPECT_GT(r.sell_fills, 100u);
    EXPECT_GT(r.modifies, 100u);
    EXPECT_GT(r.cancels, 100u);
    EXPECT_GT(r.unknown_rejects, 0u);
    EXPECT_GT(r.modify_raced_by_full_fill, 0u);
    EXPECT_GT(r.cancel_raced_by_full_fill, 0u);
    EXPECT_GT(r.modify_after_unseen_partial_fill, 0u);
    EXPECT_GT(r.fill_while_other_side_pending, 0u);
    EXPECT_GT(r.quiet_checks, 100u);
    std::printf("[ stress   ] seed %llu: max|q| %lld, fills %llu buy / %llu sell, modifies %llu, cancels %llu, "
                "UnknownOrder %llu, quiet checks %llu, "
                "modify/full-fill %llu, cancel/full-fill %llu, modify/unseen-partial %llu, "
                "fill/other-side-pending %llu\n",
                static_cast<unsigned long long>(seed), static_cast<long long>(r.max_abs_q),
                static_cast<unsigned long long>(r.buy_fills), static_cast<unsigned long long>(r.sell_fills),
                static_cast<unsigned long long>(r.modifies), static_cast<unsigned long long>(r.cancels),
                static_cast<unsigned long long>(r.unknown_rejects), static_cast<unsigned long long>(r.quiet_checks),
                static_cast<unsigned long long>(r.modify_raced_by_full_fill),
                static_cast<unsigned long long>(r.cancel_raced_by_full_fill),
                static_cast<unsigned long long>(r.modify_after_unseen_partial_fill),
                static_cast<unsigned long long>(r.fill_while_other_side_pending));
  }
}

namespace {

SimConfig default_file_config(double duration_s) {
  SimConfig cfg = load_config(std::string(LOBSIM_SOURCE_DIR) + "/configs/default.json");
  cfg.duration_s = duration_s;
  cfg.warmup_s = 0;
  finalize(cfg);
  return cfg;
}

}  // namespace

TEST(MarketMakerSim, DefaultConfigTradesBothWaysWithinTheRules) {
  const SimConfig cfg = default_file_config(240.0);
  auto run_once = [&](bool check) {
    std::ostringstream log;
    Simulator s(cfg, SimulatorOptions{&log});
    EXPECT_EQ(s.agent_count(), cfg.noise_traders.count + 2u);
    const AgentIndex mi = s.market_maker_index().value();
    EXPECT_EQ(mi, cfg.noise_traders.count + 1u) << "created last";
    const auto& mm = dynamic_cast<const AvellanedaStoikovMM&>(s.agent(mi));
    MmProbe probe(s, mi);
    probe.attach(mm);
    s.add_observer(&probe);
    EXPECT_NO_THROW(s.run());
    if (check) {
      EXPECT_EQ(probe.violations(), 0u) << probe.first_violation();
      EXPECT_EQ(mm.anomalies(), 0u);
      EXPECT_GT(probe.fills(Side::Buy), 0u);
      EXPECT_GT(probe.fills(Side::Sell), 0u);
      EXPECT_LE(probe.max_abs_inventory(), cfg.mm->q_max + cfg.mm->quote_size - 1);
      EXPECT_GT(probe.quiet_checks(), 10u);
      probe.expect_matches_book();
    }
    return std::pair(s.exchange().event_hash(), log.str());
  };
  const auto a = run_once(true);
  const auto b = run_once(false);
  EXPECT_EQ(a.first, b.first);
  EXPECT_EQ(a.second, b.second);
}

TEST(MarketMakerSim, RunWithTheMmSectionDiffersFromRunWithout) {
  const SimConfig with = default_file_config(30.0);
  SimConfig without = with;
  without.mm.reset();
  finalize(without);
  EXPECT_NE(simtest::run(with).hash, simtest::run(without).hash) << "the market maker does act";
}
