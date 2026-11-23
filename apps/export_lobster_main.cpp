// lobsim_export_lobster: turn a simulator run into a LOBSTER message file and orderbook file.
//
//   lobsim_export_lobster --run <sim output dir> --levels L [--filter-window] --out <dir> [--ticker NAME]
//
// Reads <run>/orders.log, applies every request to an OrderBook, and turns the request's events
// into LOBSTER messages in event order (external id = engine OrderId, Direction = side of the
// order the message is about, which for an execution is the resting order):
//   Trade                          -> 4 on the maker: its qty, the trade price
//   Cancelled of a resting order   -> 3 with qty_cancelled (decided by whether the order rests in
//                                     the mirror book, not by the reason)
//   Modify that keeps priority     -> 2 with the reduction
//   Modify that loses priority     -> 3 for the old remaining first; then its 4s; then 1 if it rests
//   new order that rests           -> 1 with its resting qty, after its 4s
// A mirror book, driven only by insert_passive / reduce / remove, applies every message; the
// orderbook row after a message is the mirror's top L (empty levels: price 9999999999 for asks,
// -9999999999 for bids, size 0). After each request the mirror must equal the engine's book at
// every level, or the export stops with an error.
//
// --filter-window imitates LOBSTER's price-range filter: a message is written only if its price
// is inside the top-L window of the true book on its side before or after it (window: every price
// at least as good as the L-th level, the whole side if it has fewer than L levels). Rows show the
// true book after each written message.
//
// Times: 34200 s (9:30) plus the simulated time. Exit codes: 0 done, 1 bad arguments or an error.
//
// --lobster-like [--window-start-s S] [--hidden-rate R] [--halt-at-s H] [--seed N] makes the
// files look like a real LOBSTER day (all quirks together; --filter-window is switched on):
//   * only messages at sim time >= S (default 300) are written; the book built before S keeps
//     resting, so row 0 shows liquidity no message created;
//   * a Poisson stream (R per second, default 0.2) of type-5 messages with ids from
//     kHiddenIdBase up, a price between the true best bid and best ask inclusive, size 1-100, a
//     random direction; its row repeats the previous row;
//   * one type-7 message at sim time H (default: halfway through the window): price -1, size 0,
//     id 0, direction 1, row repeated (our own stand-in encoding of a halt);
//   * time = 34200 s + (sim time - S), 9 decimals; empty levels are price 0, size 0.
// It also writes README_SYNTHETIC.txt and <ticker>_injected.json (the injected counts) and
// refuses to write into a directory named data/lobster. To isolate one quirk at a time,
// --no-filter-window keeps every message, --no-halt leaves the halt out and --hidden-rate 0 the
// hidden executions (all three only with --lobster-like).
//
// --price-scale N (default 1) multiplies every real price written (messages and orderbook rows) by N, so
// simulator prices land in the units of the data they are compared with (N = the data's tick in LOBSTER
// price units). The empty-level placeholders (9999999999, -9999999999, 0) and the halt's price -1 are not
// scaled. An N for which the largest price the run can reach, times N, would reach 9999999999 is refused.
// --audit lines keep the simulator's own (unscaled) prices.
//
// --audit <file> (off by default) writes the exporter's ground truth: one CSV line per message it
// derived, written or not, in order: seq,sim_time_ns,type,id,size,price,direction,status,line.
// status is written, filtered (outside the window before and after), before_window or injected
// (a type 5 or 7 of --lobster-like); line is the message file line of a written or injected
// message, and for the others the number of lines written before it.
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "sim/lobster_export.hpp"

