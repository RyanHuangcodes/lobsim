#pragma once

#include <cstdint>
#include <variant>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace sim {

using TimeNs = std::int64_t;        // simulation time: nanoseconds since the start of the run
using AgentIndex = std::uint32_t;   // position in the simulator's agent list; OwnerId = index + 1

// Top of book. A side with qty 0 (and price 0) is empty.
struct BBO {
  lob::Price bid{0};
  lob::Qty bid_qty{0};
  lob::Price ask{0};
  lob::Qty ask_qty{0};
  bool has_bid() const noexcept { return bid_qty > 0; }
  bool has_ask() const noexcept { return ask_qty > 0; }
  bool two_sided() const noexcept { return has_bid() && has_ask(); }
  bool operator==(const BBO&) const = default;
};

struct TradePrint {
  lob::Price price{0};
  lob::Qty qty{0};
  lob::Side aggressor_side{lob::Side::Buy};
  bool operator==(const TradePrint&) const = default;
};

// Public market data. Private messages are lob::Event values and requests are
// lob::Request values: an agent and the exchange exchange nothing else.
using MarketData = std::variant<BBO, TradePrint>;

// Config limits: a run lasts at most 1e9 s = 1e18 ns, and every configured duration
// (latency, jitter, period) is at most 1e18 ns.
inline constexpr double kMaxDurationS = 1.0e9;
inline constexpr TimeNs kMaxConfigNs = 1'000'000'000'000'000'000LL;
// Longest duration a run-time draw can give: "never, within any valid run". With now <=
// 1e18 ns (the longest run), now + kFarFutureNs <= 5e18 < 2^63 - 1, so adding a drawn
// duration to the current time cannot overflow.
inline constexpr TimeNs kFarFutureNs = 4'000'000'000'000'000'000LL;

// Converts a duration drawn at run time (seconds, as a double) to simulation time:
// truncated, at least 1 ns so that time always advances, and at most kFarFutureNs (a
// draw from a tiny rate or a huge mean would otherwise overflow the cast). NaN gives
// kFarFutureNs.
inline TimeNs seconds_to_ns(double seconds) noexcept {
  if (seconds < 1e-9) return 1;  // also every negative value
  if (!(seconds < 4.0e9)) return kFarFutureNs;
  const auto ns = static_cast<TimeNs>(seconds * 1e9);
  return ns < 1 ? 1 : ns;
}

}  // namespace sim
