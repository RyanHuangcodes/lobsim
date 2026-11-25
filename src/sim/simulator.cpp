#include "sim/simulator.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

#include "sim/agents/as_market_maker.hpp"
#include "sim/agents/calibrated_flow.hpp"
#include "sim/agents/informed_trader.hpp"
#include "sim/agents/noise_trader.hpp"

namespace sim {

// One agent with its three channels and its two kernel-owned random streams.
struct Simulator::Slot {
  std::unique_ptr<Agent> agent;
  Channel entry, response, feed;
  Xoshiro256StarStar decision_rng;
  Xoshiro256StarStar latency_rng;
  std::unique_ptr<AgentContext> ctx;
  lob::OwnerId extra_first{0};   // the agent's extra owner ids are [extra_first, extra_first + extra_count)
  std::uint32_t extra_count{0};
};

class Simulator::AgentContext final : public Context {
 public:
  AgentContext(Simulator& sim, AgentIndex index) : sim_(sim), index_(index) {}

  TimeNs now() const override { return sim_.now_; }

  void send(const lob::Request& request) override {
    Slot& slot = *sim_.agents_[index_];
    for (Observer* o : sim_.observers_) o->on_agent_send(sim_.now_, index_, request);
    const TimeNs at = slot.entry.arrival(sim_.now_, slot.latency_rng);
    sim_.queue_.schedule(at, EventKind::RequestArrival, index_, request);
  }

  void schedule_wakeup(TimeNs at) override {
    if (at < sim_.now_) throw std::logic_error("Context: wakeup scheduled in the past");
    sim_.queue_.schedule(at, EventKind::Wakeup, index_);
  }

  Xoshiro256StarStar& rng() override { return sim_.agents_[index_]->decision_rng; }
  lob::OwnerId self() const override { return index_ + 1; }
  std::uint32_t owner_count() const override { return 1 + sim_.agents_[index_]->extra_count; }
  lob::OwnerId owner_id(std::uint32_t k) const override {
    return k == 0 ? index_ + 1 : sim_.agents_[index_]->extra_first + (k - 1);
  }
  bool owns(lob::OwnerId owner) const override {
    const Slot& s = *sim_.agents_[index_];
    return owner == index_ + 1 || (owner >= s.extra_first && owner - s.extra_first < s.extra_count);
  }