namespace {

using sim::kHiddenIdBase;
using sim::kLobsterNsPerSecond;
using sim::LikeOptions;
constexpr std::int64_t kNsPerSecond = kLobsterNsPerSecond;

int usage(const std::string& why) {
  std::fprintf(stderr, "lobsim_export_lobster: %s\n", why.c_str());
  std::fprintf(stderr,
               "usage: lobsim_export_lobster --run <sim output dir> --levels L [--filter-window] --out <dir> "
               "[--ticker NAME]\n"
               "       [--lobster-like [--window-start-s S] [--hidden-rate R] [--halt-at-s H] [--seed N]\n"
               "                       [--no-filter-window] [--no-halt]] [--audit FILE] [--price-scale N]\n");
  return 1;
}

void write_synthetic_notes(const std::filesystem::path& dir, const std::string& ticker, int levels, bool filter,
                           const LikeOptions& like, std::int64_t window_end_ns, const sim::ExportResult& e) {
  const auto seconds = [](std::int64_t ns) {  // integer math, 9 decimals
    const std::string f = std::to_string(ns % kNsPerSecond);
    return std::to_string(ns / kNsPerSecond) + "." + std::string(9 - f.size(), '0') + f;
  };
  const std::string window_s = seconds(window_end_ns - like.window_start_ns);
  const auto u = [](std::uint64_t v) { return std::to_string(v); };
  std::ofstream txt(dir / "README_SYNTHETIC.txt", std::ios::binary | std::ios::trunc);
  txt << "SYNTHETIC DATA - NOT MARKET DATA\n"
         "================================\n"
         "These files were generated by lobsim_export_lobster --lobster-like from a simulator run.\n"
         "Ticker " << ticker << " is a made-up name and the date in the file names is only a format placeholder.\n"
         "Do not treat any number computed from them as a result about a real market.\n\n"
         "Generator: lobsim_export_lobster --run <sim output dir> --levels " << levels << " --lobster-like"
         " --window-start-s " << like.window_start_ns / kNsPerSecond << " --hidden-rate " << like.hidden_rate
      << (like.halt_at_ns ? " --halt-at-s " + std::to_string(*like.halt_at_ns / kNsPerSecond) : std::string())
      << " --seed " << like.seed << (filter ? "" : " --no-filter-window") << (like.halt ? "" : " --no-halt")
      << " --ticker " << ticker << " --out <dir>\n\n"
         "Conventions (our own choices; real LOBSTER files may differ):\n"
         "  * Empty levels in the orderbook file are price 0 and size 0.\n"
         "  * A halt is one type-7 message: price -1, size 0, id 0, direction 1; its orderbook row repeats the\n"
         "    previous row.\n"
         "  * Hidden executions are type-5 messages with ids " << kHiddenIdBase << " and up (2^40 + k, k = 0, 1, ...),\n"
         "    never used by any other message; the price is between the best bid and best ask of the true book,\n"
         "    the size 1-100, the direction random; the orderbook row repeats the previous row.\n"
         "  * Times are 34200 s plus the seconds since the window start, 9 decimals.\n"
         "  * The export starts at simulated second " << like.window_start_ns / kNsPerSecond
      << "; orders placed before it rest in the book without a message of their own.\n"
      << (filter ? "  * --filter-window is on: messages outside the top-" + std::to_string(levels) +
                       " window of the true book are left out.\n\n"
                 : std::string("  * --no-filter-window: every message from the window start on is written.\n\n"))
      <<
         "Injected counts:\n"
         "  hidden executions written  " << u(e.hidden_written) << "\n"
         "  hidden draws skipped       " << u(e.hidden_skipped) << " (the book was not two-sided)\n"
         "  halts written              " << u(e.halts_written)
      << (like.halt ? " (at simulated time " + seconds(e.halt_ns) + " s)" : std::string(" (--no-halt)")) << "\n"
         "  messages filtered          " << u(e.dropped) << " (outside the top-" << levels << " window)\n"
         "  messages before the window " << u(e.before_window) << "\n"
         "  messages written           " << u(e.written) << " (including the injected ones)\n"
         "  window start               " << like.window_start_ns / kNsPerSecond << " s of simulated time\n"
         "  window duration            " << window_s << " s\n";
  std::ofstream js(dir / (ticker + "_injected.json"), std::ios::binary | std::ios::trunc);
  js << "{\n  \"synthetic\": true,\n  \"hidden_exec_written\": " << u(e.hidden_written)
     << ",\n  \"hidden_skipped\": " << u(e.hidden_skipped) << ",\n  \"halt_written\": " << u(e.halts_written)
     << ",\n  \"messages_filtered\": " << u(e.dropped) << ",\n  \"messages_before_window\": " << u(e.before_window)
     << ",\n  \"messages_written\": " << u(e.written) << ",\n  \"hidden_id_base\": " << kHiddenIdBase
     << ",\n  \"window_start_s\": " << like.window_start_ns / kNsPerSecond << ",\n  \"window_duration_s\": \"" << window_s
     << "\"\n}\n";
  txt.flush();
  js.flush();
  if (!txt || !js) throw std::runtime_error("error writing the synthetic notes into " + dir.string());
}

}  // namespace

