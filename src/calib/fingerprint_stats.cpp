// Scalars, distances, bootstrap and run combination of the fingerprint (public API in fingerprint.hpp).
#include <algorithm>
#include <cmath>
#include <limits>
#include <ostream>
#include <stdexcept>

#include "calib/fingerprint.hpp"
#include "calib/fingerprint_impl.hpp"
#include "sim/rng.hpp"

namespace calib {
namespace {
const double kNaN = std::numeric_limits<double>::quiet_NaN();

// Linear-interpolation percentile of a sorted vector, q in [0, 1].
double percentile(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) return kNaN;
  const double pos = q * static_cast<double>(sorted.size() - 1);
  const std::size_t i = static_cast<std::size_t>(std::floor(pos));
  const double frac = pos - static_cast<double>(i);
  if (i + 1 >= sorted.size()) return sorted.back();
  return sorted[i] * (1.0 - frac) + sorted[i + 1] * frac;
}

Spread summarize(double point, std::vector<double> v) {
  Spread s;
  s.value = point;
  std::erase_if(v, [](double x) { return !std::isfinite(x); });
  s.defined = static_cast<std::uint32_t>(v.size());
  s.mean = s.se = s.lo = s.hi = kNaN;
  if (v.empty()) return s;
  double m = 0;
  for (double x : v) m += x;
  m /= static_cast<double>(v.size());
  double ss = 0;
  for (double x : v) ss += (x - m) * (x - m);
  s.mean = m;
  s.se = v.size() > 1 ? std::sqrt(ss / static_cast<double>(v.size() - 1)) : 0.0;
  if (v.size() >= 20) {
    std::sort(v.begin(), v.end());
    s.lo = percentile(v, 0.025);
    s.hi = percentile(v, 0.975);
  }
  return s;
}

Pmf depth_pmf(const std::vector<double>& d) {
  Pmf p;
  for (std::size_t l = 0; l < d.size(); ++l) {
    if (!std::isfinite(d[l])) continue;
    const auto c = static_cast<std::int64_t>(std::llround(1000.0 * d[l]));
    if (c <= 0) continue;
    p.values.push_back(static_cast<std::int64_t>(l) + 1);
    p.counts.push_back(static_cast<std::uint64_t>(c));
  }
  return p;
}

Distance dist(const Pmf& a, const Pmf& b) {
  Distance d;
  if (a.empty() || b.empty()) return d;
  d.w1 = wasserstein1(a, b);
  d.tv = total_variation(a, b);
  d.valid = true;
  return d;
}

// Blocks of `bl` bins starting uniformly in [first, last - bl], enough to cover the range.
void resample(const Prepared::Impl& I, sim::Xoshiro256StarStar& rng, int first, int last, int bl, std::vector<double>& acc) {
  const int n = last - first;
  const int blocks = (n + bl - 1) / bl;
  for (int k = 0; k < blocks; ++k) {
    const int s = static_cast<int>(rng.uniform_int(first, last - bl));
    I.add_range(acc, s, s + bl);
  }
}
}  // namespace

