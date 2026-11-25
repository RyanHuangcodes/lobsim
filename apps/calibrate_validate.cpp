// lobsim_calibrate validate: out-of-sample validation of the calibrated background flow (spec upgrade 3, 6).
//
//   lobsim_calibrate validate --messages M --orderbook O --train A-B --test C-D [--seeds 20] [--out DIR]
//                             [--tick-units auto|N] [--calibration FILE] [--start-price N] [--warmup-s S]
//                             [--threads N] [--resamples N] [--no-plots] [--keep-exports] [--plot-script FILE]
//
// A-B and C-D are seconds after midnight; the training period must end at or before the test period starts.
// The estimator reads only [A, B). With --calibration FILE the estimation is skipped and that file is used (the
// cross-ticker check: calibrate on one ticker, validate on another; pass --start-price for the new ticker).
// Writes validation.md, validation.json, the calibration (JSON and .md) and plots/ into --out (default
// build/validation/<TICKER>_<DATE>/). The plots need python3 with matplotlib and numpy; without them the run
// says so and goes on.
#include "calibrate_validate.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "calib/validate.hpp"

#include "calib/estimator.hpp"

namespace {

namespace fs = std::filesystem;

int usage(const std::string& why) {
  std::fprintf(stderr, "lobsim_calibrate: %s\nusage:\n  lobsim_calibrate validate %.*s\n", why.c_str(),
               static_cast<int>(kValidateUsage.size()), kValidateUsage.data());
  return 1;
}

template <class T>
bool parse_number(const std::string& s, T& v) {
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  return !s.empty() && ec == std::errc{} && p == s.data() + s.size();
}

bool parse_double(const std::string& s, double& v) {
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return !s.empty() && end == s.c_str() + s.size();
}

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

#ifdef LOBSIM_PLOT_SCRIPT
constexpr const char* kDefaultPlotScript = LOBSIM_PLOT_SCRIPT;
#else
constexpr const char* kDefaultPlotScript = "tools/plot_calibration.py";
#endif

// Runs the plot script when python3 with matplotlib and numpy is found. Returns whether plots were drawn.
bool draw_plots(const std::string& script, const fs::path& out_dir) {
  if (std::system("python3 -c 'import matplotlib, numpy' >/dev/null 2>&1") != 0) {
    std::printf("plots                not drawn: python3 with matplotlib and numpy was not found\n");
    return false;
  }
  if (!fs::exists(script)) {
    std::printf("plots                not drawn: the plot script %s does not exist\n", script.c_str());
    return false;
  }
  const std::string cmd = "python3 " + shell_quote(script) + " --validation " + shell_quote((out_dir / "validation.json").string()) +
                          " --out " + shell_quote((out_dir / "plots").string());
  if (std::system(cmd.c_str()) != 0) {
    std::printf("plots                not drawn: the plot script failed (%s)\n", cmd.c_str());
    return false;
  }
  std::printf("plots                %s\n", (out_dir / "plots").string().c_str());
  return true;
}

calib::EstimateFn make_estimator() {
  return [](const std::string& messages, const std::string& orderbook, std::int64_t from_ns, std::int64_t to_ns,
            std::optional<std::int64_t> tick_units) {
    calib::EstimateOptions eo;
    eo.from_ns = from_ns;
    eo.to_ns = to_ns;
    eo.tick_units = tick_units;
    calib::EstimateResult r = calib::estimate_files(messages, orderbook, eo);
    calib::EstimatorOutput out;
    out.first_message_ns = r.first_message_ns;
    out.last_message_ns = r.last_message_ns;
    out.summary_markdown = calib::summary_markdown(r.calibration, r.warnings);
    out.calibration = std::move(r.calibration);
    return out;
  };
}

}  // namespace

