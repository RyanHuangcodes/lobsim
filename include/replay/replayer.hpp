#pragma once

// Replays a LOBSTER message file into a lob::OrderBook and compares the book with the orderbook
// file (the oracle) after every message. Both files are read line by line; row k of the
// orderbook file is the book right after message k. Prices stay in LOBSTER units (dollars x
// 10000) and times in integer nanoseconds: nothing here uses a fractional number type.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include "replay/lobster.hpp"

namespace replay {

// ITCH's largest price, $200,000.0000 at 4 implied decimals: the replay book's max_price.
inline constexpr lob::Price kReplayMaxPrice = 2'000'000'000;

// strict: stop at the first row that is not exact down to compare_depth.
// report:  never correct the book, only count.
// resync:  after counting, correct the book to the oracle row (inside its window only).
enum class Mode : std::uint8_t { Strict, Report, Resync };
const char* to_string(Mode m) noexcept;
std::optional<Mode> parse_mode(std::string_view s);

struct ReplayOptions {
  Mode mode{Mode::Resync};
  int compare_depth{1};  // strict mode: rows must be exact down to this depth (1..L)
  int trust_depth{3};    // the matching check's headline: a burst is scored only if the previous row was exact
                         // to this depth (before any correction) and the depth exceeds the burst's prices
  std::size_t max_lookahead{100'000};  // messages buffered ahead of the current one (bursts); more is an error
};

// Input that cannot be replayed at all: a missing or unreadable file, a level count that the
// file name and the columns disagree on, files of different lengths, an unparsable first row.
class InputError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Every message is counted in exactly one of the "what happened" counters; the flags
// (time_truncated, oracle_malformed, crossed_state, ...) count lines or rows with a property.
struct Counters {
  std::uint64_t messages{0};            // lines of the message file
  std::uint64_t by_type[8]{};           // parsed messages by type, [1..7]
  std::uint64_t parse_rejected{0};      // message lines that did not parse: skipped
  std::uint64_t time_truncated{0};      // times with more than 9 fractional digits
  std::uint64_t consumed_by_seed{0};    // message 1: its effect is inside orderbook row 1
  std::uint64_t seed_orders{0};         // synthetic orders placed from row 1
  std::uint64_t seed_rejected{0};       // row-1 levels that could not be placed (a crossed row)
  std::uint64_t submitted{0};           // type 1 placed
  std::uint64_t dup_id{0};              // type 1 for an id that is resting: skipped
  std::uint64_t submit_would_cross{0};  // type 1 that would cross the book: skipped
  std::uint64_t submit_bad_price{0};    // type 1 above the replay book's max_price: skipped
  std::uint64_t cancelled{0};           // type 2 applied to a known order
  std::uint64_t deleted{0};             // type 3 applied to a known order
  std::uint64_t executed{0};            // type 4 applied to a known order
  std::uint64_t direction_mismatch{0};  // types 2-4, known id, Direction is not the order's side: skipped
  std::uint64_t price_mismatch{0};      // types 2-4, known id, price is not the order's price: skipped
  std::uint64_t delete_size_mismatch{0};  // type 3 whose size is not the order's remaining (still deleted)
  std::uint64_t oversized_reduce{0};    // type 2 or 4 larger than the remaining (the order is removed)
  std::uint64_t seed_attributed{0};     // types 2-4, unknown id, taken from a synthetic order
  std::uint64_t unknown_id{0};          // types 2-4, unknown id, no synthetic order fits: skipped
  std::uint64_t hidden_exec{0};         // type 5: skipped
  std::uint64_t cross_trade{0};         // type 6: skipped
  std::uint64_t halt{0};                // type 7: skipped
  std::uint64_t oracle_malformed{0};    // orderbook rows with levels out of order
  std::uint64_t oracle_rejected{0};     // orderbook rows that did not parse: not compared
  std::uint64_t crossed_state{0};       // rows after which the book had best bid >= best ask
};

// The first difference of a non-exact row: `depth` is 1-based; `side` is 'A' or 'B'.
struct Divergence {
  std::uint64_t line{0};  // 1-based line number, the same in both files
  int depth{0};
  char side{'A'};
  std::optional<lob::LevelView> ours;  // nullopt: the book has no level there
  BookLevel oracle{};                  // size 0: the oracle has no level there
  bool operator==(const Divergence&) const = default;
};

struct StrictFailure {
  std::uint64_t line{0};
  int exact_depth{0};                   // levels that matched (< compare_depth)
  bool oracle_unparsable{false};
  std::string oracle_error;
  std::vector<lob::LevelView> ours_asks, ours_bids;  // top L of the book
  std::optional<BookRow> oracle;
  std::vector<std::string> recent;      // the last messages up to this one, "line N: <text>"
};

struct ResyncStats {
  std::uint64_t rows{0};                      // rows on which anything was corrected
  std::vector<std::uint64_t> removed;         // [rank - 1], rank 1..L in the book before the correction
  std::uint64_t removed_beyond_l{0};          // removed levels of rank > L
  std::vector<std::uint64_t> corrected;       // [d - 1], d = oracle level 1..L
  std::uint64_t failed{0};                    // a synthetic insert that the book refused (a crossed row)
  std::uint64_t skipped_malformed{0};         // malformed rows: not used to correct
};

inline constexpr std::size_t kMaxDivergences = 1000;

// The matching-agreement check. A burst is a maximal run of consecutive type-4 messages with the
// same time and the same Direction (any other line ends it), read as one aggressive order. Before
// the burst is applied, preview_match predicts which resting orders that order would hit, and the
// prediction is compared with the burst, fill by fill. Each burst gets exactly one class.
enum class BurstClass : std::uint8_t {
  Exact,              // the same fills: id, price and size
  ExactViaSynthetic,  // the same when synthetic orders (SYN@price) are compared by price and size only
  QueueOrder,         // first difference: same price, a different order
  PriceSkip,          // first difference: a different price
  SizeSplit,          // first difference: same order and price, a different size
  Length,             // every compared fill equal, a different number of fills
  ExcludedUnknown,    // a message names an id that is neither resting nor attributable to a synthetic order
  ExcludedUntrusted,  // the previous row was not exact to the trust depth, or the depth is too shallow
};
const char* to_string(BurstClass c) noexcept;

// One fill of a burst (data) or of the prediction: an external id, or a synthetic order.
struct BurstFill {
  bool synthetic{false};
  std::int64_t external_id{0};  // unused when synthetic
  std::int64_t price{0};
  std::int64_t size{0};
  bool operator==(const BurstFill&) const = default;
};
// "<external id> <size> @ <price>" or "SYN@<price> <size> @ <price>".
std::string to_string(const BurstFill& f);

struct MatchCounts {
  int trust_depth{0};
  std::uint64_t exact{0};
  std::uint64_t exact_via_synthetic{0};
  std::uint64_t queue_order{0};
  std::uint64_t price_skip{0};
  std::uint64_t price_skip_better{0};  // the predicted fill is at a better price for the taker than the data's
  std::uint64_t price_skip_worse{0};
  std::uint64_t size_split{0};
  std::uint64_t length{0};
  std::uint64_t excluded_unknown{0};
  std::uint64_t excluded_untrusted{0};
  std::uint64_t agree() const { return exact + exact_via_synthetic; }
  std::uint64_t scored() const {
    return exact + exact_via_synthetic + queue_order + price_skip + size_split + length;
  }
};

// A mismatched burst kept for the report (the first kMaxBurstExamples of each mismatch class at
// the headline trust depth). Lines are "line N: <message line>".
struct BurstExample {
  std::uint64_t line{0};              // the burst's first line
  std::size_t first_difference{0};    // 1-based index of the first differing fill
  std::optional<bool> predicted_better;  // price_skip only: the prediction was at a better price
  std::vector<std::string> messages;  // the burst
  std::vector<std::string> data;      // the burst as fills
  std::vector<std::string> predicted; // preview_match's fills
  std::vector<std::string> before;    // up to 5 lines before the burst
  std::vector<std::string> after;     // up to 5 lines after it
};

inline constexpr std::size_t kMaxBurstExamples = 3;

struct MatchingStats {
  std::uint64_t bursts{0};
  std::uint64_t burst_messages{0};
  int headline_trust_depth{3};
  std::vector<MatchCounts> by_trust_depth;  // trust depths 1, 3, L and the headline, ascending, distinct
  std::vector<BurstExample> queue_order_examples;
  std::vector<BurstExample> price_skip_examples;
  std::vector<BurstExample> size_split_examples;
  std::vector<BurstExample> length_examples;
  const MatchCounts& headline() const;
};

struct Summary {
  Mode mode{Mode::Resync};
  int levels{0};
  int compare_depth{1};
  int trust_depth{3};
  std::uint64_t lines{0};          // rows replayed
  std::uint64_t rows_compared{0};  // rows with a parsed oracle row
  std::vector<std::uint64_t> exact_rows;  // [d - 1]: rows exact down to depth d, before any correction
  std::uint64_t divergent_rows{0};        // rows not exact at depth L
  std::vector<Divergence> divergences;    // the first kMaxDivergences non-exact rows (one entry per differing side)
  Counters counters;
  ResyncStats resync;
  MatchingStats matching;
  std::optional<StrictFailure> strict_failure;
};

// One message line and its orderbook line, parsed.
struct Entry {
  std::uint64_t line{0};
  std::string text;  // the message line as read
  Parsed<LobsterMsg> msg;
  std::string book_text;
  Parsed<BookRow> book;
};

class Replayer {
 public:
  // levels: L of the orderbook file. Throws std::invalid_argument for an option out of range.
  Replayer(int levels, ReplayOptions options);
  Replayer(const Replayer&) = delete;
  Replayer& operator=(const Replayer&) = delete;