std::vector<NamedScalar> scalars(const Fingerprint& f) {
  std::vector<NamedScalar> v;
  auto add = [&](std::string n, double x) { v.push_back({std::move(n), x}); };
  add("spread_mean", f.spread_mean);
  add("spread_one_tick_share", f.spread_one_tick_share);
  add("one_sided_share", f.one_sided_share);
  for (int s = 0; s < 2; ++s)
    for (std::size_t l = 0; l < f.depth[static_cast<std::size_t>(s)].size(); ++l)
      add(std::string(s == 0 ? "depth_buy_L" : "depth_sell_L") + std::to_string(l + 1), f.depth[static_cast<std::size_t>(s)][l]);
  for (const VolStat& x : f.vol) add("vol_" + std::to_string(x.clock_s) + "s", x.stdev);
  add("kurtosis_10s", f.kurtosis_10s);
  add("acf_ret_1s_lag1", f.acf_ret_1s_lag1);
  for (int k = 0; k < kAbsAcfLags; ++k) add("acf_abs_10s_lag" + std::to_string(k + 1), f.acf_abs_10s[static_cast<std::size_t>(k)]);
  for (int k = 0; k < kSignAcfLags; ++k) add("sign_acf_lag" + std::to_string(kSignLags[static_cast<std::size_t>(k)]), f.sign_acf[static_cast<std::size_t>(k)]);
  for (int b = 0; b < kDistanceBuckets; ++b) {
    static const char* names[] = {"fill_le0", "fill_1_2", "fill_3_9", "fill_ge10"};
    add(names[b], f.fill[static_cast<std::size_t>(b)].prob);
  }
  add("fill_all", f.fill_all.prob);
  add("executed_share", f.executed_share);
  add("lambda_limit_buy", f.lambda_limit[kBuy]);
  add("lambda_limit_sell", f.lambda_limit[kSell]);
  add("lambda_limit_total", f.lambda_limit_total);
  add("lambda_market_buy", f.lambda_market[kBuy]);
  add("lambda_market_sell", f.lambda_market[kSell]);
  add("lambda_market_total", f.lambda_market_total);
  return v;
}

DistributionDistances distances(const Fingerprint& a, const Fingerprint& b) {
  DistributionDistances d;
  d.spread = dist(a.spread_pmf, b.spread_pmf);
  for (std::size_t s = 0; s < 2; ++s) d.depth[s] = dist(depth_pmf(a.depth[s]), depth_pmf(b.depth[s]));
  return d;
}

BootstrapResult Prepared::bootstrap(const BootstrapOptions& o) const {
  if (o.block_seconds < 1 || o.resamples < 1) throw std::invalid_argument("bad bootstrap options");
  const Impl& I = *impl_;
  BootstrapResult r;
  r.options = o;
  const int bl = std::min(o.block_seconds, I.nb);
  r.block_seconds_used = bl;
  r.blocks_per_resample = (I.nb + bl - 1) / bl;
  r.degenerate = I.nb <= bl;
  const auto point = scalars(I.of_bins(0, I.nb));
  std::vector<std::vector<double>> vals(point.size());
  sim::Xoshiro256StarStar rng(o.seed);
  std::vector<double> acc;
  for (int k = 0; k < o.resamples; ++k) {
    acc.assign(I.acc_size, 0.0);
    resample(I, rng, 0, I.nb, bl, acc);
    const auto sc = scalars(I.build(acc, I.from, I.to));
    for (std::size_t i = 0; i < sc.size(); ++i) vals[i].push_back(sc[i].value);
  }
  for (std::size_t i = 0; i < point.size(); ++i) {
    const Spread s = summarize(point[i].value, vals[i]);
    r.scalars.push_back({point[i].name, point[i].value, s.lo, s.hi, s.se, s.defined});
  }
  return r;
}

HalvesYardstick Prepared::halves(const BootstrapOptions& o) const {
  const Impl& I = *impl_;
  const int mid = I.nb / 2;
  if (mid < 1) throw std::invalid_argument("the window is too short to split in halves");
  HalvesYardstick h;
  const Fingerprint a = I.of_bins(0, mid), b = I.of_bins(mid, I.nb);
  h.point = distances(a, b);
  const int len[2] = {mid, I.nb - mid};
  const int bl[2] = {std::min(o.block_seconds, len[0]), std::min(o.block_seconds, len[1])};
  h.blocks_per_half[0] = (len[0] + bl[0] - 1) / bl[0];
  h.blocks_per_half[1] = (len[1] + bl[1] - 1) / bl[1];
  std::vector<double> sw, st, dw[2], dt;
  std::vector<double> dtv[2];
  sim::Xoshiro256StarStar rng(o.seed ^ 0x9e3779b97f4a7c15ULL);
  std::vector<double> acc;
  for (int k = 0; k < o.resamples; ++k) {
    Fingerprint f[2];
    for (int half = 0; half < 2; ++half) {
      acc.assign(I.acc_size, 0.0);
      resample(I, rng, half == 0 ? 0 : mid, half == 0 ? mid : I.nb, bl[half], acc);
      f[half] = I.build(acc, I.from, I.to);
    }
    const DistributionDistances d = distances(f[0], f[1]);
    if (d.spread.valid) {
      sw.push_back(d.spread.w1);
      st.push_back(d.spread.tv);
    }
    for (std::size_t s = 0; s < 2; ++s)
      if (d.depth[s].valid) {
        dw[s].push_back(d.depth[s].w1);
        dtv[s].push_back(d.depth[s].tv);
      }
  }
  h.spread_w1 = summarize(h.point.spread.w1, sw);
  h.spread_tv = summarize(h.point.spread.tv, st);
  for (std::size_t s = 0; s < 2; ++s) {
    h.depth_w1[s] = summarize(h.point.depth[s].w1, dw[s]);
    h.depth_tv[s] = summarize(h.point.depth[s].tv, dtv[s]);
  }
  return h;
}

