#pragma once
// lobsim_calibrate compare: parameter recovery (upgrade 3, C3). See calibrate_compare.cpp.
#include <string>
#include <string_view>
#include <vector>

namespace calibrate_cli {

inline constexpr std::string_view kCompareUsage =
    "--truth <json> --estimate <json> [--tolerance-scale K] [--min-samples N]\n"
    "                  [--filtered --depth-p95-from <orderbook file>] [--md <file>]";

int compare_command(const std::vector<std::string>& args);

}  // namespace calibrate_cli
