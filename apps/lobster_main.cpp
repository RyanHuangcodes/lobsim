// lobsim_lobster: replay a LOBSTER message file and compare the book with its orderbook file.
//
//   lobsim_lobster --messages <file> --orderbook <file> [--mode strict|report|resync]
//                  [--compare-depth D] [--trust-depth T] [--out <dir>]
//
// The level count L comes from the orderbook file name (..._orderbook_<L>.csv) and must agree
// with its column count. With --out, writes summary.json and divergences.csv there. Prints a
// one-screen summary. Exit codes: 0 done, 1 bad arguments or an input that exists but cannot be
// replayed, 2 strict mode found a row that is not exact down to --compare-depth, 3 a --messages or
// --orderbook path does not exist (data missing: the message says which files to get and where to put them).
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
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

// The names of a LOBSTER pair, derived from whichever of the two paths follows the pattern
// <TICKER>_<DATE>_<START>_<END>_(message|orderbook)_<L>.csv. Empty when neither does.
struct ExpectedNames {
  std::string message, orderbook, readme;
};

ExpectedNames expected_names(const std::string& messages, const std::string& orderbook) {
  static const std::regex pattern(R"(^(.+_\d{4}-\d{2}-\d{2}_\d+_\d+)_(message|orderbook)_(\d+)\.csv$)");
  for (const std::string& path : {orderbook, messages}) {
    const std::string name = std::filesystem::path(path).filename().string();
    std::smatch m;
    if (std::regex_match(name, m, pattern)) {
      const std::string stem = m[1], levels = m[3];
      return {stem + "_message_" + levels + ".csv", stem + "_orderbook_" + levels + ".csv", stem + "_README.txt"};
    }
  }
  return {};
}

// Exit code 3: say which files are missing, where to put them, and how to get them.
int data_missing(const std::string& messages, const std::string& orderbook) {
  const ExpectedNames names = expected_names(messages, orderbook);
  std::fprintf(stderr, "lobsim_lobster: data missing\n");
  for (const std::string* p : {&messages, &orderbook}) {
    if (!std::filesystem::exists(*p)) std::fprintf(stderr, "  not found: %s\n", p->c_str());
  }
  if (names.message.empty()) {
    std::fprintf(stderr,
                 "  expected file names: <TICKER>_<DATE>_<START>_<END>_message_<L>.csv and\n"
                 "                       <TICKER>_<DATE>_<START>_<END>_orderbook_<L>.csv\n"
                 "  (e.g. INTC_2012-06-21_34200000_57600000_message_10.csv), plus the sample's README file\n");
  } else {
    std::fprintf(stderr, "  expected file names: %s\n                       %s\n                       %s (the sample's README)\n",
                 names.message.c_str(), names.orderbook.c_str(), names.readme.c_str());
  }
  std::fprintf(stderr, "  put them in the folder data/lobster/ and run scripts/run_real_data.sh\n");
  std::fprintf(stderr,
               "  LOBSTER data needs a LOBSTER account, a university subscription, an approved trial, or the sample "
               "that comes with the book Trades, Quotes and Prices (https://lobsterdata.com)\n");
  return 3;
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

  if (!std::filesystem::exists(messages) || !std::filesystem::exists(orderbook)) {
    return data_missing(messages, orderbook);
  }

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
