#pragma once

// Helpers shared by the simulator tests. Nothing here touches the file system.
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lob/events.hpp"
#include "lob/types.hpp"
#include "sim/agent.hpp"
#include "sim/config.hpp"
#include "sim/metrics.hpp"
#include "sim/simulator.hpp"

namespace simtest {

// The JSON of SPEC 5.8, as in configs/default.json.
inline nlohmann::json default_json() {
  return nlohmann::json::parse(R"({
  "seed": 42, "duration_s": 3600, "warmup_s": 60, "max_price": 1000000, "initial_price": 10000,
  "tick_display": 0.01, "fundamental": {"sigma_ticks_per_sqrt_s": 1.0, "dt_s": 0.1},
  "noise_traders": {"count": 10, "lambda": 5.0, "p_limit": 0.85, "order_lifetime_mean_s": 30.0,
                    "D": 10, "max_size": 10,
                    "latency_us": {"entry": 200, "response": 200, "feed": 200, "jitter": 50}},
  "informed": {"lambda": 2.0, "theta_ticks": 2, "size": 5,
               "latency_us": {"entry": 100, "response": 100, "feed": 100, "jitter": 20}},
  "mm": {"gamma": 0.0001, "kappa": 1.5, "sigma_ticks_per_sqrt_s": 1.0, "quote_size": 5, "q_max": 50,
         "requote_interval_ms": 100, "maker_fee_ticks": -0.002, "taker_fee_ticks": 0.003,
         "latency_us": {"entry": 50, "response": 50, "feed": 50, "jitter": 10}},
  "markout_horizons_s": [1, 5, 30], "sample_interval_s": 1.0
})");
}

inline sim::SimConfig default_config() { return sim::parse_config(default_json()); }

// A config with `count` noise traders, the given seed and duration, and no other agents.
inline sim::SimConfig noise_config(std::uint32_t count, std::uint64_t seed, double duration_s) {
  sim::SimConfig c = default_config();
  c.noise_traders.count = count;
  c.seed = seed;
  c.duration_s = duration_s;
  c.warmup_s = 0;
  c.informed.reset();
  c.mm.reset();
  sim::finalize(c);
  return c;
}

// An empty market: no configured agents; tests add their own.
inline sim::SimConfig empty_config(double duration_s) { return noise_config(0, 1, duration_s); }

struct RunResult {
  std::string log;
  std::uint64_t hash{};
  std::uint64_t requests{};
  std::uint64_t trades{};
  std::size_t resting{};
};

inline RunResult run(const sim::SimConfig& cfg) {
  std::ostringstream log;
  sim::Simulator s(cfg, sim::SimulatorOptions{&log});
  s.run();
  return {log.str(), s.exchange().event_hash(), s.exchange().requests(), s.exchange().trades(),
          s.exchange().book().resting_orders()};
}

// The five output files of lobsim_run, as text, from an in-process run with the metrics
// observer attached (the same observer and writers the tool uses).
struct RunFiles {
  std::string orders_log, trades_csv, bbo_csv, mm_fills_csv, summary_json;
  std::uint64_t hash{};
};

inline RunFiles run_with_metrics(const sim::SimConfig& cfg) {
  std::ostringstream log;
  sim::Simulator s(cfg, sim::SimulatorOptions{&log});
  sim::MetricsObserver metrics(sim::make_metrics_options(s.config()));
  s.add_observer(&metrics);
  s.run();
  RunFiles f;
  f.orders_log = log.str();
  f.hash = s.exchange().event_hash();
  std::ostringstream trades, bbo, fills;
  metrics.write_trades_csv(trades);
  metrics.write_bbo_csv(bbo);
  metrics.write_mm_fills_csv(fills);
  f.trades_csv = trades.str();
  f.bbo_csv = bbo.str();
  f.mm_fills_csv = fills.str();
  f.summary_json = metrics.summary_json(s.config(), f.hash);
  return f;
}

// The default config shortened to 30 s with a 5 s warm-up (all three kinds of agent).
inline sim::SimConfig default_config_for_files() {
  sim::SimConfig c = default_config();
  c.duration_s = 30;
  c.warmup_s = 5;
  sim::finalize(c);
  return c;
}

// A scripted agent: sends `script[k]` at time `script[k].first` (times ascending) and
// records everything it receives.
class ScriptAgent final : public sim::Agent {
 public:
  explicit ScriptAgent(std::vector<std::pair<sim::TimeNs, lob::Request>> script = {}) : script_(std::move(script)) {}

  void on_start(sim::Context& ctx) override {
    for (const auto& s : script_) ctx.schedule_wakeup(s.first);
  }
  void on_wakeup(sim::Context& ctx) override {
    if (next_ < script_.size()) {
      sent_at.push_back(ctx.now());
      ctx.send(script_[next_++].second);
    }
  }
  void on_market_data(sim::Context&, const sim::MarketData& m) override { market.push_back(m); }
  void on_private(sim::Context&, const lob::Event& e) override { events.push_back(e); }

  std::vector<sim::MarketData> market;
  std::vector<lob::Event> events;
  std::vector<sim::TimeNs> sent_at;

 private:
  std::vector<std::pair<sim::TimeNs, lob::Request>> script_;
  std::size_t next_{0};
};

inline lob::NewOrder limit(lob::Side side, lob::Price price, lob::Qty qty, std::uint64_t cid = 1) {
  return lob::NewOrder{0, cid, side, lob::OrderType::Limit, lob::TIF::GTC, price, qty, false, lob::STP::CancelTaking};
}

inline lob::NewOrder market(lob::Side side, lob::Qty qty, std::uint64_t cid = 1) {
  return lob::NewOrder{0, cid, side, lob::OrderType::Market, lob::TIF::IOC, 0, qty, false, lob::STP::CancelTaking};
}

}  // namespace simtest
