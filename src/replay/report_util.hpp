#pragma once

// Helpers shared by the replay reports (replayer_json.cpp, replayer_text.cpp); not part of the API.

#include <cstdint>
#include <string>
#include <vector>

namespace replay::detail {

inline std::string u(std::uint64_t v) { return std::to_string(v); }

inline std::string array(const std::vector<std::uint64_t>& v) {
  std::string s = "[";
  for (std::size_t i = 0; i < v.size(); ++i) s += (i == 0 ? "" : ", ") + u(v[i]);
  return s + "]";
}

inline std::int64_t messages_per_second(std::uint64_t lines, std::int64_t wall_ns) {
  if (wall_ns <= 0) return 0;
  constexpr std::uint64_t kNs = 1'000'000'000;
  const auto wall = static_cast<std::uint64_t>(wall_ns);
  if (lines <= std::uint64_t{9'000'000'000}) return static_cast<std::int64_t>(lines * kNs / wall);
  return static_cast<std::int64_t>(lines / (wall / kNs == 0 ? 1 : wall / kNs));
}

}  // namespace replay::detail
