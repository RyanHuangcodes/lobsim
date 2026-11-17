#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "sim/fundamental.hpp"
#include "sim/simulator.hpp"
#include "support/sim_support.hpp"

using namespace sim;
using simtest::limit;
using simtest::market;
using simtest::ScriptAgent;

namespace {

constexpr LatencySpec kZero{0, 0, 0, 0};
using Script = std::vector<std::pair<TimeNs, lob::Request>>;

// Records (time, agent) of every wakeup into a shared trace; schedules the given times.
struct Trace {
  std::vector<std::pair<TimeNs, int>> hits;
};

class TimerProbe final : public Agent {
 public:
  TimerProbe(int id, std::vector<TimeNs> times, Trace& trace) : id_(id), times_(std::move(times)), trace_(trace) {}
  void on_start(Context& ctx) override {
    for (TimeNs t : times_) ctx.schedule_wakeup(t);
  }
  void on_wakeup(Context& ctx) override { trace_.hits.emplace_back(ctx.now(), id_); }
  void on_market_data(Context&, const MarketData&) override {}
  void on_private(Context&, const lob::Event&) override {}

 private:
  int id_;
  std::vector<TimeNs> times_;
  Trace& trace_;
};

// Schedules `first` in on_start; its first wakeup schedules `later` (so `later` is
// scheduled after anything scheduled during on_start by other agents).
class ChainProbe final : public Agent {
 public:
  ChainProbe(int id, TimeNs first, TimeNs later, Trace& trace) : id_(id), first_(first), later_(later), trace_(trace) {}
  void on_start(Context& ctx) override { ctx.schedule_wakeup(first_); }
  void on_wakeup(Context& ctx) override {
    trace_.hits.emplace_back(ctx.now(), id_);
    if (ctx.now() == first_ && later_ > 0) ctx.schedule_wakeup(later_);
  }
  void on_market_data(Context&, const MarketData&) override {}
  void on_private(Context&, const lob::Event&) override {}

 private:
  int id_;
  TimeNs first_, later_;
  Trace& trace_;
};

// Appends (agent id, is a TradePrint) for every public message it receives; sends its script.
struct MdTrace {
  std::vector<std::pair<int, bool>> items;
};
class MdProbe final : public Agent {
 public:
  MdProbe(int id, MdTrace& trace, Script script = {}) : id_(id), trace_(trace), script_(std::move(script)) {}
  void on_start(Context& ctx) override {
    for (const auto& s : script_) ctx.schedule_wakeup(s.first);
  }
  void on_wakeup(Context& ctx) override { ctx.send(script_[next_++].second); }
  void on_market_data(Context&, const MarketData& m) override {
    trace_.items.emplace_back(id_, std::holds_alternative<TradePrint>(m));
  }
  void on_private(Context&, const lob::Event&) override {}

 private:
  int id_;
  MdTrace& trace_;
  Script script_;
  std::size_t next_{0};
};

class Counter final : public Observer {
 public:
  int fundamental = 0, samples = 0, ends = 0, requests = 0, sends = 0, books = 0, engine_events = 0;
  TimeNs end_time = -1;
  std::vector<TimeNs> sample_times;
  void on_agent_send(TimeNs, AgentIndex, const lob::Request&) override { ++sends; }
  void on_request(TimeNs, const lob::Request&) override { ++requests; }
  void on_engine_event(TimeNs, const lob::Event&) override { ++engine_events; }
  void on_book(TimeNs, const BBO&, bool) override { ++books; }
  void on_fundamental(TimeNs, double) override { ++fundamental; }
  void on_sample(TimeNs t) override {
    ++samples;
    sample_times.push_back(t);
  }
  void on_end(TimeNs t) override {
    ++ends;
    end_time = t;
  }
};

// Per owner: the private events in engine emission order, and as delivered to the agent.
// Also the global order of Trade deliveries.
class PrivateOrderObserver final : public Observer {
 public:
  std::vector<std::vector<lob::Event>> emitted, delivered;
  std::vector<AgentIndex> trade_delivery_order;
  explicit PrivateOrderObserver(std::size_t owners) : emitted(owners + 1), delivered(owners + 1) {}
  void on_engine_event(TimeNs, const lob::Event& e) override {
    std::visit(
        [&](const auto& ev) {
          if constexpr (std::is_same_v<std::decay_t<decltype(ev)>, lob::Trade>) {
            emitted[ev.maker_owner].push_back(e);
            if (ev.taker_owner != ev.maker_owner) emitted[ev.taker_owner].push_back(e);
          } else {
            emitted[ev.owner].push_back(e);
          }
        },
        e);
  }
  void on_private_delivered(TimeNs, AgentIndex agent, const lob::Event& e) override {
    delivered[agent + 1].push_back(e);
    if (std::holds_alternative<lob::Trade>(e)) trade_delivery_order.push_back(agent);
  }
};

}  // namespace

