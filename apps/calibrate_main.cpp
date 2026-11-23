// lobsim_calibrate: calibration of the background order flow (upgrade 3).
//
//   lobsim_calibrate estimate --messages <file> --orderbook <file> [--from S] [--to S]
//                             [--tick-units auto|N] [--source real|synthetic] [--out <dir>]
//
// estimate: measures the order flow of one LOBSTER-format day inside the window [--from, --to)
// (whole seconds after midnight; default 36000-55800, or the data's whole span when that is not
// covered and neither bound is given) and writes <out>/<TICKER>_<DATE>_<from>-<to>.json (schema:
// calibration/SCHEMA.md) and a short .md summary beside it. --out defaults to calibration/.
// Ticker, date and L come from the LOBSTER file names.
//
// Exit codes: 0 done; 1 bad arguments, or input that exists but cannot be estimated; 3 a
// --messages or --orderbook path does not exist (data missing).
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
#include <vector>

#include "calib/calibration.hpp"
#include "calib/estimator.hpp"
#include "replay/replayer.hpp"

namespace {

constexpr std::int64_t kMaxSeconds = 86'400;

struct Subcommand {
  std::string_view name;
  std::string_view usage;  // the arguments after the subcommand's name
  int (*run)(const std::vector<std::string>& args);
};

int estimate_command(const std::vector<std::string>& args);

constexpr std::string_view kEstimateUsage =
    "--messages <file> --orderbook <file> [--from S] [--to S] [--tick-units auto|N]\n"
    "                  [--source real|synthetic] [--out <dir>]";

// Every subcommand; a new one is one more line here.
constexpr Subcommand kSubcommands[] = {
    {"estimate", kEstimateUsage, estimate_command},
};

void print_usage() {
  std::fprintf(stderr, "usage:\n");
  for (const Subcommand& s : kSubcommands) {
    std::fprintf(stderr, "  lobsim_calibrate %.*s %.*s\n", static_cast<int>(s.name.size()), s.name.data(),
                 static_cast<int>(s.usage.size()), s.usage.data());
  }
}

int usage(const std::string& why) {
  std::fprintf(stderr, "lobsim_calibrate: %s\n", why.c_str());
  print_usage();
  return 1;
}

std::optional<std::int64_t> parse_int(std::string_view s) {
  std::int64_t v = 0;
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
  return static_cast<bool>(f);
}

int data_missing(const std::string& messages, const std::string& orderbook) {
  std::fprintf(stderr, "lobsim_calibrate: data missing\n");
  for (const std::string* p : {&messages, &orderbook}) {
    if (!std::filesystem::exists(*p)) std::fprintf(stderr, "  not found: %s\n", p->c_str());
  }
  std::fprintf(stderr,
               "  expected a LOBSTER pair <TICKER>_<DATE>_<START>_<END>_message_<L>.csv and ..._orderbook_<L>.csv,\n"
               "  e.g. in data/lobster/ (see scripts/run_real_data.sh), or a fake day from scripts/make_fake_day.sh\n");
  return 3;
}

int estimate_command(const std::vector<std::string>& args) {
  std::string messages, orderbook, out_dir = "calibration";
  calib::EstimateOptions options;
  for (std::size_t i = 0; i < args.size(); i += 2) {  // every flag takes one value
    const std::string& a = args[i];
    if (i + 1 >= args.size()) return usage(a + " needs a value");
    const std::string& v = args[i + 1];
    if (a == "--messages") {
      messages = v;
    } else if (a == "--orderbook") {
      orderbook = v;
    } else if (a == "--out") {
      out_dir = v;
    } else if (a == "--from" || a == "--to") {
      const std::optional<std::int64_t> s = parse_int(v);
      if (!s || *s < 0 || *s > kMaxSeconds) {
        return usage(a + " must be whole seconds after midnight, 0 to " + std::to_string(kMaxSeconds));
      }
      (a == "--from" ? options.from_ns : options.to_ns) = *s * calib::kNsPerSecond;
    } else if (a == "--tick-units") {
      if (v != "auto") {
        const std::optional<std::int64_t> n = parse_int(v);
        if (!n || *n < 1) return usage("--tick-units must be auto or a positive integer");
        options.tick_units = *n;
      }
    } else if (a == "--source") {
      if (v != "real" && v != "synthetic") return usage("--source must be real or synthetic");
      options.source = v;
    } else {
      return usage("unknown argument " + a);
    }
  }
  if (messages.empty() || orderbook.empty()) return usage("--messages and --orderbook are required");
  if (options.from_ns && options.to_ns && *options.from_ns >= *options.to_ns) return usage("--from must be before --to");
  if (!std::filesystem::exists(messages) || !std::filesystem::exists(orderbook)) {
    return data_missing(messages, orderbook);
  }

  calib::EstimateResult r;
  const auto t0 = std::chrono::steady_clock::now();
  try {
    r = calib::estimate_files(messages, orderbook, options);
  } catch (const std::invalid_argument& e) {
    return usage(e.what());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_calibrate: %s\n", e.what());
    return 1;
  }
  const auto wall_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

  for (const std::string& w : r.warnings) std::fprintf(stderr, "lobsim_calibrate: warning: %s\n", w.c_str());
  const calib::Calibration& c = r.calibration;
  const std::filesystem::path dir(out_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::fprintf(stderr, "lobsim_calibrate: cannot create %s\n", out_dir.c_str());
    return 1;
  }
  const std::string stem = calib::output_stem(c);
  const std::filesystem::path json_path = dir / (stem + ".json");
  const std::filesystem::path md_path = dir / (stem + ".md");
  try {
    calib::save(json_path.string(), c);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_calibrate: %s\n", e.what());
    return 1;
  }
  if (!write_file(md_path, calib::summary_markdown(c, r.warnings))) {
    std::fprintf(stderr, "lobsim_calibrate: cannot write %s\n", md_path.string().c_str());
    return 1;
  }
  std::printf("source        %s%s\n", c.source.c_str(), c.source == "synthetic" ? " (NOT market data)" : "");
  std::printf("window        %s\n", c.window_note.c_str());
  std::printf("limit/s       buy %.3f  sell %.3f\n", c.rates.limit[calib::kBuy].per_s, c.rates.limit[calib::kSell].per_s);
  std::printf("aggressive/s  buy %.3f  sell %.3f\n", c.rates.aggressive[calib::kBuy].per_s,
              c.rates.aggressive[calib::kSell].per_s);
  std::printf("tick_units    %lld, median mid %lld ticks\n", static_cast<long long>(c.tick_units),
              static_cast<long long>(c.median_mid_ticks));
  std::printf("wrote         %s\n              %s\n", json_path.string().c_str(), md_path.string().c_str());
  std::printf("run time      %lld ms\n", static_cast<long long>(wall_ms));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage("a subcommand is required");
  const std::string_view name = argv[1];
  for (const Subcommand& s : kSubcommands) {
    if (s.name == name) return s.run(std::vector<std::string>(argv + 2, argv + argc));
  }
  return usage("unknown subcommand " + std::string(name));
}
