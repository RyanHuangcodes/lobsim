#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "replay/replayer.hpp"
#include "report_util.hpp"

// The text reports for stdout (summary.json and divergences.csv are in replayer_json.cpp).
namespace replay {
namespace {

using detail::array;
using detail::messages_per_second;
using detail::u;

std::string level_text(const std::optional<lob::LevelView>& l) {
  return l ? std::to_string(l->price) + " x " + std::to_string(l->total_qty) : "-";
}

std::string level_text(const BookLevel& l) {
  return l.empty() ? "-" : std::to_string(l.price) + " x " + std::to_string(l.size);
}

void pad(std::string& s, std::size_t width) {
  if (s.size() < width) s.append(width - s.size(), ' ');
}

// "a/b (p%)", or "a/b (n/a)" when b is 0.
std::string rate(std::uint64_t a, std::uint64_t b) {
  return std::to_string(a) + "/" + std::to_string(b) + " (" + percent(a, b) + (b == 0 ? ")" : "%)");
}

// Appends `text` as a column of `width` characters, followed by at least one space.
void column(std::string& row, const std::string& text, std::size_t width) {
  row += text;
  row += ' ';
  pad(row, row.size() - text.size() - 1 + width);
}

}  // namespace

std::string text_summary(const Summary& s, std::optional<std::int64_t> wall_ns) {
  const Counters& c = s.counters;
  std::string t;
  t += "mode " + std::string(to_string(s.mode)) + ", L = " + std::to_string(s.levels) + ", compare depth " +
       std::to_string(s.compare_depth) + ", trust depth " + std::to_string(s.trust_depth) + "\n";
  t += "lines replayed       " + u(s.lines) + " (rows compared " + u(s.rows_compared) + ", not exact at depth L " +
       u(s.divergent_rows) + ")\n";
  t += "exact rows before any correction (exact / rows):\n";
  for (std::size_t d = 0; d < s.exact_rows.size(); ++d) {
    std::string cell = "  depth " + u(d + 1) + ": " + rate(s.exact_rows[d], s.rows_compared);
    if (d % 2 == 0 && d + 1 < s.exact_rows.size()) pad(cell, 40);
    t += cell + (d % 2 == 1 || d + 1 == s.exact_rows.size() ? "\n" : "");
  }
  t += "applied              submitted " + u(c.submitted) + ", cancelled " + u(c.cancelled) + ", deleted " +
       u(c.deleted) + ", executed " + u(c.executed) + "\n";
  t += "seed                 consumed_by_seed " + u(c.consumed_by_seed) + ", seed orders " + u(c.seed_orders) +
       " (rejected " + u(c.seed_rejected) + "), seed_attributed " + u(c.seed_attributed) + ", unknown_id " +
       u(c.unknown_id) + "\n";
  t += "skipped              parse_rejected " + u(c.parse_rejected) + ", dup_id " + u(c.dup_id) +
       ", submit_would_cross " + u(c.submit_would_cross) + ", submit_bad_price " + u(c.submit_bad_price) +
       ", direction_mismatch " + u(c.direction_mismatch) + ", price_mismatch " + u(c.price_mismatch) + "\n";
  t += "                     hidden_exec " + u(c.hidden_exec) + ", cross_trade " + u(c.cross_trade) + ", halt " +
       u(c.halt) + "\n";
  t += "flags                delete_size_mismatch " + u(c.delete_size_mismatch) + ", oversized_reduce " +
       u(c.oversized_reduce) + ", time_truncated " + u(c.time_truncated) + ", oracle_malformed " +
       u(c.oracle_malformed) + ", oracle_rejected " + u(c.oracle_rejected) + ", crossed_state " +
       u(c.crossed_state) + "\n";
  t += "resync               rows corrected " + u(s.resync.rows) + ", failed " + u(s.resync.failed) +
       ", malformed rows skipped " + u(s.resync.skipped_malformed) + "\n";
  t += "  corrected by level " + array(s.resync.corrected) + "\n";
  t += "  removed by rank    " + array(s.resync.removed) + " and " + u(s.resync.removed_beyond_l) + " beyond L\n";
  t += "  How to read them: a correction at level 1 suggests an engine or replayer bug, because the touch is\n"
       "  always inside the published window; corrections concentrated at the deepest level are the expected\n"
       "  effect of LOBSTER's price-range filtering.\n";
  const MatchingStats& m = s.matching;
  t += "matching             " + u(m.bursts) + " bursts (" + u(m.burst_messages) +
       " executions), each checked with preview_match before it is applied\n";
  std::string header = "  ";
  column(header, "T", 3);
  column(header, "agree/scored", 21);
  column(header, "exact", 6);
  column(header, "via SYN", 8);
  column(header, "queue", 6);
  column(header, "price (better/worse)", 21);
  column(header, "size", 5);
  column(header, "length", 7);
  column(header, "excl. unknown", 14);
  t += header + "excl. untrusted\n";
  for (const MatchCounts& c : m.by_trust_depth) {
    std::string row = "  ";
    column(row, std::to_string(c.trust_depth), 3);
    column(row, rate(c.agree(), c.scored()), 21);
    column(row, u(c.exact), 6);
    column(row, u(c.exact_via_synthetic), 8);
    column(row, u(c.queue_order), 6);
    column(row, u(c.price_skip) + " (" + u(c.price_skip_better) + "/" + u(c.price_skip_worse) + ")", 21);
    column(row, u(c.size_split), 5);
    column(row, u(c.length), 7);
    column(row, u(c.excluded_unknown), 14);
    row += u(c.excluded_untrusted);
    if (c.trust_depth == m.headline_trust_depth) row += "  <- headline";
    t += row + "\n";
  }
  t += "  agreement = (exact + via SYN) / (bursts - excluded); scored only if the previous row was exact\n"
       "  before correction to depth T and T exceeds the burst's distinct prices\n";
  if (s.strict_failure) t += "strict               FAILED at line " + u(s.strict_failure->line) + "\n";
  if (wall_ns) {
    t += "wall time            " + std::to_string(*wall_ns / 1'000'000) + " ms, " +
         std::to_string(messages_per_second(s.lines, *wall_ns)) + " messages/s\n";
  }
  return t;
}

std::string strict_failure_report(const Summary& s) {
  if (!s.strict_failure) return {};
  const StrictFailure& f = *s.strict_failure;
  std::string t = "strict mode: line " + u(f.line) + " (orderbook row " + u(f.line) + ", after message " + u(f.line) + ")";
  if (f.oracle_unparsable) {
    t += " has an orderbook row that does not parse: " + f.oracle_error + "\n";
  } else {
    t += " is exact only down to depth " + std::to_string(f.exact_depth) + ", --compare-depth is " +
         std::to_string(s.compare_depth) + "\n";
  }
  t += "level  book ask             book bid             oracle ask           oracle bid\n";
  for (std::size_t i = 0; i < static_cast<std::size_t>(s.levels); ++i) {
    std::string row = std::to_string(i + 1);
    pad(row, 7);
    std::string cell = level_text(i < f.ours_asks.size() ? std::optional<lob::LevelView>(f.ours_asks[i]) : std::nullopt);
    pad(cell, 21);
    row += cell;
    cell = level_text(i < f.ours_bids.size() ? std::optional<lob::LevelView>(f.ours_bids[i]) : std::nullopt);
    pad(cell, 21);
    row += cell;
    if (f.oracle) {
      cell = level_text(f.oracle->asks[i]);
      pad(cell, 21);
      row += cell + level_text(f.oracle->bids[i]);
    }
    t += row + "\n";
  }
  t += "last messages:\n";
  for (const std::string& m : f.recent) t += "  " + m + "\n";
  return t;
}

}  // namespace replay