double student_t975(int df) {
  static const double t[] = {12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
                             2.201,  2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
                             2.080,  2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042};
  if (df < 1) throw std::invalid_argument("df must be at least 1");
  if (df <= 30) return t[df - 1];
  return 1.959964 + 2.3732 / static_cast<double>(df);  // z + (z^3 + z) / (4 df)
}

std::vector<RunSummary> combine_runs(const std::vector<Fingerprint>& runs) {
  if (runs.empty()) throw std::invalid_argument("combine_runs: no runs");
  std::vector<std::vector<NamedScalar>> all;
  for (const Fingerprint& f : runs) {
    all.push_back(scalars(f));
    if (all.back().size() != all.front().size()) throw std::invalid_argument("combine_runs: runs differ in L");
  }
  std::vector<RunSummary> out;
  for (std::size_t i = 0; i < all[0].size(); ++i) {
    std::vector<double> v;
    for (const auto& s : all)
      if (std::isfinite(s[i].value)) v.push_back(s[i].value);
    RunSummary r;
    r.name = all[0][i].name;
    r.n = static_cast<std::uint32_t>(v.size());
    r.mean = r.sd = r.ci_lo = r.ci_hi = r.run_lo = r.run_hi = kNaN;
    if (!v.empty()) {
      double m = 0;
      for (double x : v) m += x;
      m /= static_cast<double>(v.size());
      r.mean = m;
      if (v.size() >= 2) {
        double ss = 0;
        for (double x : v) ss += (x - m) * (x - m);
        r.sd = std::sqrt(ss / static_cast<double>(v.size() - 1));
        const double half = student_t975(static_cast<int>(v.size()) - 1) * r.sd / std::sqrt(static_cast<double>(v.size()));
        r.ci_lo = m - half;
        r.ci_hi = m + half;
        std::sort(v.begin(), v.end());
        r.run_lo = percentile(v, 0.025);
        r.run_hi = percentile(v, 0.975);
      }
    }
    out.push_back(std::move(r));
  }
  return out;
}

// ---- JSON --------------------------------------------------------------------------------------

namespace {
using nlohmann::json;
json pmf_json(const Pmf& p) { return json{{"values", p.values}, {"weights_ns", p.counts}}; }
json fill_json(const FillStat& s) { return json{{"orders", s.orders}, {"filled", s.filled}, {"prob", s.prob}}; }
json spread_json(const Spread& s) {
  return json{{"point", s.value}, {"boot_mean", s.mean}, {"boot_se", s.se}, {"lo", s.lo}, {"hi", s.hi}, {"defined", s.defined}};
}
json dist_json(const Distance& d) { return json{{"valid", d.valid}, {"w1", d.w1}, {"tv", d.tv}}; }
}  // namespace

