// The estimator's statistics: auto tick, size pooling, Kaplan-Meier, the placement power-law fit
// and the sign autocorrelation. The replay observer is in estimator.cpp.
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "calib/estimator.hpp"

namespace calib {

std::int64_t choose_tick(std::int64_t gcd, std::string* warning) {
  if (gcd < 1) throw std::invalid_argument("choose_tick: the GCD must be positive");
  if (gcd % 100 == 0) return 100;
  if (warning != nullptr) {
    *warning = "tick: the GCD of the limit-order prices in the window is " + std::to_string(gcd) +
               ", not a multiple of 100, so tick_units is " + std::to_string(gcd) +
               " (real LOBSTER prices give 100; pass --tick-units to choose)";
  }
  return gcd;
}

std::int64_t percentile_999(const Pmf& p) {
  const std::uint64_t n = p.total();
  if (p.empty() || n == 0) throw std::invalid_argument("percentile_999: empty Pmf");
  const std::uint64_t rank = n - n / 1000;  // ceil(0.999 n), exactly, in integers
  std::uint64_t cum = 0;
  for (std::size_t i = 0; i < p.values.size(); ++i) {
    cum += p.counts[i];
    if (cum >= rank) return p.values[i];
  }
  return p.values.back();  // unreachable: cum reaches n >= rank
}

std::uint64_t pool_above_999(Pmf& p) {
  if (p.empty()) return 0;
  const std::int64_t top = percentile_999(p);
  const auto it = std::upper_bound(p.values.begin(), p.values.end(), top);
  const auto keep = static_cast<std::size_t>(it - p.values.begin());  // >= 1: top is a value
  std::uint64_t moved = 0;
  for (std::size_t i = keep; i < p.counts.size(); ++i) moved += p.counts[i];
  p.values.resize(keep);
  p.counts.resize(keep);
  p.counts.back() += moved;
  return moved;
}

Survival kaplan_meier(std::vector<std::int64_t> event_ns, std::vector<std::int64_t> censored_ns) {
  std::sort(event_ns.begin(), event_ns.end());
  std::sort(censored_ns.begin(), censored_ns.end());
  Survival s;
  std::uint64_t at_risk = event_ns.size() + censored_ns.size();
  double surv = 1.0;
  std::size_t i = 0, j = 0;
  while (i < event_ns.size()) {
    const std::int64_t t = event_ns[i];
    if (t <= 0) throw std::invalid_argument("kaplan_meier: event times must be positive");
    // Censored strictly before t have left the risk set; censored at t are still in it.
    while (j < censored_ns.size() && censored_ns[j] < t) {
      --at_risk;
      ++j;
    }
    std::uint64_t d = 0;
    while (i < event_ns.size() && event_ns[i] == t) {
      ++d;
      ++i;
    }
    surv *= static_cast<double>(at_risk - d) / static_cast<double>(at_risk);
    s.times_ns.push_back(t);
    s.survival.push_back(surv);
    s.at_risk.push_back(at_risk);
    s.events.push_back(d);
    at_risk -= d;
  }
  return s;
}

namespace {

// The placement fit's search ranges. mu below -1 means a distribution rising with Delta, which a
// truncated support allows; the fit is reported, never used.
constexpr double kMuLow = kFitMuLow;
constexpr double kMuHigh = kFitMuHigh;
constexpr double kDelta0Low = 0.01;
constexpr double kDelta0High = 1000.0;
constexpr int kGridPoints = 101;  // 20 per decade
constexpr int kGoldenSteps = 80;
// Z is summed term by term for Delta below this, and by Euler-Maclaurin above it.
constexpr std::int64_t kExactTerms = 1024;

// Maximum of a unimodal f on [lo, hi] by golden-section search: (argmax, max).
template <class F>
std::pair<double, double> golden_max(F f, double lo, double hi) {
  const double r = (std::sqrt(5.0) - 1.0) / 2.0;
  double a = lo, b = hi;
  double c = b - r * (b - a), d = a + r * (b - a);
  double fc = f(c), fd = f(d);
  for (int k = 0; k < kGoldenSteps; ++k) {
    if (fc >= fd) {
      b = d;
      d = c;
      fd = fc;
      c = b - r * (b - a);
      fc = f(c);
    } else {
      a = c;
      c = d;
      fc = fd;
      d = a + r * (b - a);
      fd = f(d);
    }
  }
  const double x = (a + b) / 2.0;
  return {x, f(x)};
}

struct FitData {
  std::vector<double> values;  // observed Delta >= 0
  std::vector<double> counts;
  double total{0.0};
  std::int64_t kmax{0};
};

// sum_{k=a}^{b} (delta0 + k)^-alpha for a <= b, by Euler-Maclaurin up to the third derivative
// (relative error below 1e-11 for a >= kExactTerms and alpha <= 21).
double tail_sum(double a, double b, double delta0, double alpha) {
  const auto f = [&](double x, double p) { return std::exp(-p * std::log(delta0 + x)); };
  const double u = std::log(delta0 + a), v = std::log(delta0 + b), w = v - u, beta = 1.0 - alpha;
  const double x = beta * w;
  const double integral = std::exp(beta * u) * w * (std::fabs(x) < 1e-12 ? 1.0 : std::expm1(x) / x);
  const double d1 = -alpha * (f(b, alpha + 1) - f(a, alpha + 1));
  const double d3 = -alpha * (alpha + 1) * (alpha + 2) * (f(b, alpha + 3) - f(a, alpha + 3));
  return integral + (f(a, alpha) + f(b, alpha)) / 2.0 + d1 / 12.0 - d3 / 720.0;
}

// Log-likelihood maximized over alpha = 1 + mu for one delta0 (concave in alpha: a linear term
// minus N log Z, and log Z is convex in alpha): (alpha, log-likelihood).
std::pair<double, double> best_alpha(const FitData& d, double delta0) {
  const std::int64_t exact = std::min(d.kmax + 1, kExactTerms);
  std::vector<double> logs(static_cast<std::size_t>(exact));
  for (std::size_t k = 0; k < logs.size(); ++k) logs[k] = std::log(delta0 + static_cast<double>(k));
  double weighted = 0.0;  // sum_i n_i log(delta0 + Delta_i)
  for (std::size_t i = 0; i < d.values.size(); ++i) weighted += d.counts[i] * std::log(delta0 + d.values[i]);
  const auto ll = [&](double alpha) {
    double z = 0.0;
    for (const double l : logs) z += std::exp(-alpha * l);
    if (d.kmax >= kExactTerms) {
      z += tail_sum(static_cast<double>(kExactTerms), static_cast<double>(d.kmax), delta0, alpha);
    }
    return -alpha * weighted - d.total * std::log(z);
  };
  return golden_max(ll, 1.0 + kMuLow, 1.0 + kMuHigh);
}

}  // namespace

std::optional<PlacementFit> fit_placement(const Pmf& nonneg) {
  if (nonneg.values.size() < 2 || nonneg.total() < 2) return std::nullopt;
  if (nonneg.values.front() < 0) throw std::invalid_argument("fit_placement: values must be >= 0");
  FitData d;
  d.kmax = nonneg.values.back();
  for (std::size_t i = 0; i < nonneg.values.size(); ++i) {
    d.values.push_back(static_cast<double>(nonneg.values[i]));
    d.counts.push_back(static_cast<double>(nonneg.counts[i]));
    d.total += static_cast<double>(nonneg.counts[i]);
  }
  // delta0 on a log grid, then golden-section in log10(delta0) between the best point's neighbours.
  std::vector<double> grid(static_cast<std::size_t>(kGridPoints));
  const double lo = std::log10(kDelta0Low), hi = std::log10(kDelta0High);
  for (int i = 0; i < kGridPoints; ++i) grid[static_cast<std::size_t>(i)] = lo + (hi - lo) * i / (kGridPoints - 1);
  std::size_t best = 0;
  double best_ll = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < grid.size(); ++i) {
    const double ll = best_alpha(d, std::pow(10.0, grid[i])).second;
    if (ll > best_ll) {
      best_ll = ll;
      best = i;
    }
  }
  const double a = grid[best == 0 ? 0 : best - 1];
  const double b = grid[std::min(best + 1, grid.size() - 1)];
  const auto [log_delta0, ll] =
      golden_max([&](double x) { return best_alpha(d, std::pow(10.0, x)).second; }, a, b);
  double delta0 = std::pow(10.0, log_delta0);
  if (ll < best_ll) delta0 = std::pow(10.0, grid[best]);  // keep the grid point unless refining helped
  PlacementFit f;
  f.delta0 = delta0;
  f.mu = best_alpha(d, delta0).first - 1.0;
  f.samples = nonneg.total();
  f.method =
      "discrete maximum likelihood of (delta0 + Delta)^-(1 + mu) / Z on Delta = 0.." + std::to_string(d.kmax) +
      " (truncated at the largest observed Delta), over the samples with Delta >= 0 of all spread buckets; mu in "
      "[-3, 20] by golden-section search for each delta0 (the log-likelihood is concave in mu); delta0 on a log grid "
      "of 101 points in [0.01, 1000], refined by golden-section search between the best point's neighbours";
  return f;
}

std::optional<double> sign_autocorrelation(const std::vector<std::int8_t>& signs, std::size_t lag) {
  const std::size_t n = signs.size();
  if (n <= lag) return std::nullopt;
  double mean = 0.0;
  for (const std::int8_t x : signs) mean += x;
  mean /= static_cast<double>(n);
  double c0 = 0.0;
  for (const std::int8_t x : signs) c0 += (x - mean) * (x - mean);
  if (c0 == 0.0) return std::nullopt;
  double ck = 0.0;
  for (std::size_t i = 0; i + lag < n; ++i) ck += (signs[i] - mean) * (signs[i + lag] - mean);
  return ck / c0;  // the 1/n factors cancel
}

}  // namespace calib
