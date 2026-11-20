// lobsim_lobster: replay a LOBSTER message file and compare the book with its orderbook file.
//
//   lobsim_lobster --messages <file> --orderbook <file> [--mode strict|report|resync]
//                  [--compare-depth D] [--trust-depth T] [--out <dir>]
//
// The level count L comes from the orderbook file name (..._orderbook_<L>.csv) and must agree
// with its column count. With --out, writes summary.json and divergences.csv there. Prints a
// one-screen summary. Exit codes: 0 done, 1 bad arguments or an input that cannot be replayed,
// 2 strict mode found a row that is not exact down to --compare-depth.
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "replay/replayer.hpp"

namespace {

int usage(const std::string& why) {
  std::fprintf(stderr, "lobsim_lobster: %s\n", why.c_str());
  std::fprintf(stderr,
               "usage: lobsim_lobster --messages <file> --orderbook <file> [--mode strict|report|resync]\n"
               "                      [--compare-depth D] [--trust-depth T] [--out <dir>]\n");
  return 1;
}

std::optional<int> parse_int(std::string_view s) {
  int v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

bool write_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (f) {
    f << text;
    f.flush();
  }
  if (!f) {
    std::fprintf(stderr, "lobsim_lobster: cannot write %s\n", path.string().c_str());
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string messages, orderbook, out_dir;
  replay::ReplayOptions options;
  for (int i = 1; i < argc; i += 2) {  // every flag takes one value
    const std::string a = argv[i];
    if (i + 1 >= argc) return usage(a + " needs a value");
    const std::string v = argv[i + 1];
    if (a == "--messages") {
      messages = v;
    } else if (a == "--orderbook") {
      orderbook = v;
    } else if (a == "--out") {
      out_dir = v;
    } else if (a == "--mode") {
      const auto m = replay::parse_mode(v);
      if (!m) return usage("--mode must be strict, report or resync");
      options.mode = *m;
    } else if (a == "--compare-depth") {
      const auto d = parse_int(v);
      if (!d) return usage("--compare-depth must be an integer");
      options.compare_depth = *d;
    } else if (a == "--trust-depth") {
      const auto d = parse_int(v);
      if (!d) return usage("--trust-depth must be an integer");
      options.trust_depth = *d;
    } else {
      return usage("unknown argument " + a);
    }
  }
  if (messages.empty() || orderbook.empty()) return usage("--messages and --orderbook are required");

  replay::Summary summary;
  std::int64_t wall_ns = 0;
  try {
    const auto t0 = std::chrono::steady_clock::now();
    summary = replay::replay_files(messages, orderbook, options);
    wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
  } catch (const std::invalid_argument& e) {
    return usage(e.what());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_lobster: %s\n", e.what());
    return 1;
  }

  std::fputs(replay::text_summary(summary, wall_ns).c_str(), stdout);
  if (summary.strict_failure) std::fputs(replay::strict_failure_report(summary).c_str(), stdout);
  if (!out_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    const std::filesystem::path dir(out_dir);
    if (ec || !write_file(dir / "summary.json", replay::summary_json(summary, wall_ns)) ||
        !write_file(dir / "divergences.csv", replay::divergences_csv(summary))) {
      if (ec) std::fprintf(stderr, "lobsim_lobster: cannot create %s\n", out_dir.c_str());
      return 1;
    }
  }
  return summary.strict_failure ? 2 : 0;
}
