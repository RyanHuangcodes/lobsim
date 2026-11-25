#pragma once
// `lobsim_calibrate validate` (upgrade 3, C5): the command line over calib::run_validation.
#include <string>
#include <string_view>
#include <vector>

// Exit codes: 0 done; 1 bad arguments, a failed run, or the leakage check; 3 an input file does not exist.
int validate_command(const std::vector<std::string>& args);

inline constexpr std::string_view kValidateUsage =
    "--messages <file> --orderbook <file> --train A-B --test C-D [--seeds 20] [--out <dir>]\n"
    "                  [--tick-units auto|N] [--calibration <file>] [--start-price N] [--warmup-s S]\n"
    "                  [--threads N] [--resamples N] [--no-plots] [--keep-exports] [--plot-script <file>]";
