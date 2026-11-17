// lobsim_run: run one simulation and write its outputs.
//
//   lobsim_run --config <file> [--seed N] [--duration-s X] --out <dir>
//
// Command-line flags override the config file. The wall clock is read here only, for the
// "elapsed" line on stdout: nothing derived from it goes into any output file.
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include "sim/config.hpp"
#include "sim/simulator.hpp"

namespace {

int usage() {
  std::fprintf(stderr, "usage: lobsim_run --config <file> [--seed N] [--duration-s X] --out <dir>\n");
  return 2;
}

bool parse_u64(const char* s, std::uint64_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || *end != '\0' || s[0] == '-') return false;
  out = v;
  return true;
}

bool parse_double(const char* s, double& out) {
  char* end = nullptr;
  const double v = std::strtod(s, &end);
  if (end == s || *end != '\0') return false;
  out = v;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path, out_dir;
  bool have_seed = false, have_duration = false;
  std::uint64_t seed = 0;
  double duration_s = 0.0;
  for (int i = 1; i < argc; i += 2) {  // every flag takes one value
    const std::string a = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (a == "--config" && value != nullptr) {
      config_path = value;
    } else if (a == "--out" && value != nullptr) {
      out_dir = value;
    } else if (a == "--seed" && value != nullptr && parse_u64(value, seed)) {
      have_seed = true;
    } else if (a == "--duration-s" && value != nullptr && parse_double(value, duration_s)) {
      have_duration = true;
    } else {
      return usage();
    }
  }
  if (config_path.empty() || out_dir.empty()) return usage();

  try {
    sim::SimConfig cfg = sim::load_config(config_path);
    if (have_seed) cfg.seed = seed;
    if (have_duration) {
      cfg.duration_s = duration_s;
      if (cfg.warmup_s > cfg.duration_s) cfg.warmup_s = 0.0;  // a short run has no warm-up to keep
    }
    sim::finalize(cfg);

    std::filesystem::create_directories(out_dir);
    const std::string log_path = (std::filesystem::path(out_dir) / "orders.log").string();
    std::ofstream log(log_path, std::ios::binary | std::ios::trunc);
    if (!log) {
      std::fprintf(stderr, "lobsim_run: cannot write %s\n", log_path.c_str());
      return 1;
    }

    sim::Simulator simulator(cfg, sim::SimulatorOptions{&log});
    const auto t0 = std::chrono::steady_clock::now();
    simulator.run();
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    log.flush();
    if (!log) {
      std::fprintf(stderr, "lobsim_run: error writing %s\n", log_path.c_str());
      return 1;
    }

    const sim::Exchange& ex = simulator.exchange();
    const sim::BBO top = ex.top();
    std::printf("seed                 %" PRIu64 "\n", cfg.seed);
    std::printf("simulated            %.3f s, %zu agents\n", cfg.duration_s, simulator.agent_count());
    std::printf("requests             %" PRIu64 "\n", ex.requests());
    std::printf("engine events        %" PRIu64 "\n", ex.events());
    std::printf("trades               %" PRIu64 "\n", ex.trades());
    std::printf("engine event hash    %016" PRIx64 "\n", ex.event_hash());
    std::printf("resting orders       %zu\n", ex.book().resting_orders());
    if (top.has_bid()) std::printf("best bid             %lld x %lld\n", static_cast<long long>(top.bid), static_cast<long long>(top.bid_qty));
    else std::printf("best bid             none\n");
    if (top.has_ask()) std::printf("best ask             %lld x %lld\n", static_cast<long long>(top.ask), static_cast<long long>(top.ask_qty));
    else std::printf("best ask             none\n");
    std::printf("fundamental value    %.4f after %" PRIu64 " steps\n", simulator.fundamental().value(),
                simulator.fundamental().steps());
    std::printf("simulator events     %" PRIu64 "\n", simulator.events_processed());
    std::printf("elapsed (wall clock) %.2f s, %.2f million simulator events per second\n", elapsed,
                elapsed > 0 ? static_cast<double>(simulator.events_processed()) / elapsed / 1e6 : 0.0);
    std::printf("wrote                %s\n", log_path.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_run: %s\n", e.what());
    return 1;
  }
  return 0;
}
