#include "sim/simulator.hpp"

#include <stdexcept>
#include <utility>
#include <variant>

#include "sim/agents/noise_trader.hpp"

namespace sim {

// One agent with its three channels and its two kernel-owned random streams.
struct Simulator::Slot {
  std::unique_ptr<Agent> agent;
  Channel entry, response, feed;
  Xoshiro256StarStar decision_rng;
  Xoshiro256StarStar latency_rng;
  std::unique_ptr<AgentContext> ctx;
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
  for (std::uint32_t i = 0; i < cfg_.noise_traders.count; ++i) {
    add_agent(std::make_unique<NoiseTrader>(
                  np, make_stream(cfg_.seed, StreamRole::NoiseTrader, i, StreamPurpose::Lifetimes)),
              cfg_.noise_traders.latency, StreamRole::NoiseTrader, i);
  }
  // The informed trader and the market maker are created here, in that order, from the
  // optional `informed` and `mm` config sections.
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
                                          make_stream(cfg_.seed, role, index, StreamPurpose::Latency), nullptr});
  slot->ctx = std::make_unique<AgentContext>(*this, at);
  agents_.push_back(std::move(slot));
  return at;
}

void Simulator::add_observer(Observer* observer) { observers_.push_back(observer); }

Agent& Simulator::agent(AgentIndex i) { return *agents_.at(i)->agent; }
const Agent& Simulator::agent(AgentIndex i) const { return *agents_.at(i)->agent; }

void Simulator::deliver_private(lob::OwnerId owner, const lob::Event& event) {
  if (owner == 0 || owner > agents_.size()) throw std::logic_error("Simulator: event for an unknown owner");
  const AgentIndex to = owner - 1;
  Slot& slot = *agents_[to];
  queue_.schedule(slot.response.arrival(now_, slot.latency_rng), EventKind::PrivateDelivery, to, event);
}

void Simulator::handle_request(const SimEvent& ev) {
  const lob::OwnerId sender = ev.agent + 1;
  const lob::Request request = with_owner(std::get<lob::Request>(ev.payload), sender);
  for (Observer* o : observers_) o->on_request(now_, request);

  const ExchangeOutput& out = exchange_.process(now_, sender, request);

  for (const lob::Event& event : out.events) {
    for (Observer* o : observers_) o->on_engine_event(now_, event);
    if (const auto* t = std::get_if<lob::Trade>(&event)) {
      deliver_private(t->maker_owner, event);
      if (t->taker_owner != t->maker_owner) deliver_private(t->taker_owner, event);
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