json to_json(const Fingerprint& f) {
  json j;
  j["levels"] = f.levels;
  j["tick_units"] = f.tick_units;
  j["window_ns"] = {f.from_ns, f.to_ns};
  j["messages"] = f.messages;
  j["time_ns"] = {{"total", f.total_ns}, {"no_state", f.no_state_ns}, {"empty", f.empty_ns},
                  {"one_sided", f.one_sided_ns}, {"two_sided", f.two_sided_ns}, {"crossed", f.crossed_ns}};
  j["spread_pmf_ticks"] = pmf_json(f.spread_pmf);
  j["depth_mean_by_level"] = {{"buy", f.depth[kBuy]}, {"sell", f.depth[kSell]}};
  json vol = json::array();
  for (const VolStat& v : f.vol) vol.push_back({{"clock_s", v.clock_s}, {"std", v.stdev}, {"n", v.n}, {"skipped", v.skipped}});
  j["volatility"] = vol;
  j["kurtosis_10s"] = f.kurtosis_10s;
  j["acf_ret_1s_lag1"] = f.acf_ret_1s_lag1;
  j["acf_abs_10s_lags_1_to_10"] = f.acf_abs_10s;
  j["bursts"] = f.bursts;
  j["sign_acf_lags_1_10_100"] = f.sign_acf;
  json fill = json::array();
  for (const FillStat& s : f.fill) fill.push_back(fill_json(s));
  j["fill_by_distance_bucket"] = fill;
  j["fill_all"] = fill_json(f.fill_all);
  j["placement_unknown"] = f.placement_unknown;
  j["submitted_shares"] = f.submitted_shares;
  j["executed_shares"] = f.executed_shares;
  j["executed_share"] = f.executed_share;
  j["lambda_limit_per_s"] = {{"buy", f.lambda_limit[kBuy]}, {"sell", f.lambda_limit[kSell]}, {"total", f.lambda_limit_total}};
  j["lambda_market_per_s"] = {{"buy", f.lambda_market[kBuy]}, {"sell", f.lambda_market[kSell]}, {"total", f.lambda_market_total}};
  j["off_grid_rows"] = f.off_grid_rows;
  j["duplicate_order_ids"] = f.duplicate_order_ids;
  json sc = json::object();
  for (const NamedScalar& s : scalars(f)) sc[s.name] = s.value;
  j["scalars"] = sc;
  return j;
}

void write_json(std::ostream& out, const Fingerprint& f) { out << to_json(f).dump(2) << '\n'; }

json to_json(const BootstrapResult& b) {
  json j;
  j["block_seconds"] = b.block_seconds_used;
  j["resamples"] = b.options.resamples;
  j["seed"] = b.options.seed;
  j["blocks_per_resample"] = b.blocks_per_resample;
  j["degenerate"] = b.degenerate;
  json s = json::array();
  for (const ScalarInterval& x : b.scalars)
    s.push_back({{"name", x.name}, {"value", x.value}, {"lo", x.lo}, {"hi", x.hi}, {"se", x.se}, {"defined", x.defined}});
  j["scalars"] = s;
  return j;
}

json to_json(const std::vector<RunSummary>& runs) {
  json s = json::array();
  for (const RunSummary& r : runs)
    s.push_back({{"name", r.name}, {"n", r.n}, {"mean", r.mean}, {"sd", r.sd}, {"ci_lo", r.ci_lo}, {"ci_hi", r.ci_hi},
                 {"run_lo", r.run_lo}, {"run_hi", r.run_hi}});
  return s;
}

json to_json(const DistributionDistances& d) {
  return json{{"spread", dist_json(d.spread)}, {"depth_buy", dist_json(d.depth[kBuy])}, {"depth_sell", dist_json(d.depth[kSell])}};
}

json to_json(const HalvesYardstick& h) {
  return json{{"point", to_json(h.point)},
              {"spread_w1", spread_json(h.spread_w1)},
              {"spread_tv", spread_json(h.spread_tv)},
              {"depth_buy_w1", spread_json(h.depth_w1[kBuy])},
              {"depth_buy_tv", spread_json(h.depth_tv[kBuy])},
              {"depth_sell_w1", spread_json(h.depth_w1[kSell])},
              {"depth_sell_tv", spread_json(h.depth_tv[kSell])},
              {"blocks_per_half", {h.blocks_per_half[0], h.blocks_per_half[1]}}};
}

}  // namespace calib