 private:
  Simulator& sim_;
  AgentIndex index_;
};

Simulator::Simulator(const SimConfig& cfg, Options options)
    : cfg_(cfg),
      exchange_(cfg.max_price, options.order_log),
      fundamental_(static_cast<double>(cfg.initial_price), cfg.fundamental.sigma_ticks_per_sqrt_s,
                   cfg.fundamental.dt_s,
                   make_stream(cfg.seed, StreamRole::System, 0, StreamPurpose::Decisions)) {
  finalize(cfg_);

  // Agent order is part of the tie-break rule: noise traders first.
  NoiseTraderParams np;
  np.lambda = cfg_.noise_traders.lambda;
  np.p_limit = cfg_.noise_traders.p_limit;
  np.order_lifetime_mean_s = cfg_.noise_traders.order_lifetime_mean_s;
  np.D = cfg_.noise_traders.D;
  np.max_size = cfg_.noise_traders.max_size;
  np.initial_price = cfg_.initial_price;
  np.max_price = cfg_.max_price;
  np.reference_price = cfg_.noise_traders.reference_price;
  for (std::uint32_t i = 0; i < cfg_.noise_traders.count; ++i) {
    add_agent(std::make_unique<NoiseTrader>(
                  np, make_stream(cfg_.seed, StreamRole::NoiseTrader, i, StreamPurpose::Lifetimes)),
              cfg_.noise_traders.latency, StreamRole::NoiseTrader, i);
  }
  // The informed trader, then the market maker, from the optional `informed` and `mm`
  // sections.
  if (cfg_.informed) {
    const InformedConfig& ic = *cfg_.informed;
    InformedTraderParams ip;
    ip.lambda = ic.lambda;
    ip.theta_ticks = ic.theta_ticks;
    ip.size = ic.size;
    add_agent(std::make_unique<InformedTrader>(ip, fundamental_), ic.latency, StreamRole::InformedTrader, 0);
  }
  if (cfg_.mm) {
    const MarketMakerConfig& mc = *cfg_.mm;
    MmParams mp;
    mp.gamma = mc.gamma;
    mp.kappa = mc.kappa;
    mp.sigma = mc.sigma_ticks_per_sqrt_s;
    mp.quote_size = mc.quote_size;
    mp.q_max = mc.q_max;
    mp.max_price = cfg_.max_price;
    market_maker_ = add_agent(std::make_unique<AvellanedaStoikovMM>(mp, cfg_.requote_interval_ns, cfg_.duration_ns),
                              mc.latency, StreamRole::MarketMaker, 0);
  }
  // The calibrated background flow comes last, so no other agent's index (owner id) changes.
  if (cfg_.background) {
    const BackgroundConfig& bc = *cfg_.background;
    CalibratedFlowParams cp;
    cp.calibration = bc.calibration;
    cp.start_price = bc.start_price ? *bc.start_price : bc.calibration->median_mid_ticks;
    cp.max_price = cfg_.max_price;
    background_ = add_agent(std::make_unique<CalibratedFlowAgent>(
                                cp, make_stream(cfg_.seed, StreamRole::CalibratedFlow, 0, StreamPurpose::Lifetimes)),
                            bc.latency, StreamRole::CalibratedFlow, 0);
    set_owner_count(*background_, bc.owner_count);
  }
}

Simulator::~Simulator() = default;

AgentIndex Simulator::add_agent(std::unique_ptr<Agent> agent, const LatencySpec& latency, StreamRole role,
                                std::uint32_t index) {
  if (ran_) throw std::logic_error("Simulator: add_agent after run()");
  const auto at = static_cast<AgentIndex>(agents_.size());
  auto slot = std::make_unique<Slot>(Slot{std::move(agent), Channel(latency.entry, latency.jitter),
                                          Channel(latency.response, latency.jitter),
                                          Channel(latency.feed, latency.jitter),
                                          make_stream(cfg_.seed, role, index, StreamPurpose::Decisions),
                                          make_stream(cfg_.seed, role, index, StreamPurpose::Latency), nullptr,
                                          0, 0});
  slot->ctx = std::make_unique<AgentContext>(*this, at);
  agents_.push_back(std::move(slot));
  return at;
}

void Simulator::set_owner_count(AgentIndex agent, std::uint32_t count) {
  if (ran_) throw std::logic_error("Simulator: set_owner_count after run()");
  if (agent >= agents_.size()) throw std::logic_error("Simulator: set_owner_count for an unknown agent");
  if (count < 1) throw std::logic_error("Simulator: an agent needs at least one owner id");
  Slot& slot = *agents_[agent];
  if (slot.extra_count != 0) throw std::logic_error("Simulator: set_owner_count called twice for one agent");
  if (count - 1 > ~lob::OwnerId{0} - kExtraOwnerBase - extra_owner_agent_.size())
    throw std::logic_error("Simulator: too many owner ids");
  slot.extra_first = kExtraOwnerBase + static_cast<lob::OwnerId>(extra_owner_agent_.size());
  slot.extra_count = count - 1;
  extra_owner_agent_.insert(extra_owner_agent_.end(), count - 1, agent);
}

void Simulator::add_observer(Observer* observer) { observers_.push_back(observer); }

Agent& Simulator::agent(AgentIndex i) { return *agents_.at(i)->agent; }
const Agent& Simulator::agent(AgentIndex i) const { return *agents_.at(i)->agent; }

AgentIndex Simulator::agent_of_owner(lob::OwnerId owner) const {
  if (owner >= kExtraOwnerBase) {
    if (owner - kExtraOwnerBase >= extra_owner_agent_.size()) throw std::logic_error("Simulator: event for an unknown owner");
    return extra_owner_agent_[owner - kExtraOwnerBase];
  }
  if (owner == 0 || owner > agents_.size()) throw std::logic_error("Simulator: event for an unknown owner");
  return owner - 1;
}

void Simulator::deliver_private(lob::OwnerId owner, const lob::Event& event) {
  const AgentIndex to = agent_of_owner(owner);
  Slot& slot = *agents_[to];
  queue_.schedule(slot.response.arrival(now_, slot.latency_rng), EventKind::PrivateDelivery, to, event);
}

void Simulator::handle_request(const SimEvent& ev) {
  // The owner on the request is kept if it is one of the sender's own ids, else replaced by
  // the sender's (an agent cannot act for another owner).
  lob::OwnerId sender = ev.agent + 1;
  const lob::Request& sent = std::get<lob::Request>(ev.payload);
  {
    const Slot& slot = *agents_[ev.agent];
    const lob::OwnerId asked = std::visit([](const auto& r) { return r.owner; }, sent);
    if (slot.extra_count != 0 && asked >= slot.extra_first && asked - slot.extra_first < slot.extra_count)
      sender = asked;
  }
  const lob::Request request = with_owner(sent, sender);
  for (Observer* o : observers_) o->on_request(now_, request);

  const ExchangeOutput& out = exchange_.process(now_, sender, request);

  for (const lob::Event& event : out.events) {
    for (Observer* o : observers_) o->on_engine_event(now_, event);
    if (const auto* t = std::get_if<lob::Trade>(&event)) {
      deliver_private(t->maker_owner, event);
      // Once per agent: one agent may be both sides of a trade under two of its owner ids.
      if (agent_of_owner(t->taker_owner) != agent_of_owner(t->maker_owner)) deliver_private(t->taker_owner, event);
    } else {
      const lob::OwnerId owner = std::visit(
          [](const auto& e) -> lob::OwnerId {
            if constexpr (requires { e.owner; }) return e.owner;
            else return 0;
          },
          event);
      deliver_private(owner, event);
    }
  }
  // Public data: message-major, receivers in agent order.
  for (const MarketData& md : out.public_data) {
    for (AgentIndex i = 0; i < agents_.size(); ++i) {
      Slot& slot = *agents_[i];
      queue_.schedule(slot.feed.arrival(now_, slot.latency_rng), EventKind::MarketDataDelivery, i, md);
    }
  }
  for (Observer* o : observers_) o->on_book(now_, out.top, out.top_changed);
}

void Simulator::run() {
  if (ran_) throw std::logic_error("Simulator: run() called twice");
  ran_ = true;
  const TimeNs end = cfg_.duration_ns;

  now_ = 0;
  for (auto& slot : agents_) slot->agent->on_start(*slot->ctx);
  queue_.schedule(cfg_.fundamental_dt_ns, EventKind::FundamentalStep, 0);
  queue_.schedule(cfg_.sample_interval_ns, EventKind::Sample, 0);

  while (!queue_.empty() && queue_.top().time < end) {
    const SimEvent ev = queue_.pop();
    now_ = ev.time;
    ++processed_;
    switch (ev.kind) {
      case EventKind::Wakeup: {
        Slot& slot = *agents_[ev.agent];
        slot.agent->on_wakeup(*slot.ctx);
        break;
      }
      case EventKind::RequestArrival:
        handle_request(ev);
        break;
      case EventKind::PrivateDelivery: {
        Slot& slot = *agents_[ev.agent];
        const lob::Event& event = std::get<lob::Event>(ev.payload);
        for (Observer* o : observers_) o->on_private_delivered(now_, ev.agent, event);
        slot.agent->on_private(*slot.ctx, event);
        break;
      }
      case EventKind::MarketDataDelivery: {
        Slot& slot = *agents_[ev.agent];
        slot.agent->on_market_data(*slot.ctx, std::get<MarketData>(ev.payload));
        break;
      }
      case EventKind::FundamentalStep:
        fundamental_.step();
        for (Observer* o : observers_) o->on_fundamental(now_, fundamental_.value());
        ++fundamental_k_;
        queue_.schedule(static_cast<TimeNs>(fundamental_k_) * cfg_.fundamental_dt_ns, EventKind::FundamentalStep, 0);
        break;
      case EventKind::Sample:
        for (Observer* o : observers_) o->on_sample(now_);
        ++sample_k_;
        queue_.schedule(static_cast<TimeNs>(sample_k_) * cfg_.sample_interval_ns, EventKind::Sample, 0);
        break;
    }
  }
  now_ = end;
  for (Observer* o : observers_) o->on_end(end);
}

}  // namespace sim