  // Replays both streams to the end, or to the first strict failure. Throws InputError.
  const Summary& run(std::istream& messages, std::istream& orderbook);

  const Summary& summary() const { return summary_; }
  const lob::OrderBook& book() const { return book_; }

 private:
  // Line-by-line reader of both files with a bounded lookahead buffer.
  class Source {
   public:
    Source(std::istream& messages, std::istream& orderbook, int levels, std::size_t max_lookahead);
    // The i-th entry ahead (0 = the next one), reading as needed; nullptr at the end of input.
    const Entry* peek(std::size_t i);
    std::optional<Entry> next();

   private:
    bool read_one();
    std::istream& messages_;
    std::istream& orderbook_;
    int levels_;
    std::size_t max_lookahead_;
    std::uint64_t line_{0};
    bool ended_{false};
    std::deque<Entry> ahead_;
  };

  void seed(const BookRow& row);
  void apply(const LobsterMsg& m);
  void apply_known(const LobsterMsg& m, lob::OrderId id);
  void attribute(const LobsterMsg& m);
  void compare(const Entry& e);
  void resync(const BookRow& row);
  void remove_level(lob::Side side, lob::Price price);
  void trim_level(lob::Side side, lob::Price price, lob::Qty excess);
  void forget_if_gone(lob::OrderId id);
  void remember(const Entry& e);
  void fail_strict(const Entry& e, int exact_depth);
  // The matching check of the burst that starts at `first` (already taken from the source); the
  // rest of the burst is read through the source's lookahead. Changes nothing in the book.
  void check_burst(const Entry& first, Source& source);