int main(int argc, char** argv) {
  std::string run_dir, out_dir, ticker = "SIM";
  int levels = 0;
  std::string audit_path;
  bool filter = false;
  bool no_filter = false;
  std::int64_t price_scale = 1;
  LikeOptions like;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--filter-window") {
      filter = true;
      continue;
    }
    if (a == "--lobster-like") {
      like.on = true;
      continue;
    }
    if (a == "--no-filter-window") {
      no_filter = true;
      continue;
    }
    if (a == "--no-halt") {
      like.halt = false;
      continue;
    }
    if (i + 1 >= argc) return usage(a + " needs a value");
    const std::string_view v = argv[++i];
    if (a == "--run") {
      run_dir = v;
    } else if (a == "--out") {
      out_dir = v;
    } else if (a == "--ticker") {
      ticker = v;
    } else if (a == "--audit") {
      audit_path = v;
      if (audit_path.empty()) return usage("--audit needs a file name");
    } else if (a == "--window-start-s" || a == "--halt-at-s") {
      std::int64_t secs = 0;
      const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), secs);
      if (v.empty() || ec != std::errc{} || p != v.data() + v.size() || secs < 0 || secs > 1'000'000) {
        return usage(a + " must be a whole number of seconds, 0 to 1000000");
      }
      if (a == "--window-start-s") {
        like.window_start_ns = secs * kNsPerSecond;
      } else {
        like.halt_at_ns = secs * kNsPerSecond;
      }
    } else if (a == "--hidden-rate") {
      const std::string text(v);
      char* end = nullptr;
      like.hidden_rate = std::strtod(text.c_str(), &end);
      if (text.empty() || end != text.c_str() + text.size() || !(like.hidden_rate >= 0.0) || like.hidden_rate > 1e6) {
        return usage("--hidden-rate must be a number from 0 to 1000000 (per second)");
      }
    } else if (a == "--price-scale") {
      const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), price_scale);
      if (v.empty() || ec != std::errc{} || p != v.data() + v.size() || price_scale < 1) {
        return usage("--price-scale must be a positive integer");
      }
    } else if (a == "--seed") {
      const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), like.seed);
      if (v.empty() || ec != std::errc{} || p != v.data() + v.size()) return usage("--seed must be a non-negative integer");
    } else if (a == "--levels") {
      const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), levels);
      if (v.empty() || ec != std::errc{} || p != v.data() + v.size() || levels < 1) {
        return usage("--levels must be a positive integer");
      }
    } else {
      return usage("unknown argument " + a);
    }
  }
  if (run_dir.empty() || out_dir.empty() || levels < 1) return usage("--run, --levels and --out are required");
  if (ticker.empty() || ticker.find('/') != std::string::npos) return usage("--ticker must be a non-empty name without '/'");

  if ((no_filter || !like.halt) && !like.on) return usage("--no-filter-window and --no-halt need --lobster-like");
  if (no_filter && filter) return usage("--filter-window and --no-filter-window contradict each other");
  if (!like.halt && like.halt_at_ns) return usage("--halt-at-s and --no-halt contradict each other");
  if (like.on) {
    filter = !no_filter;
    const std::filesystem::path norm = std::filesystem::absolute(out_dir).lexically_normal();
    std::filesystem::path leaf = norm.filename().empty() ? norm.parent_path() : norm;  // drop a trailing '/'
    if (leaf.filename() == "lobster" && leaf.parent_path().filename() == "data") {
      std::fprintf(stderr,
                   "lobsim_export_lobster: refusing to write --lobster-like (synthetic) files into %s; "
                   "synthetic data must never be mistaken for real data (use data/synthetic)\n",
                   out_dir.c_str());
      return 1;
    }
  }
  const std::filesystem::path log_path = std::filesystem::path(run_dir) / "orders.log";
  std::ifstream log(log_path, std::ios::binary);
  if (!log) {
    std::fprintf(stderr, "lobsim_export_lobster: cannot read %s\n", log_path.string().c_str());
    return 1;
  }
  const std::string stem = ticker + "_2012-06-21_34200000_57600000_";
  const std::filesystem::path dir(out_dir);
  const std::filesystem::path msg_path = dir / (stem + "message_" + std::to_string(levels) + ".csv");
  const std::filesystem::path book_path = dir / (stem + "orderbook_" + std::to_string(levels) + ".csv");
  try {
    std::filesystem::create_directories(dir);
    std::ofstream messages(msg_path, std::ios::binary | std::ios::trunc);
    std::ofstream orderbook(book_path, std::ios::binary | std::ios::trunc);
    if (!messages || !orderbook) throw std::runtime_error("cannot write into " + out_dir);
    std::ofstream audit;
    if (!audit_path.empty()) {
      audit.open(audit_path, std::ios::binary | std::ios::trunc);
      if (!audit) throw std::runtime_error("cannot write " + audit_path);
    }
    sim::ExportSpec spec;
    spec.levels = levels;
    spec.filter = filter;
    spec.like = like;
    spec.price_scale = price_scale;
    const sim::ExportResult exporter = sim::export_order_log(log, spec, messages, orderbook,
                                                              audit_path.empty() ? nullptr : &audit);
    const std::int64_t window_end_ns = exporter.window_end_ns;
    messages.flush();
    orderbook.flush();
    if (!messages || !orderbook) throw std::runtime_error("error writing into " + out_dir);
    if (!audit_path.empty()) {
      audit.flush();
      if (!audit) throw std::runtime_error("error writing " + audit_path);
    }
    const std::uint64_t* t = exporter.by_type;
    std::printf("requests             %llu\n", static_cast<unsigned long long>(exporter.requests));
    std::printf("messages written     %llu (type 1 %llu, 2 %llu, 3 %llu, 4 %llu)\n",
                static_cast<unsigned long long>(exporter.written), static_cast<unsigned long long>(t[1]),
                static_cast<unsigned long long>(t[2]), static_cast<unsigned long long>(t[3]),
                static_cast<unsigned long long>(t[4]));
    std::printf("messages filtered    %llu%s\n", static_cast<unsigned long long>(exporter.dropped),
                filter ? " (outside the top-L window before and after)" : "");
    if (like.on) {
      std::printf("before the window    %llu\n", static_cast<unsigned long long>(exporter.before_window));
      std::printf("hidden executions    %llu (draws skipped, book not two-sided: %llu)\n",
                  static_cast<unsigned long long>(exporter.hidden_written),
                  static_cast<unsigned long long>(exporter.hidden_skipped));
      std::printf("halts                %llu\n", static_cast<unsigned long long>(exporter.halts_written));
      write_synthetic_notes(dir, ticker, levels, filter, like, window_end_ns, exporter);
    }
    std::printf("wrote                %s\n                     %s\n", msg_path.string().c_str(),
                book_path.string().c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_export_lobster: %s\n", e.what());
    return 1;
  }
  return 0;
}
