#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <vector>

#include "sim/agent.hpp"
#include "sim/config.hpp"
#include "sim/event_queue.hpp"
#include "sim/exchange.hpp"
#include "sim/fundamental.hpp"
#include "sim/latency.hpp"
#include "sim/messages.hpp"
#include "sim/observer.hpp"
#include "sim/rng.hpp"

namespace sim {

// The discrete-event kernel (SPEC 5.1 to 5.5). Single-threaded and deterministic: the
// same config and seed give the same events in the same order.
//
// Order of processing, in full:
//   1. Every event carries (time, seq); seq is a global counter incremented on every
//      schedule. Events are processed in increasing (time, seq): events with equal time
//      are processed in the order they were scheduled. There is no other tie-break rule.
//   2. At time 0 each agent's on_start() runs, in agent order (noise traders, then the
//      informed trader, then the market maker, then agents added with add_agent()). Then
//      the first FundamentalStep and the first Sample are scheduled.
//   3. Periodic events fire at k * period, computed from an integer k.
//   4. A request arriving at the exchange is processed at once. Its private events are
//      then scheduled in engine order (a Trade to the maker's owner first, then the
//      taker's), followed by its public data: one TradePrint per trade, then one BBO if
//      the top of book changed, each fanned out to every agent in agent order.
//   5. Events at exactly the end time are not processed.
struct SimulatorOptions {
  std::ostream* order_log{nullptr};  // receives orders.log; may be null
};

class Simulator {
 public:
  using Options = SimulatorOptions;

  explicit Simulator(const SimConfig& cfg, Options options = {});
  ~Simulator();
  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  // Adds an agent after the configured ones (used by tests). Its random streams are
  // (role, index, Decisions) and (role, index, Latency). Must be called before run().
  AgentIndex add_agent(std::unique_ptr<Agent> agent, const LatencySpec& latency, StreamRole role,
                       std::uint32_t index);
  // Gives an agent `count` owner ids in all (its own, agent index + 1, and count - 1 more taken
  // from kExtraOwnerBase upwards, in call order). Requests it sends may carry any of them
  // (any other owner is replaced by its own, as before), and private events for any of them
  // are delivered to it, once per event. Must be called before run(). Agents that never call
  // it keep exactly their old owner id and event order.
  void set_owner_count(AgentIndex agent, std::uint32_t count);
  // Observers are not owned and must outlive run().
  void add_observer(Observer* observer);

  void run();  // from time 0 to the configured duration; call once

  const SimConfig& config() const noexcept { return cfg_; }
  const Exchange& exchange() const noexcept { return exchange_; }
  const Fundamental& fundamental() const noexcept { return fundamental_; }
  TimeNs now() const noexcept { return now_; }
  TimeNs end() const noexcept { return cfg_.duration_ns; }
  std::size_t agent_count() const noexcept { return agents_.size(); }
  Agent& agent(AgentIndex i);
  const Agent& agent(AgentIndex i) const;
  std::uint64_t events_processed() const noexcept { return processed_; }
  // Index of the market maker created from the config's `mm` section, if there is one.
  std::optional<AgentIndex> market_maker_index() const noexcept { return market_maker_; }
  // Index of the calibrated background flow agent, if the config has a `background` section.
  std::optional<AgentIndex> background_index() const noexcept { return background_; }

 private:
  class AgentContext;
  struct Slot;

  void handle_request(const SimEvent& ev);
  void deliver_private(lob::OwnerId owner, const lob::Event& event);
  AgentIndex agent_of_owner(lob::OwnerId owner) const;

  SimConfig cfg_;
  EventQueue queue_;
  Exchange exchange_;
  Fundamental fundamental_;
  std::vector<std::unique_ptr<Slot>> agents_;
  std::vector<Observer*> observers_;
  TimeNs now_{0};
  std::uint64_t fundamental_k_{1};
  std::uint64_t sample_k_{1};
  std::uint64_t processed_{0};
  std::optional<AgentIndex> market_maker_;
  std::optional<AgentIndex> background_;
  std::vector<AgentIndex> extra_owner_agent_;  // owner kExtraOwnerBase + i belongs to this agent
  bool ran_{false};
};

}  // namespace sim
