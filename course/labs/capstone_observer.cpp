// Capstone A starter: measure something new about the market with your own Observer.
//
//   course/labs/build_lab.sh course/labs/capstone_observer.cpp && build/release/labs/capstone_observer
//
// It runs the same ten simulated minutes twice, without and with the observer, and checks that the
// engine event hash is the same: an observer must never change the run (include/sim/observer.hpp).
// The example observer measures how long the best bid and ask stay unchanged. Replace or extend it
// with your own measurement (ideas in course/16-capstone.md).
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <map>

#include "sim/config.hpp"
#include "sim/observer.hpp"
#include "sim/simulator.hpp"

namespace {

class QuoteLifetimeObserver final : public sim::Observer {
 public:
  // Called after every request with the TRUE top of book (zero latency) and whether it changed.
  void on_book(sim::TimeNs now, const sim::BBO& top, bool changed) override {
    if (!changed) return;
    if (started_) {
      const sim::TimeNs lived = now - since_;
      ++changes_;
      total_ns_ += lived;
      longest_ns_ = std::max(longest_ns_, lived);
      ++by_spread_[last_.two_sided() ? last_.ask - last_.bid : 0];
    }
    started_ = true;
    since_ = now;
    last_ = top;
  }

  void report() const {
    std::printf("top of book changed %" PRIu64 " times\n", changes_);
    if (changes_ == 0) return;
    std::printf("average time between changes %.3f ms, longest %.3f s\n",
                static_cast<double>(total_ns_) / static_cast<double>(changes_) / 1e6, static_cast<double>(longest_ns_) / 1e9);
    std::printf("spread in ticks just before each change (0 = one-sided) -> number of changes:\n ");
    for (const auto& [spread, n] : by_spread_) std::printf("  %lld -> %" PRIu64, static_cast<long long>(spread), n);
    std::printf("\n");
  }

 private:
  bool started_{false};
  sim::TimeNs since_{0};
  sim::BBO last_{};
  std::uint64_t changes_{0};
  sim::TimeNs total_ns_{0}, longest_ns_{0};
  std::map<lob::Price, std::uint64_t> by_spread_;
};

}  // namespace

int main() {
  sim::SimConfig cfg = sim::load_config("configs/default.json");  // run from the repository root
  cfg.duration_s = 600;
  sim::finalize(cfg);

  sim::Simulator bare(cfg);
  bare.run();

  sim::Simulator watched(cfg);
  QuoteLifetimeObserver observer;
  watched.add_observer(&observer);
  watched.run();

  std::printf("event hash without the observer %016" PRIx64 ", with it %016" PRIx64 ": %s\n",
              bare.exchange().event_hash(), watched.exchange().event_hash(),
              bare.exchange().event_hash() == watched.exchange().event_hash() ? "identical, good" : "DIFFERENT: a bug");
  observer.report();
}
