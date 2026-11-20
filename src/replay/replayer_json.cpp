#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "replay/replayer.hpp"
#include "report_util.hpp"

// summary.json and divergences.csv (the text reports are in replayer_text.cpp). Integers only:
// rates are written as numerator / denominator pairs plus a percentage formatted from integers.
// Split from the text reports for the reason of D-158.
namespace replay {
namespace {

using detail::array;
using detail::messages_per_second;
using detail::u;

using Field = std::pair<const char*, std::uint64_t>;

// The counters of summary.json, in order.
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

// A JSON string literal. Example lines come from the input files, so anything may be in them.
std::string quote(const std::string& text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string q = "\"";
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      q += '\\';
      q += ch;
    } else if (c < 0x20) {
      q += "\\u00";
      q += kHex[c >> 4];
      q += kHex[c & 0xf];
    } else {
      q += ch;
    }
  }
  return q + "\"";
}

std::string strings(const std::vector<std::string>& v) {
  std::string s = "[";
  for (std::size_t i = 0; i < v.size(); ++i) s += (i == 0 ? "" : ", ") + quote(v[i]);
  return s + "]";
}

std::string examples_json(const std::vector<BurstExample>& v) {
  std::string j = "[";
  for (std::size_t i = 0; i < v.size(); ++i) {
    const BurstExample& x = v[i];
    j += i == 0 ? "\n" : ",\n";
    j += "        {\"line\": " + std::to_string(x.line) + ", \"first_difference\": " + std::to_string(x.first_difference);
    if (x.predicted_better) j += std::string(", \"price_skip\": \"") + (*x.predicted_better ? "better" : "worse") + "\"";
    j += ",\n         \"messages\": " + strings(x.messages);
    j += ",\n         \"data\": " + strings(x.data);
    j += ",\n         \"predicted\": " + strings(x.predicted);
    j += ",\n         \"before\": " + strings(x.before);
    j += ",\n         \"after\": " + strings(x.after) + "}";
  }
  return j + (v.empty() ? "]" : "\n      ]");
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
  j += "  },\n";
  const MatchingStats& m = s.matching;
  j += "  \"matching\": {\n";
  j += "    \"bursts\": " + u(m.bursts) + ",\n";
  j += "    \"burst_messages\": " + u(m.burst_messages) + ",\n";
  j += "    \"headline_trust_depth\": " + std::to_string(m.headline_trust_depth) + ",\n";
  j += "    \"by_trust_depth\": [\n";
  for (std::size_t i = 0; i < m.by_trust_depth.size(); ++i) {
    const MatchCounts& c = m.by_trust_depth[i];
    j += "      {\"trust_depth\": " + std::to_string(c.trust_depth) + ", \"exact\": " + u(c.exact) +
         ", \"exact_via_synthetic\": " + u(c.exact_via_synthetic) + ", \"queue_order\": " + u(c.queue_order) +
         ", \"price_skip\": " + u(c.price_skip) + ", \"price_skip_better\": " + u(c.price_skip_better) +
         ", \"price_skip_worse\": " + u(c.price_skip_worse) + ", \"size_split\": " + u(c.size_split) +
         ", \"length\": " + u(c.length) + ", \"excluded_unknown\": " + u(c.excluded_unknown) +
         ", \"excluded_untrusted\": " + u(c.excluded_untrusted) + ", \"numerator\": " + u(c.agree()) +
         ", \"denominator\": " + u(c.scored()) + ", \"percent\": \"" + percent(c.agree(), c.scored()) + "\"}" +
         (i + 1 < m.by_trust_depth.size() ? ",\n" : "\n");
  }
  j += "    ],\n";
  j += "    \"examples\": {\n";
  j += "      \"queue_order\": " + examples_json(m.queue_order_examples) + ",\n";
  j += "      \"price_skip\": " + examples_json(m.price_skip_examples) + ",\n";
  j += "      \"size_split\": " + examples_json(m.size_split_examples) + ",\n";
  j += "      \"length\": " + examples_json(m.length_examples) + "\n";
  j += "    }\n";
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

}  // namespace replay
