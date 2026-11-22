#pragma once

#include "lob/events.hpp"
#include "lob/types.hpp"
#include "sim/messages.hpp"
#include "sim/rng.hpp"

namespace sim {

// What an agent can do. Abstract, so a test can drive an agent with a scripted context.
class Context {
 public:
  virtual ~Context() = default;
  virtual TimeNs now() const = 0;
  // Sends a request to the exchange through this agent's entry channel.
  virtual void send(const lob::Request& request) = 0;
  // Asks for one on_wakeup() call at absolute time `at` (>= now()).
  virtual void schedule_wakeup(TimeNs at) = 0;
  // This agent's decision stream. Rule: draw from it only in on_start() and on_wakeup().
  // A draw triggered by an incoming message must use a separate stream owned by the
  // agent, because the order in which a reply and the next wakeup are handled depends on
  // latency jitter, and jitter is consumed at a rate set by every other agent.
  virtual Xoshiro256StarStar& rng() = 0;
  virtual lob::OwnerId self() const = 0;
  // An agent normally has one owner id, self(). A kernel can give it more (a calibrated
  // background flow spreads its orders over many owners so its own orders rarely meet each
  // other under self-trade prevention): owner_id(0) == self(), owner_id(k) for k < owner_count()
  // are all the ids it may put on a request, and owns() tells whether an id of an event is its.
  virtual std::uint32_t owner_count() const { return 1; }
  virtual lob::OwnerId owner_id(std::uint32_t /*k*/) const { return self(); }
  virtual bool owns(lob::OwnerId owner) const { return owner == self(); }
};

// An agent sees the market only through on_market_data() (delayed public data) and
// on_private() (delayed replies about its own orders). It has no access to the exchange
// or its book (SPEC 5.6).
class Agent {
 public:
  virtual ~Agent() = default;
  virtual void on_start(Context& ctx) = 0;
  virtual void on_wakeup(Context& ctx) = 0;
  virtual void on_market_data(Context& ctx, const MarketData& msg) = 0;
  virtual void on_private(Context& ctx, const lob::Event& event) = 0;
};

}  // namespace sim
