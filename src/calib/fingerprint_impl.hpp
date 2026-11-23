#pragma once
// Internals of the fingerprint module: the per-second sufficient statistics and how a block of them is
// recombined. Not part of the public API (include/calib/fingerprint.hpp).
#include <cstdint>
#include <vector>

#include "calib/fingerprint.hpp"

namespace calib {

// One channel of prefix sums. A channel maps a bin boundary i (0..bins) to an index range of its own
// series: the block of bins [s, e) uses indices [lo[s], hi[e]). Each stat has a lag: it sums its values
// over the indices [a, b - lag) (lag 0: all of them; lag k: the pairs (i, i + k) inside the block).
struct Channel {
  std::vector<std::int64_t> lo, hi;
  std::vector<std::vector<double>> pre;  // pre[stat][i], i = 0..number of indices
  std::vector<int> lag;
  std::size_t base{0};  // offset of this channel's first stat in an accumulator
};

// Index of the stats inside the series channels (after the additive channel).
enum SeriesStat { kSCount = 0, kSValid, kSS1, kSS2, kSS3, kSS4, kSFirstLag };
enum SeriesId { kSerR1 = 0, kSerR10, kSerR60, kSerAbs10, kSerSign, kSeriesCount };

struct Prepared::Impl {
  int levels{0};
  std::int64_t tick{1}, from{0}, to{0};
  int nb{0};
  std::vector<std::int64_t> spread_values;  // sorted distinct two-sided spreads, in ticks
  // offsets of the additive stats
  std::size_t o_dt{0}, o_nostate{0}, o_empty{0}, o_one{0}, o_crossed{0}, o_msgs{0}, o_depth{0}, o_hist{0},
      o_fn{0}, o_fk{0}, o_nodelta_n{0}, o_nodelta_k{0}, o_subsh{0}, o_exsh{0}, o_lim{0}, o_mkt{0};
  std::vector<Channel> chans;  // [0] additive, then one per SeriesId
  std::size_t acc_size{0};
  std::vector<int> series_lags[kSeriesCount];
  std::uint64_t off_grid_rows{0}, duplicate_ids{0};

  void add_range(std::vector<double>& acc, int s, int e) const;
  Fingerprint build(const std::vector<double>& acc, std::int64_t from_ns, std::int64_t to_ns) const;
  Fingerprint of_bins(int first, int last) const;
};

}  // namespace calib
