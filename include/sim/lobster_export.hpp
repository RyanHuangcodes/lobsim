#pragma once
// The exporter that turns a simulator order log into LOBSTER message and orderbook files, as a
// library: `lobsim_export_lobster` is a thin command line over it, and `lobsim_calibrate validate`
// calls it in-process. What it writes is described at the top of apps/export_lobster_main.cpp.
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "sim/messages.hpp"
#include "sim/order_log.hpp"
#include "sim/rng.hpp"

namespace sim {

inline constexpr std::int64_t kLobsterOpenNs = 34'200'000'000'000;  // 9:30:00 in nanoseconds after midnight
inline constexpr std::int64_t kLobsterNsPerSecond = 1'000'000'000;
// Ids of hidden executions: 2^40 + 0, 1, 2, ... The simulator's own order ids are small counters,
// so the export stops with an error if one ever reaches this range.
inline constexpr std::int64_t kHiddenIdBase = std::int64_t{1} << 40;

// Everything --lobster-like adds (spec D3). All times are simulator nanoseconds.
struct LikeOptions {
  bool on{false};
  std::int64_t window_start_ns{300 * kLobsterNsPerSecond};
  double hidden_rate{0.2};
  std::optional<std::int64_t> halt_at_ns;  // default: halfway through the window
  bool halt{true};                          // false: --no-halt
  std::uint64_t seed{20120621};
};

class LobsterExporter {
 public:
  LobsterExporter(lob::Price max_price, int levels, bool filter, std::ostream& messages, std::ostream& orderbook,
                  const LikeOptions& like = {}, std::int64_t window_end_ns = 0, std::ostream* audit = nullptr,
                  std::int64_t price_scale = 1);

  // Emits the injected messages (hidden executions, the halt) due before the end of the run.
  void finish();
  void request(const LoggedRequest& r);

  std::uint64_t requests() const { return requests_; }
  std::uint64_t written() const { return written_; }
  std::uint64_t dropped() const { return dropped_; }
  const std::uint64_t* by_type() const { return by_type_; }
  std::uint64_t hidden_written() const { return hidden_written_; }
  std::uint64_t hidden_skipped() const { return hidden_skipped_; }
  std::uint64_t halts_written() const { return halts_written_; }
  std::uint64_t before_window() const { return before_window_; }
  std::int64_t halt_ns() const { return halt_ns_; }

 private:
  void schedule_hidden(std::int64_t from_ns);
  void inject_until(std::int64_t limit_ns);
  void write_injected(std::int64_t at_ns, int type, std::int64_t id, std::int64_t size, std::int64_t price,
                      int direction);
  void write_audit(int type, std::int64_t id, std::int64_t size, std::int64_t price, int direction,
                   const char* status);
  void book_row(std::string& out) const;  // the orderbook row of the mirror's top L, written into `out`
  [[noreturn]] void fail(const std::string& what) const;
  std::optional<lob::OrderView> mirror_order(lob::OrderId engine_id) const;
  lob::OrderView resting(lob::OrderId engine_id, const char* what) const;
  bool in_window(lob::Side side, lob::Price price) const;
  void emit(int type, lob::OrderId id, lob::Qty size, lob::Price price, lob::Side side);
  void apply_to_mirror(int type, lob::OrderId id, lob::Qty size, lob::Price price, lob::Side side);

  lob::OrderBook engine_;
  lob::OrderBook mirror_;
  std::unordered_map<lob::OrderId, lob::OrderId> mirror_id_;  // engine id -> mirror id, resting orders only
  std::size_t levels_;
  bool filter_;
  std::ostream& messages_;
  std::ostream& orderbook_;
  lob::EventBuffer events_;
  TimeNs time_{0};
  std::uint64_t requests_{0};
  std::uint64_t written_{0};
  std::uint64_t dropped_{0};
  std::uint64_t by_type_[8]{};
  LikeOptions like_;
  Xoshiro256StarStar rng_;
  std::string last_row_;
  // Scratch buffers reused for every request and message, so the per-request comparison of the two
  // books and the row building make no allocation once they have grown (D-907).
  mutable std::vector<lob::LevelView> scratch_a_, scratch_b_;
  std::int64_t halt_ns_{0};
  std::int64_t window_end_ns_{0};
  std::int64_t next_hidden_ns_{0};
  bool halt_done_{false};
  std::uint64_t hidden_written_{0};
  std::uint64_t hidden_skipped_{0};
  std::uint64_t halts_written_{0};
  std::uint64_t before_window_{0};
  std::ostream* audit_;
  std::int64_t price_scale_;
  std::uint64_t audit_seq_{0};
};

struct ExportSpec {
  int levels{0};
  bool filter{false};  // --filter-window (--lobster-like switches it on unless the caller says otherwise)
  LikeOptions like;
  std::int64_t price_scale{1};
};

// What one export wrote.
struct ExportResult {
  std::uint64_t requests{0}, written{0}, dropped{0};
  std::uint64_t by_type[8]{};
  std::uint64_t hidden_written{0}, hidden_skipped{0}, halts_written{0}, before_window{0};
  std::int64_t halt_ns{0};
  std::int64_t window_end_ns{0};  // the time of the last request (--lobster-like only)
};

// Reads a whole orders.log from `log` (a seekable stream: --lobster-like needs a first pass to find the
// window's end) and writes the two LOBSTER files. Throws std::runtime_error (or ReplayError) with the
// exporter's messages. The streams are not flushed or checked; `audit` may be null.
ExportResult export_order_log(std::istream& log, const ExportSpec& spec, std::ostream& messages,
                              std::ostream& orderbook, std::ostream* audit = nullptr);

}  // namespace sim
