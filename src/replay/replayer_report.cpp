#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "replay/replayer.hpp"

// Reports of a replay (the replay itself is in replayer.cpp). Integers only: rates are written as
// numerator / denominator pairs plus a percentage formatted from integers.
namespace replay {
namespace {

using Field = std::pair<const char*, std::uint64_t>;

// The counters in report order; summary.json and the text summary both read this list.
std::vector<Field> counter_fields(const Counters& c) {
  return {
      {"messages", c.messages},
      {"type_1", c.by_type[1]},
      {"type_2", c.by_type[2]},
      {"type_3", c.by_type[3]},
      {"type_4", c.by_type[4]},
      {"type_5", c.by_type[5]},
      {"type_6", c.by_type[6]},
      {"type_7", c.by_type[7]},
      {"parse_rejected", c.parse_rejected},
      {"time_truncated", c.time_truncated},
      {"consumed_by_seed", c.consumed_by_seed},
      {"seed_orders", c.seed_orders},
      {"seed_rejected", c.seed_rejected},
      {"submitted", c.submitted},
      {"dup_id", c.dup_id},
      {"submit_would_cross", c.submit_would_cross},
      {"submit_bad_price", c.submit_bad_price},
      {"cancelled", c.cancelled},
      {"deleted", c.deleted},
      {"executed", c.executed},
      {"direction_mismatch", c.direction_mismatch},
      {"price_mismatch", c.price_mismatch},
      {"delete_size_mismatch", c.delete_size_mismatch},
      {"oversized_reduce", c.oversized_reduce},
      {"seed_attributed", c.seed_attributed},
      {"unknown_id", c.unknown_id},
      {"hidden_exec", c.hidden_exec},
      {"cross_trade", c.cross_trade},
      {"halt", c.halt},
      {"oracle_malformed", c.oracle_malformed},
      {"oracle_rejected", c.oracle_rejected},
      {"crossed_state", c.crossed_state},
  };
}

std::string u(std::uint64_t v) { return std::to_string(v); }

std::string array(const std::vector<std::uint64_t>& v) {
  std::string s = "[";
  for (std::size_t i = 0; i < v.size(); ++i) s += (i == 0 ? "" : ", ") + u(v[i]);
  return s + "]";
}

std::string level_text(const std::optional<lob::LevelView>& l) {
  return l ? std::to_string(l->price) + " x " + std::to_string(l->total_qty) : "-";
}

std::string level_text(const BookLevel& l) {
  return l.empty() ? "-" : std::to_string(l.price) + " x " + std::to_string(l.size);
}

std::int64_t messages_per_second(std::uint64_t lines, std::int64_t wall_ns) {
  if (wall_ns <= 0) return 0;
  constexpr std::uint64_t kNs = 1'000'000'000;
  const auto wall = static_cast<std::uint64_t>(wall_ns);
  if (lines <= std::uint64_t{9'000'000'000}) return static_cast<std::int64_t>(lines * kNs / wall);
  return static_cast<std::int64_t>(lines / (wall / kNs == 0 ? 1 : wall / kNs));
}

void pad(std::string& s, std::size_t width) {
  if (s.size() < width) s.append(width - s.size(), ' ');
}

}  // namespace

std::string percent(std::uint64_t a, std::uint64_t b) {
  if (b == 0) return "n/a";
  // a <= b in every use; a * 10000 fits for any file of fewer than 1.8e15 lines.
  const std::uint64_t hundredths = a * 10000 / b;
  std::string frac = std::to_string(hundredths % 100);
  if (frac.size() < 2) frac.insert(0, 1, '0');
  return std::to_string(hundredths / 100) + "." + frac;
}

std::string summary_json(const Summary& s, std::optional<std::int64_t> wall_ns) {
  std::string j = "{\n";
  j += "  \"mode\": \"" + std::string(to_string(s.mode)) + "\",\n";
  j += "  \"levels\": " + std::to_string(s.levels) + ",\n";
  j += "  \"compare_depth\": " + std::to_string(s.compare_depth) + ",\n";
  j += "  \"trust_depth\": " + std::to_string(s.trust_depth) + ",\n";
  j += "  \"completed\": " + std::string(s.strict_failure ? "false" : "true") + ",\n";
  j += "  \"strict_failure_line\": " + (s.strict_failure ? u(s.strict_failure->line) : std::string("null")) + ",\n";
  j += "  \"lines\": " + u(s.lines) + ",\n";
  j += "  \"rows_compared\": " + u(s.rows_compared) + ",\n";
  j += "  \"counters\": {\n";
  const std::vector<Field> fields = counter_fields(s.counters);
  for (std::size_t i = 0; i < fields.size(); ++i) {
    j += "    \"" + std::string(fields[i].first) + "\": " + u(fields[i].second) + (i + 1 < fields.size() ? ",\n" : "\n");
  }
  j += "  },\n";
  j += "  \"exact_rows\": [\n";
  for (std::size_t d = 0; d < s.exact_rows.size(); ++d) {
    j += "    {\"depth\": " + u(d + 1) + ", \"exact\": " + u(s.exact_rows[d]) + ", \"rows\": " + u(s.rows_compared) +
         ", \"percent\": \"" + percent(s.exact_rows[d], s.rows_compared) + "\"}" +
         (d + 1 < s.exact_rows.size() ? ",\n" : "\n");
  }
  j += "  ],\n";
  j += "  \"divergent_rows\": " + u(s.divergent_rows) + ",\n";
  j += "  \"resync\": {\n";
  j += "    \"rows\": " + u(s.resync.rows) + ",\n";
  j += "    \"corrected_by_level\": " + array(s.resync.corrected) + ",\n";
  j += "    \"removed_by_rank\": " + array(s.resync.removed) + ",\n";
  j += "    \"removed_beyond_level_l\": " + u(s.resync.removed_beyond_l) + ",\n";
  j += "    \"failed\": " + u(s.resync.failed) + ",\n";
  j += "    \"skipped_malformed\": " + u(s.resync.skipped_malformed) + "\n";
  j += "  }";
  if (wall_ns) {
    j += ",\n  \"wall_ns\": " + std::to_string(*wall_ns) + ",\n";
    j += "  \"messages_per_second\": " + std::to_string(messages_per_second(s.lines, *wall_ns));
  }
  j += "\n}\n";
  return j;
}

std::string divergences_csv(const Summary& s) {
  std::string out = "line,depth,side,ours_price,ours_size,oracle_price,oracle_size\n";
  for (const Divergence& d : s.divergences) {
    out += u(d.line) + ',' + std::to_string(d.depth) + ',' + (d.side == 'A' ? "ask" : "bid") + ',';
    out += d.ours ? std::to_string(d.ours->price) + ',' + std::to_string(d.ours->total_qty) : std::string(",0");
    out += ',' + std::to_string(d.oracle.price) + ',' + std::to_string(d.oracle.size) + '\n';
  }
  return out;
}

std::string text_summary(const Summary& s, std::optional<std::int64_t> wall_ns) {
  const Counters& c = s.counters;
  std::string t;
  t += "mode " + std::string(to_string(s.mode)) + ", L = " + std::to_string(s.levels) + ", compare depth " +
       std::to_string(s.compare_depth) + ", trust depth " + std::to_string(s.trust_depth) + "\n";
  t += "lines replayed       " + u(s.lines) + " (rows compared " + u(s.rows_compared) + ", not exact at depth L " +
       u(s.divergent_rows) + ")\n";
  t += "exact rows before any correction (exact / rows):\n";
  for (std::size_t d = 0; d < s.exact_rows.size(); ++d) {
    std::string cell = "  depth " + u(d + 1) + ": " + u(s.exact_rows[d]) + "/" + u(s.rows_compared) + " (" +
                       percent(s.exact_rows[d], s.rows_compared) + "%)";
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
