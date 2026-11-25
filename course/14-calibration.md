# Module 14: Calibration

> **In one sentence:** instead of guessing how the background traders behave, lobsim measures it from a day of data (how often orders arrive, where they are placed, how big they are, how long they live), builds a trader that copies those measurements, and then tests the result on a later part of the day that the measuring step never saw.
>
> **You will be able to:** explain "fit the inputs, validate the outputs out of sample", define a placement distance and compute one, compute a Kaplan–Meier survival curve with censoring, read the calibration file, explain how the calibrated agent samples exactly and portably, describe parameter recovery and what it proved, read a validation scorecard and its verdict rule, and explain the leakage check.
>
> **Time:** about 5 hours. This is the most statistical module; Module 11 should feel comfortable first.

> **Data status.** As in Module 13, no real data has been used yet. Every number here comes from synthetic days (lobsim's own output, labelled SYNTHETIC) or from a hand-made "truth". The same code will run unchanged on real files.

## 14.1 The problem with hand-picked settings

The noise traders of Module 9 use settings someone chose: offsets uniform from 1 to 10 ticks, sizes uniform from 1 to 10, exponential lifetimes with mean 30 s. Real data looks different (CALIBRATION.md section 1): distances of new orders from the best price are spread very broadly (roughly a power law), sizes cluster at round numbers, and lifetimes are not exponential (one study found 27.7% of limit orders cancelled within two seconds).

How do you make a simulator match a real market? A popular answer is to tune parameters until the simulation reproduces a few well-known statistics. Platt and Gebbie showed that this is not enough: many different settings can reproduce the same few statistics, so matching them says little. lobsim splits the job into two steps that share nothing:

1. **Fit the inputs directly.** Measure the microscopic order flow straight from the data: rates, placements, sizes, lifetimes. No optimizer, no tuning.
2. **Validate the outputs out of sample.** Simulate with those measured inputs, then compare **emergent** properties that were never fitted (spread, depth, volatility, fill rates) against a **different time period** of the data that the measuring step never read.

Why expect this to work at all? Farmer, Patelli and Zovko built a zero-intelligence model from directly measured order-flow rates and it explained 96% of the differences in bid-ask spreads across stocks, and 76% of the differences in how fast prices diffuse. So: expect spreads to match well, volatility less well.

## 14.2 What the estimator measures

`lobsim_calibrate estimate` (`src/calib/estimator.cpp`) is a **replay observer** (Module 13's `ReplayObserver`): it watches every message of a time window together with the **published** book rows just before and after it. It never trusts the replayed book, which can be wrong (Module 13.6); the published rows are the truth.

**The window.** By default 10:00 to 15:30, leaving out the open and the close, which behave differently. Messages before the window are replayed (so the book is right) but not measured; the replay **stops before** the first message at or after the window's end, so nothing later is ever read (the `stop_before` hook).

**Rates.** λ_L: limit orders (type 1) per second, by side. λ_M: aggressive orders per second, counted as **bursts** of executions (Module 13.5), by the aggressor's side (the opposite of LOBSTER's Direction).

**Placement distance.** For a new buy at price p, measured from the best bid b just **before** it arrives, in ticks:

$$\Delta = \frac{b - p}{\text{tick}}$$

Δ = 0 joins the best bid, Δ > 0 is behind it, Δ < 0 improves it (inside the spread). Sells mirror this from the best ask. An order that does not cross has Δ ≥ −(s − 1), with s the spread in ticks. The worked example of CALIBRATION.md: best bid 100.00, best ask 100.03, tick 0.01, so s = 3. A buy at 99.98 has Δ = 2; at 100.01, Δ = −1; at 100.02, Δ = −2 = −(s − 1), the most aggressive price that does not cross. The estimator keeps one histogram (a PMF, "probability mass function") of Δ per side and per spread bucket (s = 1, s = 2, s ≥ 3), because where people place orders depends on how wide the spread is.

The code, from `Collector::submit` in `estimator.cpp` (line 189), which handles every type-1 message:

```cpp
      spread = (ask.price - bid.price) / tick_;
      delta = side == kBuy ? (bid.price - m.price) / tick_ : (m.price - ask.price) / tick_;
      if (delta < -(spread - 1)) why = "would_cross";
    ...
  ++placement_[side][spread_bucket(spread)][delta];
```

Orders that cannot be placed (no previous row, a one-sided or crossed row, an off-grid price, a would-cross) are excluded **and counted by reason**, never dropped silently.

**The tick.** Real LOBSTER prices are dollars × 10,000; a one-cent tick is 100 units. The estimator finds the tick as the greatest common divisor of the limit prices in the window (a multiple of 100 gives 100). Prices that are not a multiple of the tick ("subpenny") are counted and excluded.

**Sizes.** One PMF for limit order sizes, one for the total size of each aggressive burst. The rare sizes above the 99.9th percentile are pooled into the top bin (and the number pooled is reported).

**Lifetimes**: next section.

## 14.3 Survival curves and censoring

An order's **lifetime** is the time from submission to cancellation. But many orders never get cancelled where we can see it:

- it was **executed** first (we only know it would have lived at least that long);
- the **window ended** first;
- it **left the published window**: its price became worse than the L-th published level, after which LOBSTER stops sending its messages, so its fate is unknown (with the exception that a side showing fewer than L levels is fully visible).

These lifetimes are **censored**: we know a lower bound, not the value. Dropping them, or counting them as cancels, both bias the answer (Lab 14.1 shows how much). The standard tool from medicine and engineering is the **Kaplan–Meier** estimator. With distinct cancel times t₁ < t₂ < ..., dᵢ cancels at tᵢ and nᵢ orders still "at risk" (alive and observed) just before tᵢ:

$$\hat S(t) = \prod_{t_i \le t} \left(1 - \frac{d_i}{n_i}\right)$$

Ŝ(t) is the estimated chance that an order is still alive after t. Censored orders count in nᵢ while they are observed and leave the risk set without counting as a cancel.

**Worked example** (CALIBRATION.md section 2, checked exactly by the unit test `KaplanMeier.WorkedExampleOfSection13`): four orders. A is cancelled at 1 s, B executed at 2 s (censored), C cancelled at 3 s, D still alive at the end, 5 s (censored). At t = 1: n = 4, d = 1, Ŝ = 3/4. At t = 3: n = 2 (A is gone, B censored), d = 1, Ŝ = 3/4 × 1/2 = 3/8.

The estimator keeps one survival curve per distance bucket (Δ ≤ 0, 1 to 2, 3 to 9, ≥ 10), since orders far from the price live differently from orders at the front.

## 14.4 The calibration file

The estimate is written as JSON (format: `calibration/SCHEMA.md`, C++ type `calib::Calibration` in `include/calib/calibration.hpp`). It is designed to be traceable:

- `source`: `"real"`, `"synthetic"` or `"hand-made"`. Every number is labelled by it, and the estimator marks data synthetic when a `README_SYNTHETIC.txt` sits next to the message file.
- `inputs`: the file names and their **SHA-256** fingerprints; `code_commit`: the git commit of the estimator; the window used and a note on how it was chosen.
- the estimates as plain arrays: a PMF is `values` (strictly increasing) and `counts`; a survival curve is `times_ns`, `survival`, `at_risk`, `events`, plus the counts of each kind of censoring;
- `report`: diagnostics for people and for validation, never read by the simulator.

Prices, distances and sizes are integers; times are integer nanoseconds. Only rates, probabilities and survival values are decimals.

## 14.5 The calibrated background flow

`CalibratedFlowAgent` (`src/sim/agents/calibrated_flow.cpp`) replaces the ten noise traders with **one** agent that samples the file. It is switched on by an optional `background` section in the config; without it nothing changes (every old config, hash and output is identical).

- **Two Poisson clocks**: limit orders at λ_L (buy + sell), aggressive market orders at λ_M. The side is drawn with the measured probabilities.
- **Placement**: the spread bucket comes from the agent's view of the book; Δ is drawn from that bucket's PMF; the price is the own-side best ∓ Δ. A Δ that would cross is drawn again, up to 100 times, then clamped to −(s − 1) and counted.
- **Size** from the measured PMF; **lifetime** drawn when the `Ack` arrives, from a separate stream (Module 7.6), by inverse transform on the survival curve.
- **Fallbacks**, each counted: one-sided or empty views of the book; a spread bucket with no data uses the nearest non-empty one (D-385, D-399).
- **Many owner ids.** One agent produces the whole flow, so its market orders would constantly meet its own resting orders and be cut short by self-trade prevention (Module 2.6). It therefore sends orders under many owner ids in turn. Parameter recovery measured that the default 50 ids are **not** enough (6.3% of aggressive orders were truncated), so the validation runs use 10,000 (D-393).

### Sampling exactly and portably

How do you draw from a histogram such as "100 shares 40%, 200 shares 20%, ..."? Floating-point probabilities and `std::discrete_distribution` would be neither exact nor the same on every platform (Module 7). Instead each PMF becomes a table of **integer cumulative counts**:

```cpp
CumulativeTable::CumulativeTable(const calib::Pmf& pmf) {
  ...
  std::uint64_t sum = 0;
  for (std::uint64_t c : pmf.counts) {
    if (c == 0 || sum > ~std::uint64_t{0} - c) throw std::invalid_argument("CumulativeTable: bad counts");
    sum += c;
    cum_.push_back(sum);
  }
}

std::int64_t CumulativeTable::value_at(std::uint64_t r) const {
  if (r >= total()) throw std::out_of_range("CumulativeTable: draw beyond the total");
  // The first entry whose cumulative count is above r.
  const auto it = std::upper_bound(cum_.begin(), cum_.end(), r);
  return values_[static_cast<std::size_t>(it - cum_.begin())];
}
```

A draw is one `uniform_int(0, total − 1)` (exactly fair, Module 7.4) followed by a **binary search** (`std::upper_bound`, O(log n)) for the first cumulative count above it. With counts 40, 20, 40 the cumulative table is 40, 60, 100: a draw of 0 to 39 gives the first value, 40 to 59 the second, 60 to 99 the third. Exact, and the same on every machine.

Lifetimes use the same idea on the survival curve: draw u in [0, 1) and take the first time where Ŝ(t) ≤ u (`lifetime_from_survival`, a `std::partition_point`). If u is below the curve's last value (the curve never gets that low), no cancel is scheduled: that order lives until it fills or the session ends, and these "no-cancel draws" are counted.

## 14.6 Parameter recovery: test the estimator on a known truth

Before trusting the estimator on real data, test it where the right answer is known: **choose** the parameters, simulate them, estimate them back, compare. `calibration/recovery_truth.json` is a hand-made "truth" in the same file format (written by `tools/make_recovery_truth.py`): 40 limit and 4 aggressive orders per second, a power-law placement, lifetimes that are 30% "fleeting" (gone within 0.5 s) and otherwise exponential, and sizes clustered on round lots.

`lobsim_calibrate compare` judges the estimate by stated rules: rates within 3%, placement total variation ≤ 0.03 per bucket with enough samples, size TV ≤ 0.02, lifetime medians within 10%. **Total variation** TV = ½ Σ |P(x) − Q(x)| is the share of probability in the wrong place (0 = same shape, 1 = no overlap); CALIBRATION.md also uses the **Wasserstein-1** distance, W1 = Σ |F_P(x) − F_Q(x)|, which also measures *how far* probability is misplaced, in ticks or shares (Lab 14.3).

What it showed (CALIBRATION.md section 5, synthetic): two hours exported with 40 levels, all 14 judged checks pass, and pass again on 10 of 10 further seeds; pooled over seeds, the estimated placement is statistically indistinguishable from exact sampling of the truth. Then three honest limits:

- With only **10 levels exported**, the lifetime of orders 10 or more ticks deep fails: almost all of them are outside the published window the moment they are submitted, so they are censored at once and too few cancels are left to measure (198 of 26,591 orders). The choice of L = 40 is stated as the reason the main check passes.
- With **LOBSTER's price-window filter** at L = 10, λ_L reads 9% low and the far tail of the placement PMF is lost (orders placed outside the window are never seen); the deep-lifetime check passes in only 5 of 10 seeds.
- **The check was tested.** Nine faults were put into a copy of the estimator, one at a time. Eight were caught. The ninth (reading the aggressor as the Direction side) was not, because the truth is the same on both sides: the unit tests cover that one instead.

## 14.7 The fingerprint and out-of-sample validation

**One measuring stick.** `include/calib/fingerprint.hpp` (`lobsim_fingerprint`) computes a scorecard from LOBSTER-format files: spread distribution, depth at each level, volatility of log mid returns at 1, 10 and 60 s, kurtosis, autocorrelations, the memory of trade signs, fill probability by distance, and the input rates as a sanity check. Simulations are first **exported in LOBSTER format with the same filter and the same L** as the data, so data and simulation go through exactly the same code. No apples-to-oranges.

**Validation** (`lobsim_calibrate validate`):

1. Estimate from the **training** period.
2. Simulate 20 seeds of the calibrated flow, each as long as the **test** period (after a 300 s warm-up that is not measured), and 20 seeds of the old noise traders as a **baseline** under the same conditions.
3. Fingerprint the data's test period and both simulations, and give each metric a verdict.

**Uncertainty.** For the data: a **moving-block bootstrap**. The test period is cut into one-second bins, and 1,000 fake "test periods" are built by gluing together randomly chosen 5-minute blocks of it; the spread of the metric across them gives a 95% interval. (Blocks rather than single seconds, so that neighbouring seconds stay together and their dependence is kept.) For the simulations: a 95% interval of the mean over the 20 seeds.

**The verdict rule** (D-377): **match** if the two intervals overlap or touch; **close** if the gap between them is at most twice the data interval's half-width; **miss** otherwise; **n/a** if a number is undefined. The baseline is judged by the same rule, and nothing is tuned to improve a verdict.

**Leakage.** If the estimator saw any of the test period, the "out of sample" test would be a fake. After estimating, `check_estimator_range` requires that every message the estimator read lies inside the training period; otherwise the run stops before simulating anything (`LeakageError`). Tests make sure a stand-in estimator that reads 30 s into the test period is refused.

### What the synthetic dress rehearsal showed

On the two synthetic days (train on the first half hour, test on the second; CALIBRATION.md section 8):

| | Calibrated flow (match / close / miss) | Old noise traders |
|---|---|---|
| SYNTHA | 31 / 17 / 4 | 29 / 12 / 11 |
| SYNTHB | 36 / 12 / 4 | 16 / 12 / 24 |
| calibrated on SYNTHA, tested on SYNTHB | 16 / 14 / 22 | 16 / 12 / 24 |

(one metric undefined in each.) Depth at every level, lifetimes and most rates match or are close. What misses is informative: the fake days were generated with an informed trader and a market maker, which the calibrated zero-intelligence flow does not include, so it misses the **memory of trade signs** (the informed trader keeps trading the same way) and the share of one-tick spreads (the market maker's quotes). An ablation run confirmed those links. And calibrating on one day and testing on a day that trades twice as fast does much worse: a calibration describes the market it came from. CALIBRATION.md is explicit that this rehearsal is partly circular (the data is itself a lobsim run) and says nothing about real markets.

**Known gaps** (README, CALIBRATION.md section 9): order signs have no memory in the calibrated flow (real signs have long memory, autocorrelation decaying roughly like τ^−0.6), and rates do not depend on the state of the book (as they do in "queue-reactive" models).

**Skipped on purpose:** the JSON reader and writer (`src/calib/calibration.cpp`), the estimator's lifetime tracking and its report (`estimator_stats.cpp`, `estimator_report.cpp`), the power-law fit (reported, never used by the simulator), the fingerprint's statistics (`fingerprint_stats.cpp`), the validation report writer, and the SHA-256 implementation (`src/calib/sha256.cpp`).

## Lab 14.1: Kaplan–Meier, by hand and by the estimator

```sh
python3 course/labs/py/kaplan_meier.py
```

Expected:

```
worked example:
  t = 1.0 s: at risk 4, cancelled 1, S = 0.75
  t = 3.0 s: at risk 2, cancelled 1, S = 0.375
  median lifetime: 3.0 s

if B and D were (wrongly) counted as cancels:
  t = 1.0 s: S = 0.75
  t = 2.0 s: S = 0.5
  t = 3.0 s: S = 0.25
  t = 5.0 s: S = 0.0
  median: 2.0 s  (too short: orders look less patient than they are)
```

Now let the real estimator measure the same four orders. `tests/fixtures/calib/km_example/` is a hand-written LOBSTER day containing exactly that story (orders 11 to 14 at 999900):

```sh
cat tests/fixtures/calib/km_example/TEST_2012-06-21_34200000_57600000_message_3.csv
build/release/lobsim_calibrate estimate \
  --messages tests/fixtures/calib/km_example/TEST_2012-06-21_34200000_57600000_message_3.csv \
  --orderbook tests/fixtures/calib/km_example/TEST_2012-06-21_34200000_57600000_orderbook_3.csv \
  --out out/calib_km
python3 -c "import json; print(json.load(open('out/calib_km/TEST_2012-06-21_34200-34206.json'))['lifetime']['1-2'])"
```

When this course was written, the `1-2` bucket printed `times_ns` [1000000000, 3000000000], `survival` [0.75, 0.375], `at_risk` [4, 2], `events` [1, 1], with 1 order censored by execution and 2 at the window end. Which message lines are A, B, C and D? Why are there 5 orders in this bucket, not 4? And why is the bucket `1-2` and not `<=0`?

<details><summary>Answers</summary>

Orders 11 (cancelled at 34201, 1 s), 12 (executed at 34202, 2 s), 13 (cancelled at 34203, 3 s), 14 (still resting at the end). The fifth is order 20, a buy at 999800 submitted at 34205 and still alive at the end (censored at the window end). The bucket: the best bid when they arrived was order 2's 1,000,000, so a buy at 999,900 is (1,000,000 − 999,900) / 100 = 1 tick behind it: Δ = 1 (the tick is 100 units, found as the GCD of the prices). Order 20 at 999,800, arriving when the best bid was 999,900 (order 14), also has Δ = 1.

</details>

## Lab 14.2: placement distances by hand

Best bid 2500, best ask 2504, tick 1 (s = 4). Compute Δ and the spread bucket for: a buy at 2498; a buy at 2502; a sell at 2504; a sell at 2501; a buy at 2504. Which one is excluded, and with what reason?

<details><summary>Answers</summary>

Buy 2498: Δ = 2500 − 2498 = 2. Buy 2502: Δ = −2. Sell 2504: Δ = 2504 − 2504 = 0 (joins the ask). Sell 2501: Δ = 2501 − 2504 = −3 = −(s − 1), the most aggressive allowed. Buy 2504: Δ = −4 < −(s − 1) = −3: it would cross (it reaches the ask), so it is excluded as `would_cross`. All are in the `3+` bucket (s = 4).

</details>

## Lab 14.3: how far apart are two distributions?

```sh
python3 course/labs/py/distances.py
```

Expected:

```
identical:          TV 0.0  W1 0.0
all at 0 vs all at 1:  TV 1.0  W1 1.0
all at 0 vs all at 10: TV 1.0  W1 10.0
  (TV cannot tell 'a little off' from 'far off' once the supports do not overlap; W1 can)
20% moved 3 ticks:    TV 0.200  W1 0.600
20% moved 9 ticks:    TV 0.200  W1 1.800
```

When would you prefer TV, and when W1, to compare a simulated placement distribution with the data?

<details><summary>Answer</summary>

TV judges shape: "is the probability in the right places?", which suits a check like "the simulator draws Δ from the right histogram". W1 is in the variable's own units and grows with how far mass is displaced, which suits questions like "on average, how many ticks off are the simulated orders?". lobsim reports both for distributions.

</details>

## Lab 14.4: a market made only of measured flow

`configs/calibrated.json` runs the hand-made truth through the calibrated agent (no noise traders, no informed trader, no market maker; 10,000 owner ids; zero latency):

```sh
build/release/lobsim_run --config configs/calibrated.json --duration-s 120 --out out/calib_run
```

The `background` lines it prints, when this course was written:

```
background           calibrated flow (hand-made, configs/../calibration/recovery_truth.json), 10000 owner ids
background orders    4849 limit, 466 market (rates 40.0000 and 4.0000 per s configured)
background placement 18 redraws, 0 clamped to the crossing bound, 0 price clamps; fallbacks: 2 one-sided book, 1 empty book
background cancels   4849 scheduled, 0 no-cancel draws, 2535 sent, 0 rejects ignored
```

1. Check the two rates against what was configured.
2. 4,849 cancels were scheduled but only 2,535 sent. Where did the others go?
3. Why are there 2 "one-sided" and 1 "empty book" fallbacks, and only at the very start?

<details><summary>Answers</summary>

1. 4,849 / 120 ≈ 40.4 limit orders per second (40 configured), 466 / 120 ≈ 3.9 market orders per second (4). Poisson counts wobble.
2. Their orders were filled (or the run ended) before the cancel time came. The agent only sends a cancel for an order its own records still show as open.
3. At the start the book is empty, then has one side only, and the agent must place its first orders from the start price.

</details>

## Lab 14.5: parameter recovery, the CI version

```sh
ctest --preset release -R calibration_recovery
cat build/release/calibration_recovery/compare.md
```

The CI version simulates 20 minutes of the truth (not 2 hours), so it uses twice the tolerances. When this course was written it passed in about a second and ended `result: PASS (14 passed, 0 failed, 4 informational, 0 skipped)`. Read the table: which check came closest to its tolerance? What do the `note` column's numbers in the lifetime rows mean?

<details><summary>Answer</summary>

With these numbers, placement sell s=2 (TV 0.0380 against a tolerance of 0.0600, on 2,972 samples). CALIBRATION.md explains that at a few thousand samples, exact sampling alone gives a TV of about 0.03, which is why the full check needs two hours. The notes count, per lifetime bucket, the cancellations (the events) and the censored orders by reason (executed, left the window, alive at the end).

</details>

## Lab 14.6 (optional): the dress rehearsal

```sh
scripts/make_fake_day.sh --out out/synthetic
scripts/run_real_data.sh --calibrate --data-dir out/synthetic/SYNTHA --report out/rehearsal_SYNTHA.md --out out/rehearsal
```

**Always pass `--report`**: without it the script writes its results into `REAL_DATA.md` itself. The first command makes the two synthetic days (about 4 seconds); the second estimates on the first half of SYNTHA, simulates 20 + 20 seeds and fingerprints everything (about 7 seconds with several cores). When this course was written it printed `calibrated verdicts 31 match, 17 close, 4 miss, 1 undefined` and `baseline verdicts 29 match, 12 close, 11 miss, 1 undefined`, the same as CALIBRATION.md. Open `out/rehearsal/calibration/SYNTHA_2012-06-21_34200000_57600000_10/validation.md`, find the four misses of the calibrated flow, and explain each using 14.7.

<details><summary>Answer</summary>

The four are `spread_mean`, `spread_one_tick_share`, `fill_1_2` and `sign_acf_lag100`. The fake day was made by noise traders **plus** a market maker and an informed trader. The market maker's quotes, 2 to 3 ticks wide, shaped the data's spread, which was one tick wide only 1.6% of the time; the calibrated zero-intelligence flow, with no market maker, leaves a one-tick spread 18% of the time and a slightly narrower mean spread. The fill probability of orders 1 to 2 ticks deep depends on who takes liquidity, which also changed. And `sign_acf_lag100`: the informed trader keeps trading in the same direction while the mispricing lasts, which gives the data's trade signs a long memory (0.057 at lag 100), while the calibrated flow draws every sign independently (about 0). CALIBRATION.md's ablation ties the one-tick share to the market maker and the sign memory to the informed trader.

</details>

## Common questions

**Why not just fit the parameters with an optimizer to match spread and volatility?** Because then matching spread and volatility proves little: you tuned for it, and other settings could match just as well. Measuring the inputs directly and checking untouched outputs on unseen data is a much harder test to pass by accident.

**What does "out of sample" protect against?** Overfitting: a model that describes the particular data it was fitted to, noise included, but not the next period. Testing on a later period the estimator never read is the honest check.

**Why a bootstrap instead of a formula for the data's uncertainty?** Market statistics are dependent over time and their distributions are awkward; a bootstrap gets an interval directly from the data. Using blocks keeps the dependence between nearby seconds.

**Why does the calibrated agent need 10,000 owner ids?** It is one agent playing thousands of traders. With few ids, its market orders keep running into its own resting orders and self-trade prevention cuts them short, which distorts the very sizes it was calibrated to reproduce.

**Is the calibrated flow "better" than the noise traders?** On the synthetic days it misses far fewer metrics, especially when the day is busier than the noise traders' fixed settings. It still cannot produce what nobody measured or modelled: sign memory, and the effect of the informed trader and market maker.

## Quiz

1. State the two steps of lobsim's calibration method and why they are kept apart.
2. Best ask 1000, tick 1, spread 2. A sell limit at 999: what is Δ, and is it allowed?
3. Three orders: cancelled at 2 s, executed at 3 s, cancelled at 4 s. Compute Ŝ(2) and Ŝ(4).
4. Why are executed orders censored instead of treated as cancelled?
5. How does `CumulativeTable` draw a value, and why is it exact?
6. What does the leakage check verify, and what happens if it fails?
7. Name one thing the calibrated flow cannot reproduce, and why.

<details><summary>Answers</summary>

1. Fit the inputs directly from data (no tuning), then validate emergent outputs on a different period the estimator never read. Keeping them apart means the validation tests something that was not fitted.
2. Δ = 999 − 1000 = −1 = −(s − 1): allowed, it improves the ask by one tick without crossing.
3. Ŝ(2) = 1 − 1/3 = 2/3. At 4 s only the third order is at risk (n = 1, d = 1): Ŝ(4) = 2/3 × 0 = 0.
4. An executed order was not cancelled, so its lifetime "until cancel" is unknown; we only know it is longer than its time to execution. Treating it as a cancel would make orders look shorter-lived than they are.
5. One `uniform_int(0, total − 1)` and a binary search in the integer cumulative counts. Integers and the project's own unbiased generator make it exact and the same on every platform.
6. That every message the estimator read lies inside the training period. If not, the run stops with a `LeakageError` before any simulation.
7. Trade-sign memory (it draws every side independently), state-dependent rates, or the effects of an informed trader and a market maker, which it does not include.

</details>

## What's next

Module 15 covers what turns a program into a project people can trust: the dashboard that shows a run, the automatic checks that run on every change, and the working habits recorded in `DECISIONS.md`.
