#pragma once

#include "lob/events.hpp"
#include "lob/types.hpp"
#include "sim/messages.hpp"

namespace sim {

// Observers see ground truth with zero latency: metrics and test probes are built on
// them. An observer MUST NOT schedule events, send requests or draw random numbers, so
// attaching or removing one can never change the simulation. The kernel schedules its
// Sample events whether or not an observer is attached.
class Observer {
 public:
  virtual ~Observer() = default;
  // An agent called send(); the request is now travelling to the exchange.
  virtual void on_agent_send(TimeNs /*now*/, AgentIndex /*agent*/, const lob::Request& /*request*/) {}
  // The request arrived at the exchange (owner already forced); it is processed next.
  virtual void on_request(TimeNs /*now*/, const lob::Request& /*request*/) {}
  // One engine event produced by that request, in engine order.
  virtual void on_engine_event(TimeNs /*now*/, const lob::Event& /*event*/) {}
  // The true top of book after the request, and whether it differs from before.
  virtual void on_book(TimeNs /*now*/, const BBO& /*top*/, bool /*changed*/) {}
  // A private event is about to be handed to its owner's on_private().
  virtual void on_private_delivered(TimeNs /*now*/, AgentIndex /*agent*/, const lob::Event& /*event*/) {}
  virtual void on_fundamental(TimeNs /*now*/, double /*value*/) {}
  virtual void on_sample(TimeNs /*now*/) {}
  virtual void on_end(TimeNs /*end*/) {}
};

}  // namespace sim
