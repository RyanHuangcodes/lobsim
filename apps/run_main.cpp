// lobsim_run: run one simulation and write its outputs.
//
//   lobsim_run --config <file> [--seed N] [--duration-s X] --out <dir>
//
// Command-line flags override the config file. Writes orders.log, trades.csv, bbo.csv,
// mm_fills.csv and summary.json into --out. The wall clock is read here only, for the
// "elapsed" line on stdout: nothing derived from it goes into any output file.
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <ostream>
#include <string>

#include "lob/events.hpp"
#include "sim/agents/as_market_maker.hpp"
#include "sim/agents/calibrated_flow.hpp"
#include "sim/config.hpp"
#include "sim/metrics.hpp"
#include "sim/simulator.hpp"

namespace {

int usage() {
  std::fprintf(stderr, "usage: lobsim_run --config <file> [--seed N] [--duration-s X] --out <dir>\n");
  return 2;
}

std::string opt(const std::optional<double>& v, const char* format = "%.4f") {
  if (!v) return "n/a";
  char buf[64];
  std::snprintf(buf, sizeof buf, format, *v);
  return buf;
}

std::string opt(const std::optional<std::int64_t>& v) { return v ? std::to_string(*v) : "n/a"; }

void print_metrics(const sim::SimConfig& cfg, const sim::MetricsSummary& m) {
  const sim::RunCounts& c = m.counts;
  std::printf("window               warm-up %.1f s to %.1f s\n", cfg.warmup_s, cfg.duration_s);
  std::printf("requests by type     %" PRIu64 " new, %" PRIu64 " cancel, %" PRIu64 " modify\n", c.requests_new,
              c.requests_cancel, c.requests_modify);
  std::printf("engine events        %" PRIu64 " ack, %" PRIu64 " reject, %" PRIu64 " trade, %" PRIu64
              " cancelled, %" PRIu64 " modified\n",
              c.ev_ack, c.ev_reject, c.ev_trade, c.ev_cancelled, c.ev_modified);
  std::printf("traded volume        %lld lots\n", static_cast<long long>(c.traded_volume));
  std::string rejects;
  for (std::size_t r = 0; r < c.rejects.size(); ++r) {
    if (c.rejects[r] == 0) continue;
    rejects += (rejects.empty() ? "" : ", ") + std::string(lob::to_string(static_cast<lob::RejectReason>(r))) + " " +
               std::to_string(c.rejects[r]);
  }
  std::printf("rejects              %s\n", rejects.empty() ? "none" : rejects.c_str());
  static constexpr const char* names[sim::kRoleCount] = {"noise", "informed", "market maker"};
  for (std::size_t r = 0; r < sim::kRoleCount; ++r) {
    if (r == 1 && !cfg.informed) continue;
    if (r == 2 && !cfg.mm) continue;
    std::printf("role %-12s    %" PRIu64 " orders sent, taker volume %lld, maker volume %lld lots\n", names[r],
                c.roles[r].orders_sent, static_cast<long long>(c.roles[r].taker_volume),
                static_cast<long long>(c.roles[r].maker_volume));
  }
  if (m.has_mm) {
    const auto& w = m.mm_window;
    const auto& t = m.mm_total;
    std::printf("mm window fills      %" PRIu64 " (%" PRIu64 " buy, %" PRIu64 " sell), volume %lld buy / %lld sell lots\n",
                w.fills, w.buy_fills, w.sell_fills, static_cast<long long>(w.volume_buy),
                static_cast<long long>(w.volume_sell));
    std::printf("mm pnl after warm-up %.4f ticks, max |inventory| %lld lots\n", w.pnl_after_warmup,
                static_cast<long long>(w.max_abs_inventory));
    std::printf("mm quoting both     %s of the window, average quoted spread %s ticks\n",
                opt(w.quoting_fraction, "%.4f").c_str(), opt(w.avg_quoted_spread).c_str());
    std::printf("mm whole run         %" PRIu64 " fills, volume %lld buy / %lld sell lots, final inventory %lld, max |inventory| %lld\n",
                t.fills, static_cast<long long>(t.volume_buy), static_cast<long long>(t.volume_sell),
                static_cast<long long>(t.final_inventory), static_cast<long long>(t.max_abs_inventory));
    std::printf("mm pnl total         %.4f ticks = cash %lld + inventory value %.4f - fees %.4f\n", t.pnl_total,
                static_cast<long long>(t.cash), t.inventory_value, t.fees);
    for (const sim::MarkoutStats& k : m.markouts)
      std::printf("markout %gs  n %" PRIu64 " (skipped %" PRIu64 "), mean %s, sd %s, 95%% CI [%s, %s], qty-weighted %s ticks/lot\n",
                  k.horizon_s, k.n, k.skipped, opt(k.mean).c_str(), opt(k.sd).c_str(), opt(k.ci_lo).c_str(),
                  opt(k.ci_hi).c_str(), opt(k.mean_qty_weighted).c_str());
    for (const sim::MarkoutStats& k : m.markouts) {
      const sim::Decomposition& d = k.decomposition;
      std::printf("  %gs = spread capture + mid move: n %" PRIu64 ", markout %s = %s + %s (mid move 95%% CI [%s, %s])\n",
                  k.horizon_s, d.n, opt(d.markout_mean).c_str(), opt(d.spread_capture_mean).c_str(),
                  opt(d.mid_move_mean).c_str(), opt(d.mid_move_ci_lo).c_str(), opt(d.mid_move_ci_hi).c_str());
    }
  }
  const auto& sp = m.spread;
  std::printf("spread (two-sided)   %s of the window; mean %s, median %s, p90 %s, p99 %s ticks\n",
              opt(sp.two_sided_fraction).c_str(), opt(sp.mean).c_str(), opt(sp.median).c_str(), opt(sp.p90).c_str(),
              opt(sp.p99).c_str());
  const auto& r = m.returns;
  std::printf("returns              %" PRIu64 " samples (%" PRIu64 " missing), %" PRIu64 " returns, sd %s, zero fraction %s, excess kurtosis %s\n",
              r.samples, r.missing, r.returns, opt(r.sd, "%.3e").c_str(), opt(r.zero_fraction).c_str(),
              opt(r.excess_kurtosis).c_str());
  std::string acf, abs_acf;
  for (const auto& v : r.acf) acf += (acf.empty() ? "" : " ") + opt(v, "%.3f");
  for (const auto& v : r.abs_acf) abs_acf += (abs_acf.empty() ? "" : " ") + opt(v, "%.3f");
  std::printf("return acf lag 1-5   %s\n", acf.c_str());
  std::printf("|return| acf lag 1-10 %s\n", abs_acf.c_str());
}

bool write_file(const std::filesystem::path& path, const std::function<void(std::ostream&)>& body) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (f) {
    body(f);
    f.flush();
  }
  if (!f) {
    std::fprintf(stderr, "lobsim_run: cannot write %s\n", path.string().c_str());
    return false;
  }
  return true;
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
    sim::MetricsObserver metrics(sim::make_metrics_options(simulator.config()));
    simulator.add_observer(&metrics);
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
    if (const auto mi = simulator.market_maker_index()) {
      // What only the agent knows: its own counters and its belief about its inventory.
      const auto& mm = dynamic_cast<const sim::AvellanedaStoikovMM&>(simulator.agent(*mi));
      std::printf("mm final inventory   %lld lots (the agent believes %lld)\n",
                  static_cast<long long>(metrics.mm_inventory()), static_cast<long long>(mm.inventory()));
      std::printf("mm requests          %" PRIu64 " new, %" PRIu64 " modify, %" PRIu64 " cancel\n", mm.news_sent(),
                  mm.modifies_sent(), mm.cancels_sent());
      std::string rejects;
      for (int r = 0; r <= static_cast<int>(lob::RejectReason::PostOnlyWouldCross); ++r) {
        const auto reason = static_cast<lob::RejectReason>(r);
        if (mm.rejects(reason) == 0) continue;
        rejects += (rejects.empty() ? "" : ", ") + std::string(lob::to_string(reason)) + " " +
                   std::to_string(mm.rejects(reason));
      }
      std::printf("mm rejects           %s\n", rejects.empty() ? "none" : rejects.c_str());
      std::printf("mm anomalies         %" PRIu64 "\n", mm.anomalies());
    }
    if (const auto bi = simulator.background_index()) {
      const auto& bg = dynamic_cast<const sim::CalibratedFlowAgent&>(simulator.agent(*bi));
      const sim::CalibratedFlowCounters& k = bg.counters();
      std::printf("background           calibrated flow (%s, %s), %u owner ids\n", cfg.background->calibration->source.c_str(),
                  cfg.background->file.c_str(), static_cast<unsigned>(cfg.background->owner_count));
      std::printf("background orders    %" PRIu64 " limit, %" PRIu64 " market (rates %.4f and %.4f per s configured)\n",
                  k.limits_sent, k.markets_sent, bg.limit_rate(), bg.aggressive_rate());
      std::printf("background placement %" PRIu64 " redraws, %" PRIu64 " clamped to the crossing bound, %" PRIu64
                  " price clamps; fallbacks: %" PRIu64 " one-sided book, %" PRIu64 " empty book\n",
                  k.placement_redraws, k.placement_clamped, k.price_clamped, k.fallback_one_sided, k.fallback_empty);
      std::printf("background buckets   %" PRIu64 " placements drawn from the nearest non-empty spread bucket\n",
                  k.placement_bucket_fallback);
      std::printf("background cancels   %" PRIu64 " scheduled, %" PRIu64 " no-cancel draws, %" PRIu64 " sent, %" PRIu64
                  " rejects ignored\n",
                  k.cancels_scheduled, k.no_cancel_draws, k.cancels_sent, k.ignored_rejects);
    }
    print_metrics(cfg, metrics.summary());

    const std::filesystem::path dir(out_dir);
    const std::string summary = metrics.summary_json(cfg, ex.event_hash());
    if (!write_file(dir / "trades.csv", [&](std::ostream& o) { metrics.write_trades_csv(o); }) ||
        !write_file(dir / "bbo.csv", [&](std::ostream& o) { metrics.write_bbo_csv(o); }) ||
        !write_file(dir / "mm_fills.csv", [&](std::ostream& o) { metrics.write_mm_fills_csv(o); }) ||
        !write_file(dir / "summary.json", [&](std::ostream& o) { o << summary; }))
      return 1;
    std::printf("simulator events     %" PRIu64 "\n", simulator.events_processed());
    std::printf("elapsed (wall clock) %.2f s, %.2f million simulator events per second\n", elapsed,
                elapsed > 0 ? static_cast<double>(simulator.events_processed()) / elapsed / 1e6 : 0.0);
    std::printf("wrote                %s and trades.csv, bbo.csv, mm_fills.csv, summary.json\n", log_path.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_run: %s\n", e.what());
    return 1;
  }
  return 0;
}
