// lobsim_calibrate compare: checks that an estimated calibration recovers the hand-made truth the
// simulation sampled (spec upgrade 3, section 4.3).
//
//   lobsim_calibrate compare --truth <json> --estimate <json> [--tolerance-scale K] [--min-samples N]
//                            [--filtered --depth-p95-from <orderbook file>] [--md <file>]
//
// Prints one row per check (rates, placement per side and spread bucket, sizes, lifetime medians,
// and the fit for information) and writes the same table as Markdown with --md. --filtered judges
// placement only up to the 95th percentile of the published window's depth, read from the filtered
// day's orderbook file (L from its name, ticks from the estimate's tick_units), reports the rates'
// bias instead of judging it, and uses the filtered lifetime tolerance.
//
// Exit codes: 0 every check passed; 1 bad arguments or an input that cannot be read; 2 a check
// failed; 3 an input file does not exist.
#include "calibrate_compare.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "calib/calibration.hpp"
#include "calib/recovery.hpp"
#include "replay/lobster.hpp"

namespace calibrate_cli {
namespace {

int fail_usage(const std::string& why) {
  std::fprintf(stderr, "lobsim_calibrate compare: %s\n", why.c_str());
  std::fprintf(stderr, "usage: lobsim_calibrate compare %.*s\n", static_cast<int>(kCompareUsage.size()),
               kCompareUsage.data());
  return 1;
}

std::optional<double> parse_double(const std::string& s) {
  try {
    std::size_t used = 0;
    const double v = std::stod(s, &used);
    if (used != s.size()) return std::nullopt;
    return v;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::uint64_t> parse_u64(const std::string& s) {
  std::uint64_t v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

}  // namespace

int compare_command(const std::vector<std::string>& args) {
  std::string truth_path, estimate_path, depth_path, md_path;
  calib::CompareOptions o;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--filtered") {
      o.filtered = true;
      continue;
    }
    if (i + 1 >= args.size()) return fail_usage(a + " needs a value");
    const std::string& v = args[++i];
    if (a == "--truth") {
      truth_path = v;
    } else if (a == "--estimate") {
      estimate_path = v;
    } else if (a == "--depth-p95-from") {
      depth_path = v;
    } else if (a == "--md") {
      md_path = v;
    } else if (a == "--tolerance-scale") {
      const std::optional<double> k = parse_double(v);
      if (!k || !(*k > 0.0)) return fail_usage("--tolerance-scale must be a positive number");
      o.tolerance_scale = *k;
    } else if (a == "--min-samples") {
      const std::optional<std::uint64_t> n = parse_u64(v);
      if (!n) return fail_usage("--min-samples must be a non-negative integer");
      o.min_samples = *n;
    } else {
      return fail_usage("unknown argument " + a);
    }
  }
  if (truth_path.empty() || estimate_path.empty()) return fail_usage("--truth and --estimate are required");
  if (o.filtered != !depth_path.empty()) return fail_usage("--filtered and --depth-p95-from go together");
  for (const std::string* p : {&truth_path, &estimate_path, &depth_path}) {
    if (!p->empty() && !std::filesystem::exists(*p)) {
      std::fprintf(stderr, "lobsim_calibrate compare: not found: %s\n", p->c_str());
      return 3;
    }
  }

  calib::Comparison result;
  std::string depth_note;
  try {
    const calib::Calibration truth = calib::load(truth_path);
    const calib::Calibration estimate = calib::load(estimate_path);
    if (o.filtered) {
      const std::optional<int> levels = replay::levels_from_filename(depth_path);
      if (!levels) return fail_usage("--depth-p95-from needs a LOBSTER orderbook file name (..._orderbook_<L>.csv)");
      std::ifstream book(depth_path, std::ios::binary);
      if (!book) throw std::runtime_error("cannot read " + depth_path);
      const calib::WindowDepth d = calib::window_depth(book, *levels, estimate.tick_units);
      o.max_delta = d.p95;
      for (int s = 0; s < 2; ++s) {
        depth_note += std::string(s == calib::kBuy ? "bid" : "ask") + " side: p95 depth " +
                      (d.p95[s] ? std::to_string(*d.p95[s]) + " ticks" : std::string("none (never L levels)")) +
                      " over " + std::to_string(d.rows[s]) + " rows with L levels (" +
                      std::to_string(d.fewer_levels[s]) + " with fewer)" + (s == calib::kBuy ? "; " : "");
      }
      if (d.malformed > 0) depth_note += "; " + std::to_string(d.malformed) + " malformed rows skipped";
    }
    result = calib::compare_recovery(truth, estimate, o);
  } catch (const std::invalid_argument& e) {
    return fail_usage(e.what());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_calibrate compare: %s\n", e.what());
    return 1;
  }

  if (!depth_note.empty()) std::printf("window depth (L from %s): %s\n", depth_path.c_str(), depth_note.c_str());
  std::fputs(calib::comparison_text(result).c_str(), stdout);
  std::printf("passed by group: rate %zu, placement %zu, size %zu, lifetime median %zu\n", result.passed_in("rate"),
              result.passed_in("placement"), result.passed_in("size"), result.passed_in("lifetime median"));
  if (!md_path.empty()) {
    std::ofstream md(md_path, std::ios::binary | std::ios::trunc);
    if (md) {
      if (!depth_note.empty()) md << "Window depth: " << depth_note << ".\n\n";
      md << calib::comparison_markdown(result);
      md.flush();
    }
    if (!md) {
      std::fprintf(stderr, "lobsim_calibrate compare: cannot write %s\n", md_path.c_str());
      return 1;
    }
  }
  return result.passed() ? 0 : 2;
}

}  // namespace calibrate_cli