TEST(Kernel, EqualTimeEventsRunInSchedulingOrder) {
  Simulator s(simtest::empty_config(1.0));
  Trace trace;
  // a0 schedules 1000, 2000, 1000; a1 then schedules 1000 and 500. At t = 1000 the order
  // of scheduling is a0, a0, a1.
  s.add_agent(std::make_unique<TimerProbe>(0, std::vector<TimeNs>{1000, 2000, 1000}, trace), kZero,
              StreamRole::Test, 0);
  s.add_agent(std::make_unique<TimerProbe>(1, std::vector<TimeNs>{1000, 500}, trace), kZero, StreamRole::Test, 1);
  s.run();
  const std::vector<std::pair<TimeNs, int>> expect = {{500, 1}, {1000, 0}, {1000, 0}, {1000, 1}, {2000, 0}};
  EXPECT_EQ(trace.hits, expect);
}

TEST(Kernel, ALowerAgentScheduledLaterRunsAfterAHigherAgentScheduledEarlier) {
  Simulator s(simtest::empty_config(1.0));
  Trace trace;
  // a0 wakes at 500 and only then schedules a wakeup for 1000; a1 scheduled its own
  // wakeup for 1000 during on_start. Same time, a1 was scheduled first, so a1 runs first
  // although its index is higher. a2 schedules its 1000 wakeup last of all (at 700), so
  // it runs after both.
  s.add_agent(std::make_unique<ChainProbe>(0, 500, 1000, trace), kZero, StreamRole::Test, 0);
  s.add_agent(std::make_unique<ChainProbe>(1, 1000, 0, trace), kZero, StreamRole::Test, 1);
  s.add_agent(std::make_unique<ChainProbe>(2, 700, 1000, trace), kZero, StreamRole::Test, 2);
  s.run();
  const std::vector<std::pair<TimeNs, int>> expect = {{500, 0}, {700, 2}, {1000, 1}, {1000, 0}, {1000, 2}};
  EXPECT_EQ(trace.hits, expect);
}

TEST(Kernel, PublicDataIsFannedOutMessageMajor) {
  Simulator s(simtest::empty_config(1.0));
  MdTrace trace;
  s.add_agent(std::make_unique<MdProbe>(0, trace), kZero, StreamRole::Test, 0);
  s.add_agent(std::make_unique<MdProbe>(1, trace), kZero, StreamRole::Test, 1);
  s.add_agent(std::make_unique<MdProbe>(2, trace, Script{{100, limit(lob::Side::Sell, 101, 5)}}), kZero,
              StreamRole::Test, 2);
  s.add_agent(std::make_unique<MdProbe>(3, trace, Script{{300, market(lob::Side::Buy, 3)}}), kZero, StreamRole::Test,
              3);
  s.run();
  // The sell gives one BBO for everybody. The market buy gives a TradePrint and then a
  // BBO: all receivers of the first message, then all receivers of the second.
  using E = std::pair<int, bool>;
  const std::vector<E> expect = {{0, false}, {1, false}, {2, false}, {3, false}, {0, true},  {1, true},
                                 {2, true},  {3, true},  {0, false}, {1, false}, {2, false}, {3, false}};
  EXPECT_EQ(trace.items, expect);
}

