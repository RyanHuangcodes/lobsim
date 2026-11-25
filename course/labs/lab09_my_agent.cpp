// Lab 9: write your own trader and drop it into the simulated market.
//
//   course/labs/build_lab.sh course/labs/lab09_my_agent.cpp && build/release/labs/lab09_my_agent
//
// The trader below ("Copycat") follows the crowd: once a second it compares the last trade price
// it has heard about with the one it heard a second ago. Price went up: it buys 1 lot at market.
// Price went down: it sells 1 lot. It never holds more than 20 lots either way.
//
// It is an ordinary sim::Agent, like NoiseTrader or AvellanedaStoikovMM: it sees the market only
// through delayed messages and trades only by sending requests to the exchange. Change its rule,
// rebuild, run again. Ideas are at the end of course/09-traders.md.
#include <cstdio>
#include <memory>
#include <optional>
#include <variant>

#include "sim/agent.hpp"
#include "sim/config.hpp"
#include "sim/simulator.hpp"

namespace {

constexpr sim::TimeNs kSecond = 1'000'000'000;

class Copycat final : public sim::Agent {
 public:
  void on_start(sim::Context& ctx) override { ctx.schedule_wakeup(ctx.now() + kSecond); }

  void on_wakeup(sim::Context& ctx) override {
    ctx.schedule_wakeup(ctx.now() + kSecond);  // keep the alarm clock going
    if (last_trade_ && price_a_second_ago_) {
      if (*last_trade_ > *price_a_second_ago_ && inventory_ < 20) send_market(ctx, lob::Side::Buy);
      else if (*last_trade_ < *price_a_second_ago_ && inventory_ > -20) send_market(ctx, lob::Side::Sell);
    }
    price_a_second_ago_ = last_trade_;
  }

  void on_market_data(sim::Context&, const sim::MarketData& msg) override {
    if (const auto* t = std::get_if<sim::TradePrint>(&msg)) last_trade_ = t->price;
    if (const auto* b = std::get_if<sim::BBO>(&msg)) bbo_ = *b;
  }

  void on_private(sim::Context& ctx, const lob::Event& event) override {
    // Our market orders are always the taker, so our side is the aggressor side.
    if (const auto* t = std::get_if<lob::Trade>(&event); t != nullptr && t->taker_owner == ctx.self()) {
      const bool bought = t->aggressor_side == lob::Side::Buy;
      inventory_ += bought ? t->qty : -t->qty;
      cash_ += bought ? -t->price * t->qty : t->price * t->qty;
      ++fills_;
    }
  }

  long long orders() const { return orders_; }
  long long fills() const { return fills_; }
  long long inventory() const { return inventory_; }
  long long cash() const { return cash_; }

 private:
  void send_market(sim::Context& ctx, lob::Side side) {
    ctx.send(lob::NewOrder{ctx.self(), next_client_id_++, side, lob::OrderType::Market, lob::TIF::IOC, 0, 1, false,
                           lob::STP::CancelTaking});
    ++orders_;
  }

  std::optional<lob::Price> last_trade_, price_a_second_ago_;
  sim::BBO bbo_{};
  std::uint64_t next_client_id_{1};
  long long orders_{0}, fills_{0}, inventory_{0}, cash_{0};
};

}  // namespace

int main() {
  sim::SimConfig cfg = sim::load_config("configs/default.json");  // run from the repository root
  cfg.duration_s = 600;  // ten simulated minutes
  sim::finalize(cfg);

  sim::Simulator simulator(cfg);
  auto agent = std::make_unique<Copycat>();
  Copycat* copycat = agent.get();
  // 100 us each way, up to 20 us of jitter. StreamRole::Test keeps every other agent's random
  // numbers exactly as they were (course/07-randomness.md explains why that matters).
  const sim::AgentIndex index = simulator.add_agent(std::move(agent), sim::LatencySpec{100'000, 100'000, 100'000, 20'000},
                                                    sim::StreamRole::Test, 0);
  simulator.run();

  const double mid = simulator.exchange().last_mid().value_or(0.0);
  std::printf("Copycat is agent %u (owner id %u)\n", index, index + 1);
  std::printf("orders sent %lld, fills %lld, final inventory %lld lots\n", copycat->orders(), copycat->fills(),
              copycat->inventory());
  std::printf("cash %lld ticks, last mid %.1f\n", copycat->cash(), mid);
  std::printf("PnL = cash + inventory * mid = %.1f ticks\n", static_cast<double>(copycat->cash()) + copycat->inventory() * mid);
}