int validate_command(const std::vector<std::string>& args) {
  calib::ValidateOptions o;
  std::string train_text, test_text, plot_script = kDefaultPlotScript;
  bool plots = true;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--no-plots") {
      plots = false;
      continue;
    }
    if (a == "--keep-exports") {
      o.keep_exports = true;
      continue;
    }
    if (i + 1 >= args.size()) return usage(a + " needs a value");
    const std::string& v = args[++i];
    std::int64_t n = 0;
    if (a == "--messages") o.messages = v;
    else if (a == "--orderbook") o.orderbook = v;
    else if (a == "--out") o.out_dir = v;
    else if (a == "--train") train_text = v;
    else if (a == "--test") test_text = v;
    else if (a == "--calibration") o.calibration_file = v;
    else if (a == "--plot-script") plot_script = v;
    else if (a == "--tick-units") {
      if (v == "auto") o.tick_units.reset();
      else if (parse_number(v, n) && n >= 1) o.tick_units = n;
      else return usage("--tick-units must be auto or a positive integer");
    } else if (a == "--start-price") {
      if (!parse_number(v, n) || n < 1) return usage("--start-price must be a positive integer (ticks)");
      o.start_price = n;
    } else if (a == "--seeds") {
      if (!parse_number(v, n) || n < 2 || n > 1000) return usage("--seeds must be an integer from 2 to 1000");
      o.seeds = static_cast<int>(n);
    } else if (a == "--threads") {
      if (!parse_number(v, n) || n < 0 || n > 256) return usage("--threads must be an integer from 0 to 256 (0: automatic)");
      o.threads = static_cast<int>(n);
    } else if (a == "--resamples") {
      if (!parse_number(v, n) || n < 20 || n > 100000) return usage("--resamples must be an integer from 20 to 100000");
      o.bootstrap_resamples = static_cast<int>(n);
    } else if (a == "--warmup-s") {
      if (!parse_double(v, o.warmup_s) || !(o.warmup_s >= 0.0)) return usage("--warmup-s must be a number >= 0");
    } else {
      return usage("unknown argument " + a);
    }
  }
  if (o.messages.empty() || o.orderbook.empty() || train_text.empty() || test_text.empty()) {
    return usage("--messages, --orderbook, --train and --test are required");
  }
  try {
    o.train = calib::parse_period(train_text, "--train");
    o.test = calib::parse_period(test_text, "--test");
    calib::check_period_order(o.train, o.test);
  } catch (const std::invalid_argument& e) {
    return usage(e.what());
  }
  const calib::EstimateFn estimator = make_estimator();

  const auto t0 = std::chrono::steady_clock::now();  // wall clock: shown on stdout, never written to a file
  try {
    const calib::ValidateResult r = calib::run_validation(o, estimator, [](const std::string& s) {
      std::fprintf(stderr, "lobsim_calibrate: %s\n", s.c_str());
    });
    const fs::path dir(r.out_dir);
    const bool drawn = plots && draw_plots(plot_script, dir);
    if (!plots) std::printf("plots                not drawn (--no-plots)\n");
    {
      std::ofstream md(dir / "validation.md", std::ios::binary | std::ios::trunc);
      md << calib::render_markdown(r.json, drawn);
      md.flush();
      if (!md) {
        std::fprintf(stderr, "lobsim_calibrate: cannot write %s\n", (dir / "validation.md").string().c_str());
        return 1;
      }
    }
    const auto& c = r.json.at("verdict_counts");
    std::printf("label                %s\n", r.json.at("label").get<std::string>().c_str());
    std::printf("calibrated verdicts  %d match, %d close, %d miss, %d undefined\n", c.at("calibrated").at("match").get<int>(),
                c.at("calibrated").at("close").get<int>(), c.at("calibrated").at("miss").get<int>(), c.at("calibrated").at("n/a").get<int>());
    std::printf("baseline verdicts    %d match, %d close, %d miss, %d undefined\n", c.at("baseline").at("match").get<int>(),
                c.at("baseline").at("close").get<int>(), c.at("baseline").at("miss").get<int>(), c.at("baseline").at("n/a").get<int>());
    std::printf("wrote                %s and validation.json, %s\n", (dir / "validation.md").string().c_str(),
                fs::path(r.calibration_path).filename().string().c_str());
    std::printf("elapsed (wall clock) %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  } catch (const calib::DataMissingError& e) {
    std::fprintf(stderr, "lobsim_calibrate: %s\n", e.what());
    return 3;
  } catch (const calib::LeakageError& e) {
    std::fprintf(stderr, "lobsim_calibrate: ABORTED: %s\n", e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_calibrate: %s\n", e.what());
    return 1;
  }
  return 0;
}