TEST(Kernel, AnEventAtExactlyTheEndTimeIsNotProcessed) {
  Simulator s(simtest::empty_config(1.0));  // T = 1e9 ns
  Trace trace;
  s.add_agent(std::make_unique<TimerProbe>(0, std::vector<TimeNs>{999'999'999, 1'000'000'000, 1'000'000'001}, trace),
              kZero, StreamRole::Test, 0);
  Counter obs;
  s.add_observer(&obs);
  s.run();
  ASSERT_EQ(trace.hits.size(), 1u);
  EXPECT_EQ(trace.hits[0].first, 999'999'999);
  EXPECT_EQ(obs.end_time, 1'000'000'000);
  EXPECT_EQ(obs.ends, 1);
  EXPECT_EQ(s.now(), 1'000'000'000);
}

TEST(Kernel, PeriodicEventsFireOnIntegerMultiples) {
  Simulator s(simtest::empty_config(10.0));  // dt 0.1 s, sample 1 s
  Counter obs;
  s.add_observer(&obs);
  s.run();
  EXPECT_EQ(obs.fundamental, 99);  // steps at 0.1 .. 9.9; 10.0 is the end
  EXPECT_EQ(s.fundamental().steps(), 99u);
  EXPECT_EQ(obs.samples, 9);
  for (std::size_t i = 0; i < obs.sample_times.size(); ++i)
    EXPECT_EQ(obs.sample_times[i], static_cast<TimeNs>(i + 1) * 1'000'000'000);
}

TEST(Kernel, RunTwiceAndLateAddAgentThrow) {
  Simulator s(simtest::empty_config(0.5));
  s.run();
  EXPECT_THROW(s.run(), std::logic_error);
  EXPECT_THROW(s.add_agent(std::make_unique<ScriptAgent>(), kZero, StreamRole::Test, 5), std::logic_error);
}

TEST(Kernel, TradeGoesToMakerOwnerThenTakerOwner) {
  Simulator s(simtest::empty_config(1.0));
  auto a0 = std::make_unique<ScriptAgent>(Script{{100, limit(lob::Side::Sell, 101, 5)}});
  auto a1 = std::make_unique<ScriptAgent>(Script{{200, market(lob::Side::Buy, 3)}});
  ScriptAgent* p0 = a0.get();
  ScriptAgent* p1 = a1.get();
  s.add_agent(std::move(a0), kZero, StreamRole::Test, 0);
  s.add_agent(std::move(a1), kZero, StreamRole::Test, 1);
  PrivateOrderObserver obs(2);
  s.add_observer(&obs);
  s.run();

  // Owner 1 (maker): Ack, Trade. Owner 2 (taker): Ack, Trade.
  ASSERT_EQ(p0->events.size(), 2u);
  ASSERT_EQ(p1->events.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<lob::Ack>(p0->events[0]));
  const auto& t0 = std::get<lob::Trade>(p0->events[1]);
  EXPECT_EQ(t0, std::get<lob::Trade>(p1->events[1]));
  EXPECT_EQ(t0.maker_owner, 1u);
  EXPECT_EQ(t0.taker_owner, 2u);
  // With equal latency the maker's copy was scheduled first, so it is delivered first.
  EXPECT_EQ(obs.trade_delivery_order, (std::vector<AgentIndex>{0, 1}));
}

TEST(Kernel, PublicDataReachesEveryAgentTradePrintsThenBbo) {
  Simulator s(simtest::empty_config(1.0));
  auto a0 = std::make_unique<ScriptAgent>(Script{{100, limit(lob::Side::Sell, 101, 5)}});
  auto a1 = std::make_unique<ScriptAgent>(Script{{300, market(lob::Side::Buy, 3)}});
  ScriptAgent* p0 = a0.get();
  ScriptAgent* p1 = a1.get();
  s.add_agent(std::move(a0), kZero, StreamRole::Test, 0);
  s.add_agent(std::move(a1), kZero, StreamRole::Test, 1);
  s.run();
  // The first order gives a BBO; the market order gives a TradePrint and then a BBO.
  ASSERT_EQ(p1->market.size(), 3u);
  EXPECT_EQ(p0->market, p1->market);
  EXPECT_TRUE(std::holds_alternative<BBO>(p1->market[0]));
  EXPECT_TRUE(std::holds_alternative<TradePrint>(p1->market[1]));
  EXPECT_TRUE(std::holds_alternative<BBO>(p1->market[2]));
}

TEST(Kernel, LatencyDelaysRequestsAndReplies) {
  class Stamp final : public Agent {
   public:
    std::vector<TimeNs> ack_times, md_times;
    void on_start(Context& ctx) override { ctx.schedule_wakeup(1000); }
    void on_wakeup(Context& ctx) override { ctx.send(limit(lob::Side::Buy, 50, 1)); }
    void on_market_data(Context& ctx, const MarketData&) override { md_times.push_back(ctx.now()); }
    void on_private(Context& ctx, const lob::Event&) override { ack_times.push_back(ctx.now()); }
  };
  Simulator s(simtest::empty_config(1.0));
  auto agent = std::make_unique<Stamp>();
  Stamp* p = agent.get();
  s.add_agent(std::move(agent), LatencySpec{300, 500, 700, 0}, StreamRole::Test, 0);
  s.run();
  ASSERT_EQ(p->ack_times.size(), 1u);
  EXPECT_EQ(p->ack_times[0], 1000 + 300 + 500);
  ASSERT_EQ(p->md_times.size(), 1u);
  EXPECT_EQ(p->md_times[0], 1000 + 300 + 700);
}

TEST(Kernel, PrivateDeliveriesArriveInEngineEmissionOrderDespiteJitter) {
  Simulator s(simtest::empty_config(1.0));
  Script sc0, sc1;
  // Two agents trade against each other every 10 us, with cancels (some of unknown ids),
  // all inside a 400 us jitter window.
  for (int i = 0; i < 150; ++i) {
    const TimeNs t = 10'000 + i * 10'000;
    const lob::Price px = 100 + (i % 5);
    sc0.emplace_back(t, limit(lob::Side::Sell, px + 1, 1 + i % 3, static_cast<std::uint64_t>(i + 1)));
    if (i % 4 == 3)
      sc1.emplace_back(t + 5'000, lob::Cancel{0, static_cast<lob::OrderId>(i)});
    else
      sc1.emplace_back(t + 5'000, limit(lob::Side::Buy, px + 2, 1 + i % 4, static_cast<std::uint64_t>(i + 1)));
  }
  auto a0 = std::make_unique<ScriptAgent>(sc0);
  auto a1 = std::make_unique<ScriptAgent>(sc1);
  ScriptAgent* p0 = a0.get();
  ScriptAgent* p1 = a1.get();
  const LatencySpec lat{50'000, 50'000, 50'000, 400'000};
  s.add_agent(std::move(a0), lat, StreamRole::Test, 0);
  s.add_agent(std::move(a1), lat, StreamRole::Test, 1);
  PrivateOrderObserver obs(2);
  s.add_observer(&obs);
  s.run();
  EXPECT_GT(obs.emitted[1].size(), 100u);
  EXPECT_GT(obs.emitted[2].size(), 100u);
  EXPECT_EQ(obs.delivered[1], obs.emitted[1]);
  EXPECT_EQ(obs.delivered[2], obs.emitted[2]);
  EXPECT_EQ(p0->events, obs.emitted[1]);
  EXPECT_EQ(p1->events, obs.emitted[2]);
  bool trades = false;
  for (const lob::Event& e : p0->events) trades = trades || std::holds_alternative<lob::Trade>(e);
  EXPECT_TRUE(trades);
}

TEST(Kernel, ExtraObserverChangesNeitherHashNorLog) {
  const SimConfig cfg = simtest::noise_config(5, 99, 20.0);
  const simtest::RunResult plain = simtest::run(cfg);

  std::ostringstream log;
  Simulator s(cfg, SimulatorOptions{&log});
  Counter c1, c2;
  s.add_observer(&c1);
  s.add_observer(&c2);
  s.run();
  EXPECT_EQ(s.exchange().event_hash(), plain.hash);
  EXPECT_EQ(log.str(), plain.log);
  EXPECT_EQ(static_cast<std::uint64_t>(c1.requests), plain.requests);
  EXPECT_EQ(c1.requests, c2.requests);
  EXPECT_GE(c1.sends, c1.requests);
}

TEST(Kernel, ObserverSeesGroundTruth) {
  Simulator s(simtest::noise_config(3, 5, 5.0));
  Counter c;
  s.add_observer(&c);
  s.run();
  EXPECT_EQ(static_cast<std::uint64_t>(c.requests), s.exchange().requests());
  EXPECT_EQ(static_cast<std::uint64_t>(c.engine_events), s.exchange().events());
  EXPECT_EQ(c.books, c.requests);
  EXPECT_GT(c.requests, 0);
}

TEST(Fundamental, SameSeedSameWalkDifferentSeedDiffers) {
  Fundamental a(100.0, 1.0, 0.1, make_stream(7, StreamRole::System, 0, StreamPurpose::Decisions));
  Fundamental b(100.0, 1.0, 0.1, make_stream(7, StreamRole::System, 0, StreamPurpose::Decisions));
  Fundamental c(100.0, 1.0, 0.1, make_stream(8, StreamRole::System, 0, StreamPurpose::Decisions));
  for (int i = 0; i < 100; ++i) {
    a.step();
    b.step();
    c.step();
  }
  EXPECT_EQ(a.value(), b.value());
  EXPECT_NE(a.value(), c.value());
  EXPECT_EQ(a.steps(), 100u);
}

TEST(Fundamental, IncrementVarianceIsSigmaSquaredDt) {
  const double sigma = 2.0, dt = 0.25;
  Fundamental f(0.0, sigma, dt, make_stream(3, StreamRole::System, 0, StreamPurpose::Decisions));
  constexpr int n = 200000;
  double prev = f.value(), sum = 0, sq = 0;
  for (int i = 0; i < n; ++i) {
    f.step();
    const double inc = f.value() - prev;
    prev = f.value();
    sum += inc;
    sq += inc * inc;
  }
  const double mean = sum / n;
  EXPECT_NEAR(sq / n - mean * mean, sigma * sigma * dt, 0.02);  // expected 1.0, sd of the estimate about 0.003
  EXPECT_NEAR(mean, 0.0, 0.01);
}

TEST(Fundamental, SimulatorStepsItAtTheConfiguredRate) {
  const SimConfig cfg = simtest::empty_config(100.0);
  Simulator a(cfg), b(cfg);
  a.run();
  b.run();
  EXPECT_EQ(a.fundamental().steps(), 999u);
  EXPECT_EQ(a.fundamental().value(), b.fundamental().value());
}