  int levels_;
  ReplayOptions options_;
  lob::OrderBook book_;
  std::unordered_map<std::int64_t, lob::OrderId> by_external_;  // resting orders only
  std::unordered_map<lob::OrderId, std::int64_t> external_of_;  // the reverse, for reports
  std::unordered_set<lob::OrderId> synthetic_;                   // seed and resync orders, resting
  std::deque<std::string> recent_;                               // the last kRecent message lines
  // Depth to which the previous row was exact before any correction (0 if it was not compared):
  // the matching check's trust rule reads it.
  int previous_exact_depth_{0};
  std::size_t burst_left_{0};  // messages of the current burst still to come after this one
  Summary summary_;
};

// Opens both files, takes L from the orderbook file name (and checks the message file name if it
// has one) and replays. Throws InputError, or std::invalid_argument for an option out of range.
Summary replay_files(const std::string& messages_path, const std::string& orderbook_path,
                     const ReplayOptions& options);

// summary.json. wall_ns: the measured run time, left out when nullopt (the deterministic part).
std::string summary_json(const Summary& s, std::optional<std::int64_t> wall_ns);
// divergences.csv: line,depth,side,ours_price,ours_size,oracle_price,oracle_size.
std::string divergences_csv(const Summary& s);
// The one-screen report for stdout, including how to read the corrections histogram.
std::string text_summary(const Summary& s, std::optional<std::int64_t> wall_ns);
// The book and the oracle side by side and the last messages, for a strict failure.
std::string strict_failure_report(const Summary& s);

// a / b as a percentage with two decimals, by integer arithmetic ("n/a" when b == 0).
std::string percent(std::uint64_t a, std::uint64_t b);

}  // namespace replay
