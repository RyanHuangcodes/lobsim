#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "replay/replayer.hpp"

// The matching-agreement check (spec 7). The replay itself is in replayer.cpp.
namespace replay {
namespace {

constexpr std::size_t kContext = 5;  // lines kept before and after an example burst

struct Comparison {
  BurstClass cls{BurstClass::Exact};
  std::size_t first_difference{0};  // 1-based; 0 when the fills agree
  bool predicted_better{false};     // price_skip only
};

// Fill by fill; the first difference decides (spec 7.4). A synthetic fill matches a synthetic
// fill at the same price: they have no external id to compare.
Comparison compare_fills(const std::vector<BurstFill>& data, const std::vector<BurstFill>& predicted,
                         lob::Side taker) {
  bool synthetic = false;
  const std::size_t n = std::min(data.size(), predicted.size());
  for (std::size_t i = 0; i < n; ++i) {
    const BurstFill& d = data[i];
    const BurstFill& p = predicted[i];
    synthetic = synthetic || d.synthetic || p.synthetic;
    if (d.price != p.price) {
      const bool better = taker == lob::Side::Buy ? p.price < d.price : p.price > d.price;
      return {BurstClass::PriceSkip, i + 1, better};
    }
    const bool same_order = d.synthetic == p.synthetic && (d.synthetic || d.external_id == p.external_id);
    if (!same_order) return {BurstClass::QueueOrder, i + 1, false};
    if (d.size != p.size) return {BurstClass::SizeSplit, i + 1, false};
  }
  if (data.size() != predicted.size()) return {BurstClass::Length, n + 1, false};
  return {synthetic ? BurstClass::ExactViaSynthetic : BurstClass::Exact, 0, false};
}

void count(MatchCounts& c, BurstClass cls, bool predicted_better) {
  switch (cls) {
    case BurstClass::Exact: ++c.exact; return;
    case BurstClass::ExactViaSynthetic: ++c.exact_via_synthetic; return;
    case BurstClass::QueueOrder: ++c.queue_order; return;
    case BurstClass::PriceSkip:
      ++c.price_skip;
      ++(predicted_better ? c.price_skip_better : c.price_skip_worse);
      return;
    case BurstClass::SizeSplit: ++c.size_split; return;
    case BurstClass::Length: ++c.length; return;
    case BurstClass::ExcludedUnknown: ++c.excluded_unknown; return;
    case BurstClass::ExcludedUntrusted: ++c.excluded_untrusted; return;
  }
}

std::string line_text(const Entry& e) { return "line " + std::to_string(e.line) + ": " + e.text; }

}  // namespace

const char* to_string(BurstClass c) noexcept {
  switch (c) {
    case BurstClass::Exact: return "exact";
    case BurstClass::ExactViaSynthetic: return "exact_via_synthetic";
    case BurstClass::QueueOrder: return "queue_order";
    case BurstClass::PriceSkip: return "price_skip";
    case BurstClass::SizeSplit: return "size_split";
    case BurstClass::Length: return "length";
    case BurstClass::ExcludedUnknown: return "excluded_unknown";
    case BurstClass::ExcludedUntrusted: return "excluded_untrusted";
  }
  return "?";
}

std::string to_string(const BurstFill& f) {
  const std::string who = f.synthetic ? "SYN@" + std::to_string(f.price) : std::to_string(f.external_id);
  return who + " " + std::to_string(f.size) + " @ " + std::to_string(f.price);
}

const MatchCounts& MatchingStats::headline() const {
  for (const MatchCounts& c : by_trust_depth) {
    if (c.trust_depth == headline_trust_depth) return c;
  }
  return by_trust_depth.front();  // unreachable: the constructor lists the headline depth
}

void Replayer::check_burst(const Entry& first, Source& source) {
  const LobsterMsg& head = *first.msg.value;
  std::vector<const Entry*> burst{&first};
  for (std::size_t i = 0;; ++i) {
    const Entry* next = source.peek(i);
    if (next == nullptr || !next->msg.ok()) break;
    const LobsterMsg& m = *next->msg.value;
    if (m.type != 4 || m.time_ns != head.time_ns || m.direction != head.direction) break;
    burst.push_back(next);
  }
  burst_left_ = burst.size() - 1;
  burst_size_ = burst.size();
  MatchingStats& stats = summary_.matching;
  ++stats.bursts;
  stats.burst_messages += burst.size();

  // The data side, as the messages will be applied one by one (4.1, 4.3), without changing the
  // book: what each message takes is added up per engine order, so a later message of the burst
  // sees what the earlier ones left.
  const lob::Side resting = head.direction > 0 ? lob::Side::Buy : lob::Side::Sell;
  std::unordered_map<lob::OrderId, lob::Qty> taken;
  std::vector<BurstFill> data;
  lob::OrderId last_synthetic = 0;  // the synthetic order the last data fill is attributed to; 0 when it is not
  lob::OrderId last_known = 0;      // the known order the last data fill is against; 0 when it is not
  bool unknown = false;  // also set when a size sum does not fit in 64 bits: the burst cannot be predicted
  std::set<std::int64_t> prices;
  lob::Qty qty = 0;
  std::int64_t limit = head.price;
  for (const Entry* e : burst) {
    const LobsterMsg& m = *e->msg.value;
    prices.insert(m.price);
    if (__builtin_add_overflow(qty, m.size, &qty)) unknown = true;  // hostile sizes: counted excluded_unknown
    limit = resting == lob::Side::Sell ? std::max(limit, m.price) : std::min(limit, m.price);
    if (const auto it = by_external_.find(m.order_id); it != by_external_.end()) {
      const std::optional<lob::OrderView> o = book_.order(it->second);
      if (o && taken[o->id] < o->remaining) {
        // A message that disagrees with its order is skipped when applied: it takes nothing.
        if (o->side == resting && o->price == m.price &&
            __builtin_add_overflow(taken[o->id], m.size, &taken[o->id])) {
          unknown = true;
        }
        // Two consecutive fills of one known order: two aggressive orders at the same instant (the
        // first stopped inside this order, the next went on from there) that the burst rule reads as
        // one. A single aggressive order fills a resting order in one piece, so they are one fill.
        if (last_known == o->id && data.back().external_id == m.order_id && data.back().price == m.price) {
          if (__builtin_add_overflow(data.back().size, m.size, &data.back().size)) unknown = true;
        } else {
          data.push_back(BurstFill{false, m.order_id, m.price, m.size});
        }
        last_synthetic = 0;
        last_known = o->id;
        continue;
      }
    }
    last_known = 0;
    bool attributed = false;
    for (const lob::OrderView& o : book_.level_orders(resting, m.price)) {
      if (synthetic_.count(o.id) == 0 || o.remaining - taken[o.id] < m.size) continue;
      if (__builtin_add_overflow(taken[o.id], m.size, &taken[o.id])) unknown = true;
      // Consecutive messages attributed to the same synthetic order are one fill of that order:
      // the data cannot say how a seeded quantity was split, the engine fills it in one piece
      // (fills of different real orders are never merged: their ids are known).
      if (last_synthetic == o.id) {
        data.back().size += m.size;  // fits: the sum was checked against `taken` just above
      } else {
        data.push_back(BurstFill{true, 0, m.price, m.size});
      }
      last_synthetic = o.id;
      attributed = true;
      break;
    }
    if (!attributed) unknown = true;
  }

  // The prediction: one aggressive order on the other side, up to the burst's worst price.
  const lob::Side taker = lob::opposite(resting);
  std::vector<BurstFill> predicted;
  if (!unknown) {
    for (const lob::Fill& f : book_.preview_match(taker, limit, qty)) {
      const auto ext = external_of_.find(f.maker_id);
      if (synthetic_.count(f.maker_id) != 0 || ext == external_of_.end()) {
        predicted.push_back(BurstFill{true, 0, f.price, f.qty});
      } else {
        predicted.push_back(BurstFill{false, ext->second, f.price, f.qty});
      }
    }
  }

  const Comparison cmp =
      unknown ? Comparison{BurstClass::ExcludedUnknown, 0, false} : compare_fills(data, predicted, taker);
  const auto distinct = static_cast<int>(prices.size());
  BurstClass headline = cmp.cls;
  for (MatchCounts& c : stats.by_trust_depth) {
    BurstClass cls = cmp.cls;
    const bool trusted = previous_exact_depth_ >= c.trust_depth && c.trust_depth >= distinct + 1;
    if (cls != BurstClass::ExcludedUnknown && !trusted) cls = BurstClass::ExcludedUntrusted;
    count(c, cls, cmp.predicted_better);
    if (c.trust_depth == stats.headline_trust_depth) headline = cls;
  }

  std::vector<BurstExample>* examples = nullptr;
  switch (headline) {
    case BurstClass::QueueOrder: examples = &stats.queue_order_examples; break;
    case BurstClass::PriceSkip: examples = &stats.price_skip_examples; break;
    case BurstClass::SizeSplit: examples = &stats.size_split_examples; break;
    case BurstClass::Length: examples = &stats.length_examples; break;
    default: break;
  }
  if (examples == nullptr || examples->size() >= kMaxBurstExamples) return;
  BurstExample x;
  x.line = first.line;
  x.first_difference = cmp.first_difference;
  if (headline == BurstClass::PriceSkip) x.predicted_better = cmp.predicted_better;
  for (const Entry* e : burst) x.messages.push_back(line_text(*e));
  for (const BurstFill& f : data) x.data.push_back(to_string(f));
  for (const BurstFill& f : predicted) x.predicted.push_back(to_string(f));
  // recent_ ends with the burst's first line.
  const std::size_t before = recent_size() - 1;
  for (std::size_t i = before > kContext ? before - kContext : 0; i < before; ++i) x.before.push_back(recent_line(i));
  for (std::size_t i = 0; i < kContext; ++i) {
    const Entry* e = source.peek(burst.size() - 1 + i);
    if (e == nullptr) break;
    x.after.push_back(line_text(*e));
  }
  examples->push_back(std::move(x));
}

}  // namespace replay
