# Calibrating the simulated market to data

lobsim's background traders used to have hand-picked settings. This page describes how they are now **measured** from a LOBSTER day, how the simulator reproduces what was measured, and how the result is **tested on data it was not fitted to**.

> **Data status.** No real LOBSTER files have been run yet (the free samples are no longer available). Everything below that has a number was computed from the two synthetic fake days `SYNTHA` and `SYNTHB` and is labelled synthetic. No number here says anything about a real market. When real files are placed in `data/lobster/`, one command runs the same code on them with no code changes (see [Running it on real data](#12-running-it-on-real-data)).

Contents: [1 Method](#1-method-in-two-steps-and-why) | [2 Definitions](#2-definitions-and-worked-examples) | [3 Estimator](#3-the-estimator) | [4 Calibrated flow](#4-the-calibrated-background-flow) | [5 Parameter recovery](#5-parameter-recovery) | [6 Fingerprint](#6-the-market-fingerprint) | [7 Validation](#7-out-of-sample-validation) | [8 Dress rehearsal](#8-dress-rehearsal-on-synthetic-data-not-real-market-data) | [9 Known gaps](#9-known-gaps) | [10 Out of scope](#10-out-of-scope-and-stretch-goals) | [11 C7](#11-c7-optional-was-not-done) | [12 Real data](#12-running-it-on-real-data)

## 1. Method, in two steps, and why

Many market simulators are "validated" by showing that they produce a few well-known stylized facts, rather than by calibrating them to data. Platt and Gebbie show that this stylized-fact-centric approach is inadequate [4]. Fitting behavioural parameters by matching moments (the method of simulated moments) does not fix this: it finds only an ambiguous link between the data and parameters tied to agent behaviour rules [5]. So the work is split in two, with nothing shared between the steps:

1. **Fit the inputs directly.** Measure the microscopic order flow straight from the data: how often orders arrive, where they are placed, how big they are and how long they live. There is no optimizer and no tuning.
2. **Validate the outputs out of sample.** Simulate with the measured inputs, then compare *emergent* properties that were not fitted (spread, depth, volatility, fill rates) against a **different time period** of the data. The estimator never reads that period, and a test enforces it (section 7).

Why this can work: Farmer, Patelli and Zovko built a zero-intelligence model from directly measured order-flow rates. It explained 96% of the cross-sectional variance of the bid-ask spread and 76% of the price diffusion rate [1]. Expect spreads to match well and volatility less well.

What real data shows, and what the old hand-picked noise trader did instead:

| Old noise trader | What real data shows | Now |
|---|---|---|
| Limit price = mid +- d, d uniform on [1, D] | New limit-order distances from the best quote are very broadly (power-law) distributed [2][3] | Distance sampled from the **measured** distribution |
| Exponential order lifetimes | Lifetimes are not well fit by an exponential [6][7]; on Island ECN 27.7% of limit orders were cancelled within two seconds [8] | Lifetime sampled from the **measured** survival curve |
| Uniform sizes | Sizes cluster at round lots | Sampled from the **measured** size distribution |
| Independent buy/sell signs | Order signs have long memory, with autocorrelation decaying roughly like $\tau^{-0.6}$ [9] | **Not modelled.** Measured and reported as a known gap |
| Constant rates | Event rates depend on the book state in queue-reactive models [10] | **Not modelled.** Reported as a known gap |

References (numbered as in the specification of this work):

1. Farmer, Patelli and Zovko, ["The Predictive Power of Zero Intelligence in Financial Markets"](https://arxiv.org/abs/cond-mat/0309233)
2. Bouchaud, Mezard and Potters, ["Statistical properties of stock order books"](https://arxiv.org/abs/cond-mat/0203511)
3. Potters and Bouchaud, ["More statistical properties of order books and price impact"](https://arxiv.org/abs/cond-mat/0210710)
4. Platt and Gebbie, ["The Problem of Calibrating an Agent-Based Model of High-Frequency Trading"](https://arxiv.org/abs/1606.01495)
5. Platt and Gebbie, ["Can agent-based models probe market microstructure?"](https://econpapers.repec.org/RePEc:eee:phsmap:v:503:y:2018:i:c:p:1092-1106)
6. Chakraborti et al., ["Econophysics: Empirical facts and agent-based models"](https://arxiv.org/pdf/0909.1974)
7. Mike and Farmer, ["An empirical behavioral model of liquidity and volatility"](https://arxiv.org/pdf/0709.0159)
8. Hasbrouck and Saar, ["Technology and liquidity provision: fleeting orders"](https://pages.stern.nyu.edu/~jhasbrou/Research/Working%20Papers/HS6.pdf)
9. Lillo and Farmer, ["The Long Memory of the Efficient Market"](https://arxiv.org/abs/cond-mat/0311053)
10. Huang, Lehalle and Rosenbaum, ["Simulating and analyzing order book data: The queue-reactive model"](https://arxiv.org/abs/1312.0563)
11. ["Simulation and estimation of an agent-based market-model with a matching engine"](https://arxiv.org/pdf/2108.07806)

## 2. Definitions and worked examples

**Placement distance** $\Delta$, in ticks, of a new buy limit order at price $p$ (sells mirror this), measured from the true best bid $b$ just **before** the order arrives:

$$\Delta = \frac{b - p}{\text{tick}}$$

$\Delta = 0$ joins the best bid, $\Delta > 0$ is behind it, $\Delta < 0$ improves it (inside the spread). Real submissions never cross, so $\Delta \ge -(s-1)$ where $s$ is the spread in ticks.

*Worked example.* Best bid $100.00, best ask $100.03, tick $0.01, so $s = 3$. A buy at $99.98 has $\Delta = 2$. A buy at $100.01 has $\Delta = -1$. A buy at $100.02 has $\Delta = -2 = -(s-1)$, the most aggressive price that does not cross.

**Lifetime survival (Kaplan-Meier).** An order's lifetime is the time from submission to cancellation. If it is fully executed first, or its fate becomes unobservable, it is **censored** at that time: we know only that it lived at least that long. Ignoring censoring biases lifetimes. With event times $t_1 < t_2 < \dots$, $d_i$ cancellations at $t_i$ and $n_i$ orders still at risk just before $t_i$:

$$\hat S(t) = \prod_{t_i \le t} \left(1 - \frac{d_i}{n_i}\right)$$

*Worked example.* Four orders. A is cancelled at 1 s, B is executed at 2 s (censored), C is cancelled at 3 s, D is still alive at the end, 5 s (censored). At $t = 1$: $n = 4$, $d = 1$, so $\hat S = 3/4$. At $t = 3$: $n = 2$ (A is gone, B is censored), $d = 1$, so $\hat S = \tfrac34 \cdot \tfrac12 = \tfrac38$.

**Comparing distributions.** For two PMFs on integers with CDFs $F_P$ and $F_Q$, the Wasserstein-1 distance $W_1 = \sum_x |F_P(x) - F_Q(x)|$ is in the variable's units (ticks, shares), and the total-variation distance $TV = \tfrac12 \sum_x |P(x) - Q(x)|$ measures shape.

**Unit tests reproduce both examples exactly.** `Placement.WorkedExampleOfSection13` checks the three $\Delta$ values (2, -1, -2). `KaplanMeier.WorkedExampleOfSection13` checks $\hat S = 3/4$ then $3/8$ (both exact in doubles). `KaplanMeier.CensoredAtAnEventTimeAreStillAtRisk` covers the tie convention. $W_1$ and $TV$ are checked by hand in `CalibDistance.Identical`, `DisjointSupports`, `MixedByHand` and `LongGap` (`tests/unit/calib_file_test.cpp`).

## 3. The estimator

`lobsim_calibrate estimate` measures the order flow of one LOBSTER day inside a window and writes a calibration file. It is an observer attached to the LOBSTER replayer (D-301): it sees each message with the true published book before and after it, and the published window bounds. The file's every field and rule is in [calibration/SCHEMA.md](calibration/SCHEMA.md); the decisions behind the estimator are D-301 to D-319.

```
build/release/lobsim_calibrate estimate --messages <TICKER>_<DATE>_..._message_<L>.csv \
    --orderbook <TICKER>_<DATE>_..._orderbook_<L>.csv [--from S] [--to S] [--tick-units auto|N] \
    [--source real|synthetic] [--out calibration/]
```

It writes `calibration/<TICKER>_<DATE>_<from>-<to>.json` and a short human summary `.md` beside it. The file records every estimate as plain arrays, the sample counts behind each, the SHA-256 of both input files, the window and the code's git commit (D-316). Exit codes: 0, 1 (bad arguments or input that cannot be estimated), 3 (an input file does not exist).

**Window.** `--from` and `--to` are whole seconds after midnight. The default is **10:00 to 15:30** (36,000 to 55,800 s), which leaves out the open and the close, whose behaviour differs. When no bound is given and the file does not cover the default window (the fake days span one hour), the data's whole span is used and `window_note` says so. A given window is clipped to the data and that is noted too; an empty result is an error (D-304). Messages before `--from` are replayed (so the book is right) but not observed. Nothing at or after the window end is read except the one line that shows the bound (D-302).

**Tick.** `--tick-units` is the number of file price units per tick. The default `auto` takes the GCD of the type-1 prices of the window: a multiple of 100 gives 100 (real LOBSTER, $0.01 = 100 units of $0.0001), anything else gives the GCD with a warning. The fake days store prices in simulator ticks, so `auto` gives 1 there. A type-1 price not divisible by the tick is **subpenny**: it is counted and excluded from the rates, sizes, placement and lifetimes (D-305). All distances and spreads are in ticks. The median mid-price (time-weighted, floored to ticks) is stored and becomes the simulator's start price (D-313).

**Rates** (events per second, each split by side). $\lambda_L$ counts type-1 limit submissions by the order's side. $\lambda_M$ counts **bursts** of type-4 executions (the replayer's grouping of executions of one aggressive order), by the side of the aggressor, which is the opposite of the LOBSTER direction (D-307).

**Placement.** The PMF $P(\Delta \mid \text{spread bucket}, \text{side})$ for the buckets $s = 1$, $s = 2$ and $s \ge 3$, over the integers from $-(s-1)$ to the largest observed $\Delta$ (D-306). Orders that cannot be placed are excluded and counted by reason (no previous row, unparsed or malformed or one-sided or crossed previous row, best quote off the tick grid, would cross); such an order is still a submission, so it counts in $\lambda_L$ and in the size PMF. The simulator uses the empirical PMF. For the report only, a power law $P(\Delta) \propto 1/(\Delta_0 + \Delta)^{1+\mu}$ for $\Delta \ge 0$ [2][3] is fitted by discrete maximum likelihood (D-314). On the fake days the fit runs into its bound (the noise trader's uniform distance is not power-law shaped), and the report says so.

**Sizes.** Empirical PMFs, in shares, for limit orders and for the total size of each aggressive burst. Sizes above the 99.9th percentile (nearest rank) are pooled into the top bin, and the number pooled is reported (D-308).

**Lifetimes.** Kaplan-Meier survival (section 2) in distance buckets $\Delta \le 0$, $1$-$2$, $3$-$9$ and $\ge 10$. An order's clock starts at its submission. Its fate is one of:

- **Cancellation (the event):** a full delete, type 3.
- **Censored at full execution.** Partial executions only reduce the order.
- **Censored at the end of the window.**
- **Censored on leaving the published window:** at the first orderbook row where its price is worse than the published L-th level on its side, LOBSTER stops sending its messages, so its fate is unobservable. The order's own submission row counts too (D-310).
- **The fewer-than-L exception:** if a side shows fewer than L levels, the whole side is visible and no order on it can leave the window, so nothing is censored on that side.
- Partial cancels (type 2) do not end a lifetime, except one that takes the remaining size to zero, which is counted and treated as the cancellation (D-309).

Messages naming an order that is not tracked are counted in three classes (after censoring, to an excluded order, unknown id), never silently dropped (D-311). Tests: `Censoring.AnOrderLeavingThePublishedWindowIsCensoredOnThatRow` and `Censoring.ASideShowingFewerThanLLevelsHidesNothing`.

**Excluded orders.** Orders submitted before the window have an unknown start time and are excluded (their later messages count as unknown ids). Also excluded and counted: subpenny orders, orders without a usable placement row (above), and messages whose direction or price disagrees with the tracked order.

**Known filtering bias.** With window-filtered data (LOBSTER's price window, which real files have), submissions placed outside the published window are never seen. So $\lambda_L$ and the far tail of the placement PMF are **underestimated**. Section 5 measures by how much.

## 4. The calibrated background flow

`CalibratedFlowAgent` replaces the whole background (the ten old noise traders) with one agent that samples the measured flow. It is opt-in; the old noise trader stays the default.

**Config.** An optional `background` section (D-322):

```json
"background": {"model": "calibrated", "file": "calibration/SYNTHA_2012-06-21_34200-36000.json",
               "owner_count": 50, "latency_us": 0, "start_price": 9984}
```

`file` is resolved against the config file's directory; `owner_count` defaults to 50, latency to 0, and `start_price` to the calibration's `median_mid_ticks`. Unknown keys are an error, and so is a calibration the agent cannot sample from (for example an empty size PMF with a positive rate), with the field named. **Defaults are off:** without the section nothing changes, `to_json(cfg)` has no `background` key, and every existing config, hash and output is identical. The informed trader and market maker are off in the validation runs because they have no data to be calibrated from; users can switch them back on.

**Sampling rules** (D-324). Limit orders arrive as a Poisson process at $\lambda_L$ with the measured side probabilities. The spread bucket comes from the agent's local view of the book, $\Delta$ is drawn from that bucket's PMF, the price is best $\mp \Delta$ ticks, and the size comes from the limit-size PMF (GTC). Aggressive orders arrive at $\lambda_M$, with measured side probabilities and sizes, as market orders (IOC).

- **Integer cumulative tables.** Each PMF becomes a table of cumulative `uint64` counts. A draw is the project's `uniform_int(0, total - 1)` plus a binary search, which is exact and portable. Totals above $2^{63}$ use raw rejection sampling. Only the project's RNG is used, never a `std::` distribution.
- **Inverse CDF for lifetimes.** At the order's acknowledgment one `uniform01` $u$ is drawn from a separate stream, and the lifetime is the smallest $t$ with $\hat S(t) \le u$ in the order's distance bucket.
- **The no-cancel tail.** If $u$ is below the last survival value (the curve never goes that low) or the curve is empty, no cancel is scheduled: the order lives until it is filled or the session ends. Orders in the tail are counted. Test `CalibratedFlow.NoCancelTailLeavesAboutFortyPercentOfOrdersAlone`.

**Crossing redraws and the clamp.** If $\Delta < -(s-1)$ the placement would cross. The agent redraws, up to 100 times, then uses $-(s-1)$ and counts `placement_clamped`. Tests: `CalibratedFlow.CrossingPlacementsAreRedrawn`, `ClampsToTheCrossingBoundAfter100Redraws`. Prices outside the valid range are moved to it and counted (`price_clamped`).

**Fallbacks when the local book is not two-sided** (D-325). Own side present, other side absent: the reference is the own best, the bucket is `3+`, and nothing can cross (counted `fallback_one_sided`). Own side absent, other side present: the reference is the other best $\mp$ 1 tick, a virtual spread of 1 (bucket `1`, bound 0). Empty book: the reference is the start price, bucket `1`, bound 0 (`fallback_empty`). In addition, when the bucket the agent is in has no samples for that side, the agent uses the **nearest non-empty bucket** (decision D-385, made in parallel with this page; it is what lets a calibration from a day that never visited one spread bucket still run). Tests: `CalibratedFlow.EmptyBookUsesTheStartPriceAndIsCounted`, `OneSidedBookFallbacks`.

**Several owner ids.** One agent sends the whole flow under `owner_count` owner ids (default 50, used in turn), so that self-trade prevention almost never triggers. The simulator gives the agent its own id plus extra ids from $2^{30}$ upwards, so no existing agent's id, event order or hash changes. Private events for an extra id go to the agent, and a cancel carries the owner of the order's placement (D-323). The agent runs after every other agent and has its own random stream role (D-324). The exported order log works with the exporter and the replayer unchanged (`calibrated_flow_closed_loop`).

## 5. Parameter recovery

<!-- C3 RECOVERY SECTION -->

## 6. The market fingerprint

One statistics module (`include/calib/fingerprint.hpp`, CLI `lobsim_fingerprint`) takes LOBSTER-format files plus `tick_units` and computes the scorecard. Simulator runs are exported with `lobsim_export_lobster --lobster-like --filter-window` at the same L as the data, with `--price-scale <tick_units>` so prices are in the data's units, **before** measuring. **Data and simulation therefore go through the same code after the same window filtering**: there are no apples-to-oranges comparisons. The estimator definitions are in [docs/FINGERPRINT.md](docs/FINGERPRINT.md) and D-340 to D-351.

| Metric | Definition |
|---|---|
| Spread distribution | Time-weighted PMF of the spread in ticks; mean, share at one tick, one-sided share |
| Depth profile | Time-weighted mean size at levels 1 to L, per side |
| Volatility | Std of log mid returns on a fixed clock of 1 s, 10 s and 60 s (fixed-clock sampling, because tick-by-tick mid prices proved unreliable for moments in a comparable study [11]) |
| Tails | Excess kurtosis of the 10 s returns |
| Bid-ask bounce | Lag-1 autocorrelation of 1 s returns |
| Volatility clustering | Autocorrelation of $\lvert r \rvert$ (10 s returns), lags 1 to 10 |
| Trade-sign memory | Autocorrelation of aggressive-order signs at lags 1, 10 and 100 (in bursts) [9] |
| Fill probability | Fraction of limit orders at least partly executed, per distance bucket and overall |
| Executed share | Executed shares over submitted shares of those orders |
| Sanity: input rates | $\lambda_L$ and $\lambda_M$ re-measured from the data or simulation |

None of these is fitted by the estimator except the last row, which is a sanity check.

**Autocorrelation definition.** For every series, centre at the mean of the valid entries and take $\rho_k = \dfrac{\text{mean of } c_i c_{i+k} \text{ over the valid pairs}}{\text{mean of } c_i^2}$. The numerator is divided by its own pair count rather than $n$, so a short series is not shrunk towards 0; this differs from the textbook estimator by a factor $n/(n-k)$ (D-344).

**Uncertainty.**
- **Data:** a moving-block bootstrap. The window is cut into one-second bins and resampled in overlapping blocks of 300 bins (5 minutes), 1,000 resamples, a fixed seed; the interval is the 2.5 to 97.5 percentiles. Autocorrelation pairs never join two blocks (D-347, D-348).
- **Simulation:** 20 seeds; per scalar the mean and a 95% t-interval of the mean across seeds (D-349).
- **Distributions** (spread, depth shape): $W_1$ and $TV$ of the simulation against the data, compared with a yardstick, the same distance between the two halves of the data, with its own bootstrap spread (D-350).

## 7. Out-of-sample validation

```
build/release/lobsim_calibrate validate --messages M.csv --orderbook O.csv \
    --train 36000-45900 --test 45900-55800 [--seeds 20] [--out DIR] [--threads N]
```

Steps (D-371 to D-376): estimate from the **train** period; simulate 20 seeds of the calibrated flow, each as long as the **test** period (after a 300 s warm-up that is not measured); simulate 20 seeds of the **old hand-picked noise traders** as a baseline under the same conditions (informed trader and market maker off, same start price, same duration, same export), so the comparison isolates the background-flow model; fingerprint the data's test period and both simulations with the code of section 6. `--calibration FILE` uses an existing calibration instead of estimating (for the cross-ticker check).

**Outputs** (default directory `build/validation/<TICKER>_<DATE>/`, D-381): `validation.md` (one scorecard table: metric, data (test) with its 95% interval, calibrated simulation, baseline simulation, verdict; plus the distance table), `validation.json` (every number, every seed's fingerprint), the calibration file and summary used, and `plots/`. Each page and table title says SYNTHETIC, REAL or hand-made calibration (D-380).

**Verdict rule** (D-377). The data interval is the bootstrap 2.5 to 97.5 percentiles; the simulation interval is the 95% interval of the mean over seeds.
- **match:** the intervals overlap or touch.
- **close:** the gap between them is at most 2 times the data interval's half-width.
- **miss:** otherwise, shown with the ratio simulation/data.
- **n/a:** any of the six numbers is not finite (for example a data value that has no orders behind it).

The baseline is judged by the same rule. Nothing is tuned to improve a verdict.

**Leakage rule** (D-372). The test period is never read by the estimator. After estimation, `check_estimator_range` must show that the estimator's reported observed message range lies inside the training period and does not touch the test period; a missing range is refused too. A failure aborts the run (exit 1) before any simulation. Tests (`tests/unit/calib_validate_test.cpp`): `Leakage.AnObservedRangeInsideTheTrainingPeriodPasses` and `Leakage.AnObservedRangeThatTouchesTheTestPeriodIsRefused`, plus tests in which an estimator stand-in, and the real estimator given a window reaching 30 s into the test period, make `run_validation` throw `LeakageError` with nothing written. With `--calibration FILE` no estimation is done, so the check is reported as not applicable.

**Plots** (`tools/plot_calibration.py`, drawn when `python3` can import matplotlib and numpy; `--no-plots` skips them): spread PMF, depth profile, survival curves and the volatility-against-sampling-interval curve, each with data, calibrated and baseline. In the survival plot the "data" curve is the **training period's** Kaplan-Meier curve, because the estimator never reads the test period; the simulation curves are the estimator run on the exported window of the first seed of each model (D-378).

## 8. Dress rehearsal on synthetic data (NOT real market data)

> **SYNTHETIC.** Everything in this section was computed from the two synthetic LOBSTER-like days `SYNTHA` and `SYNTHB` (`scripts/make_fake_day.sh`, simulator output with made-up tickers). No number here says anything about a real market. Real-data calibration is pending data: nothing has been run on real files.

It exercises the exact code path real data will use: `scripts/run_real_data.sh --calibrate`, which runs `lobsim_calibrate validate` (estimate on the training period, 20 seeds of the calibrated flow and 20 seeds of the old noise traders as a baseline, fingerprint of the test period of the data against both). The days span 34200 to 37800 s, so the script split each span in half and said so: **train 34200-36000 s, test 36000-37800 s**. Release build, 8 threads.

Commands:

```
scripts/make_fake_day.sh --out data/synthetic
scripts/run_real_data.sh --calibrate --data-dir data/synthetic/SYNTHA --report <scratch>.md
scripts/run_real_data.sh --calibrate --data-dir data/synthetic/SYNTHB --report <scratch>.md
# cross-ticker: both days in one folder (hard links), so the script sees two tickers
scripts/run_real_data.sh --calibrate --data-dir data/synthetic_both --report <scratch>.md
```

Wall time of each `validate` run (20 + 20 seeds, 1800 s measured each, includes the 1000-resample bootstrap of the data and the estimator): SYNTHA 12.1 s, SYNTHB 32.2 s when run alone; in the two-ticker run SYNTHA 16.5 s, SYNTHB 21.9 s, cross-ticker SYNTHA to SYNTHB 14.2 s (53 s for the whole script). Times vary with machine load. The runs are deterministic: the two-ticker run reproduced the single-folder `validation.json` byte for byte.

### Why the result is partly circular

The fake days were generated by the old hand-picked noise traders **plus** an informed trader and an Avellaneda-Stoikov market maker (`configs/default.json`; SYNTHB: informed rate x4, noise rate x2). The calibrated flow is estimated from those files, but it replaces the whole background with one zero-intelligence agent and runs **without** the informed trader and the market maker, as the validation specifies. So it should match the spread and depth the noise traders and the market maker produced, and it can miss what the informed trader added: volatility, trade-sign memory. That gap is a useful result: it shows what the background flow alone cannot explain. It is also not a test of calibration against an independent truth, because the data is itself a lobsim run.

### Scorecard: SYNTHA (SYNTHETIC data, NOT real market data)

Verdicts, calibrated simulation: Calibrated: 30 match, 19 close, 3 miss, 1 undefined. Baseline: 29 match, 13 close, 10 miss, 1 undefined.

| metric | data (test) [95% interval] | calibrated sim [95% CI of mean] | baseline sim [95% CI of mean] | verdict |
|---|---|---|---|---|
| `spread_mean` | 2.02 [1.99, 2.049] | 1.91 [1.905, 1.915] | 2.225 [2.22, 2.23]; miss (sim/data = 1.101) | miss (sim/data = 0.9452) |
| `spread_one_tick_share` | 0.01574 [0.001509, 0.03533] | 0.1839 [0.1798, 0.1881] | 0.0003555 [0.000162, 0.000549]; close | miss (sim/data = 11.69) |
| `one_sided_share` | 0 [0, 0] | 0 [0, 0] | 0 [0, 0]; match | match |
| `depth_buy_L1` | 56.83 [40.07, 74.82] | 76.55 [74.14, 78.96] | 66.87 [64.17, 69.58]; match | match |
| `depth_buy_L2` | 177.6 [132.7, 221.9] | 228.7 [223.4, 234] | 216.9 [210.1, 223.6]; match | close |
| `depth_buy_L3` | 279.9 [238.9, 319.1] | 310.4 [306.2, 314.6] | 318.6 [313.3, 323.8]; match | match |
| `depth_buy_L4` | 324.1 [296.3, 346.6] | 338.5 [335.6, 341.5] | 347.9 [345.6, 350.3]; match | match |
| `depth_buy_L5` | 339.7 [325.4, 351] | 343.9 [340.8, 347.1] | 350.5 [348.4, 352.6]; match | match |
| `depth_buy_L6` | 340.9 [332.7, 350.7] | 345.3 [342.7, 347.9] | 351.1 [348.8, 353.3]; match | match |
| `depth_buy_L7` | 340.9 [335.9, 349.2] | 344.9 [342.2, 347.7] | 351.3 [348.8, 353.8]; match | match |
| `depth_buy_L8` | 332.4 [329.4, 339.6] | 334.5 [331.8, 337.2] | 348.7 [345.6, 351.7]; close | match |
| `depth_buy_L9` | 311 [297.5, 328.7] | 295.8 [291.4, 300.2] | 320 [316, 324]; match | match |
| `depth_buy_L10` | 249.4 [205.5, 292.7] | 196.4 [191.4, 201.4] | 216.4 [210.7, 222]; match | close |
| `depth_sell_L1` | 78.7 [56.9, 101.7] | 51.67 [50.34, 52.99] | 67.26 [64.38, 70.15]; match | close |
| `depth_sell_L2` | 223.3 [179.8, 269.6] | 168 [164.8, 171.2] | 215.9 [210.8, 221]; match | close |
| `depth_sell_L3` | 308.9 [282.3, 335.8] | 263.6 [260, 267.2] | 316.3 [312.8, 319.9]; match | close |
| `depth_sell_L4` | 337 [327.7, 348.2] | 315.1 [312.3, 317.9] | 347.8 [344.8, 350.8]; match | close |
| `depth_sell_L5` | 343.1 [333.4, 349] | 335.8 [333.5, 338.1] | 351.3 [348.8, 353.7]; match | match |
| `depth_sell_L6` | 343.7 [336.8, 350.1] | 345.3 [342.7, 347.8] | 351.4 [348.5, 354.2]; match | match |
| `depth_sell_L7` | 336.1 [330.3, 342] | 346.8 [345, 348.7] | 350.3 [347.4, 353.2]; close | close |
| `depth_sell_L8` | 324 [307.4, 343.1] | 343.7 [341.7, 345.7] | 348.4 [345.1, 351.8]; close | match |
| `depth_sell_L9` | 285.4 [249.6, 324] | 324.5 [321.5, 327.5] | 321 [315.8, 326.2]; match | match |
| `depth_sell_L10` | 200.6 [152, 255.7] | 250.2 [246.2, 254.1] | 217.2 [210.2, 224.1]; match | match |
| `vol_1s` | 6.179e-05 [5.826e-05, 6.571e-05] | 6.728e-05 [6.654e-05, 6.803e-05] | 4.953e-05 [4.91e-05, 4.996e-05]; miss (sim/data = 0.8016) | close |
| `vol_10s` | 8.479e-05 [7.437e-05, 9.027e-05] | 0.0001013 [9.916e-05, 0.0001035] | 8.234e-05 [8.018e-05, 8.449e-05]; match | close |
| `vol_60s` | 0.0001904 [0.0001326, 0.0002237] | 0.0001266 [0.0001162, 0.000137] | 0.0001173 [0.0001101, 0.0001245]; close | match |
| `kurtosis_10s` | -0.2402 [-0.795, 0.2264] | -0.3222 [-0.4207, -0.2238] | -0.2423 [-0.3627, -0.1219]; match | match |
| `acf_ret_1s_lag1` | -0.39 [-0.4237, -0.3496] | -0.3221 [-0.3318, -0.3124] | -0.2625 [-0.2751, -0.2499]; miss (sim/data = 0.6731) | close |
| `acf_abs_10s_lag1` | -0.04674 [-0.2069, 0.02653] | 0.07085 [0.02222, 0.1195] | 0.02087 [-0.01262, 0.05436]; match | match |
| `acf_abs_10s_lag2` | 0.03943 [-0.03946, 0.1491] | 0.02069 [-0.03084, 0.07223] | -0.021 [-0.05789, 0.01589]; match | match |
| `acf_abs_10s_lag3` | -0.04422 [-0.1161, 0.1202] | 0.01168 [-0.02077, 0.04412] | -0.02421 [-0.05774, 0.009326]; match | match |
| `acf_abs_10s_lag4` | -0.08117 [-0.302, 0.06674] | -0.01718 [-0.05073, 0.01636] | -0.005434 [-0.04088, 0.03002]; match | match |
| `acf_abs_10s_lag5` | 0.1816 [0.0637, 0.271] | 0.01021 [-0.02069, 0.0411] | -0.007598 [-0.03879, 0.0236]; close | close |
| `acf_abs_10s_lag6` | 0.04174 [-0.1625, 0.129] | -0.005871 [-0.04748, 0.03574] | -0.01486 [-0.04801, 0.01829]; match | match |
| `acf_abs_10s_lag7` | -0.09412 [-0.2547, 0.03904] | -0.01222 [-0.04761, 0.02317] | 0.01156 [-0.0185, 0.04163]; match | match |
| `acf_abs_10s_lag8` | -0.1578 [-0.3924, 0.06801] | 0.02965 [-0.005476, 0.06478] | -0.01428 [-0.04298, 0.01443]; match | match |
| `acf_abs_10s_lag9` | -0.1147 [-0.2718, 0.1193] | 0.001222 [-0.03136, 0.03381] | -0.01494 [-0.05517, 0.02528]; match | match |
| `acf_abs_10s_lag10` | 0.06443 [-0.08972, 0.2156] | 0.01412 [-0.02607, 0.05431] | 0.02646 [-0.0006733, 0.0536]; match | match |
| `sign_acf_lag1` | 0.04395 [0.02664, 0.06279] | 0.0003783 [-0.004642, 0.005398] | -0.002515 [-0.006451, 0.001422]; close | close |
| `sign_acf_lag10` | 0.03483 [0.01645, 0.04239] | 0.0005669 [-0.0042, 0.005333] | -0.002394 [-0.005772, 0.0009836]; close | close |
| `sign_acf_lag100` | 0.05712 [0.04184, 0.08096] | 0.0007156 [-0.0037, 0.005131] | 0.00245 [-0.001723, 0.006623]; close | close |
| `fill_le0` | 0.8855 [0.8711, 0.8965] | 0.6328 [0.6301, 0.6354] | 0.8599 [0.8574, 0.8624]; close | miss (sim/data = 0.7146) |
| `fill_1_2` | 0.3107 [0.2892, 0.3355] | 0.269 [0.2668, 0.2711] | 0.232 [0.2287, 0.2354]; miss (sim/data = 0.7468) | close |
| `fill_3_9` | 0.01372 [0.007527, 0.02011] | 0.005659 [0.005332, 0.005987] | 0.001511 [0.001285, 0.001737]; close | close |
| `fill_ge10` | n/a [n/a] | 0 [0, 0] | n/a [n/a]; n/a | n/a |
| `fill_all` | 0.1831 [0.177, 0.1889] | 0.1655 [0.1646, 0.1664] | 0.1509 [0.1502, 0.1516]; miss (sim/data = 0.8241) | close |
| `executed_share` | 0.1809 [0.175, 0.187] | 0.1632 [0.1624, 0.1639] | 0.1495 [0.1488, 0.1501]; miss (sim/data = 0.8264) | close |
| `lambda_limit_buy` | 23.57 [23.17, 23.98] | 24.04 [23.98, 24.09] | 21.23 [21.18, 21.28]; miss (sim/data = 0.9008) | close |
| `lambda_limit_sell` | 23.47 [22.99, 23.96] | 23.86 [23.81, 23.91] | 21.25 [21.2, 21.3]; miss (sim/data = 0.9056) | match |
| `lambda_limit_total` | 47.03 [46.27, 47.88] | 47.89 [47.83, 47.95] | 42.48 [42.42, 42.54]; miss (sim/data = 0.9032) | match |
| `lambda_market_buy` | 4.437 [3.868, 5.001] | 4.002 [3.98, 4.023] | 3.371 [3.351, 3.391]; close | match |
| `lambda_market_sell` | 4.024 [3.67, 4.472] | 4.299 [4.284, 4.314] | 3.363 [3.343, 3.384]; close | match |
| `lambda_market_total` | 8.462 [8.176, 8.695] | 8.301 [8.273, 8.328] | 6.734 [6.708, 6.761]; miss (sim/data = 0.7959) | match |

### Scorecard: SYNTHB (SYNTHETIC data, NOT real market data)

Verdicts, calibrated simulation: Calibrated: 31 match, 16 close, 5 miss, 1 undefined. Baseline: 16 match, 10 close, 26 miss, 1 undefined.

| metric | data (test) [95% interval] | calibrated sim [95% CI of mean] | baseline sim [95% CI of mean] | verdict |
|---|---|---|---|---|
| `spread_mean` | 2.032 [2.001, 2.059] | 1.958 [1.954, 1.962] | 2.225 [2.22, 2.23]; miss (sim/data = 1.095) | close |
| `spread_one_tick_share` | 0.01287 [0.001989, 0.03156] | 0.1611 [0.1584, 0.1639] | 0.0003555 [0.000162, 0.000549]; close | miss (sim/data = 12.52) |
| `one_sided_share` | 0 [0, 0] | 0 [0, 0] | 0 [0, 0]; match | match |
| `depth_buy_L1` | 116.9 [60.54, 168.6] | 95.62 [93.09, 98.14] | 66.87 [64.17, 69.58]; match | match |
| `depth_buy_L2` | 339.9 [204.1, 469.7] | 344 [337.6, 350.4] | 216.9 [210.1, 223.6]; match | match |
| `depth_buy_L3` | 500.3 [378, 618.9] | 552.7 [546.5, 558.9] | 318.6 [313.3, 323.8]; close | match |
| `depth_buy_L4` | 582 [505.9, 656.5] | 652.3 [648, 656.7] | 347.9 [345.6, 350.3]; miss (sim/data = 0.5979) | match |
| `depth_buy_L5` | 633.7 [590.4, 681.3] | 679.5 [676.4, 682.5] | 350.5 [348.4, 352.6]; miss (sim/data = 0.5531) | match |
| `depth_buy_L6` | 655.8 [638.7, 684] | 685.8 [682.9, 688.8] | 351.1 [348.8, 353.3]; miss (sim/data = 0.5353) | match |
| `depth_buy_L7` | 655.2 [642.6, 676.8] | 685.1 [682, 688.2] | 351.3 [348.8, 353.8]; miss (sim/data = 0.5362) | close |
| `depth_buy_L8` | 646.4 [625.6, 674.9] | 674.8 [671.2, 678.5] | 348.7 [345.6, 351.7]; miss (sim/data = 0.5394) | match |
| `depth_buy_L9` | 594 [535.4, 660] | 615.6 [611.2, 620.1] | 320 [316, 324]; miss (sim/data = 0.5388) | match |
| `depth_buy_L10` | 478.4 [355.9, 604.1] | 447.9 [440.7, 455] | 216.4 [210.7, 222]; close | match |
| `depth_sell_L1` | 165.9 [97.4, 237.9] | 103.9 [100, 107.7] | 67.26 [64.38, 70.15]; close | match |
| `depth_sell_L2` | 442.2 [314.1, 578.6] | 359.6 [352, 367.1] | 215.9 [210.8, 221]; close | match |
| `depth_sell_L3` | 572.4 [494.5, 658] | 562.3 [556.2, 568.5] | 316.3 [312.8, 319.9]; miss (sim/data = 0.5526) | match |
| `depth_sell_L4` | 638.5 [610.6, 675.4] | 652.4 [647.7, 657] | 347.8 [344.8, 350.8]; miss (sim/data = 0.5447) | match |
| `depth_sell_L5` | 657.5 [647.1, 674] | 678 [674.4, 681.6] | 351.3 [348.8, 353.7]; miss (sim/data = 0.5342) | close |
| `depth_sell_L6` | 655.7 [630.4, 682.7] | 686.7 [682.9, 690.5] | 351.4 [348.5, 354.2]; miss (sim/data = 0.5359) | close |
| `depth_sell_L7` | 627.8 [588.9, 668.5] | 687.9 [683.9, 691.9] | 350.3 [347.4, 353.2]; miss (sim/data = 0.558) | close |
| `depth_sell_L8` | 583.8 [515.8, 647.9] | 672.9 [667.9, 677.9] | 348.4 [345.1, 351.8]; miss (sim/data = 0.5968) | close |
| `depth_sell_L9` | 509.1 [392, 619.5] | 594.7 [588.1, 601.3] | 321 [315.8, 326.2]; close | match |
| `depth_sell_L10` | 373.4 [218.9, 525.7] | 409.6 [402.7, 416.6] | 217.2 [210.2, 224.1]; match | match |
| `vol_1s` | 5.976e-05 [5.741e-05, 6.094e-05] | 8.007e-05 [7.95e-05, 8.064e-05] | 4.932e-05 [4.889e-05, 4.975e-05]; miss (sim/data = 0.8252) | miss (sim/data = 1.34) |
| `vol_10s` | 9.242e-05 [8.012e-05, 9.421e-05] | 0.0001082 [0.0001056, 0.0001109] | 8.198e-05 [7.984e-05, 8.413e-05]; match | close |
| `vol_60s` | 0.0003196 [0.0001871, 0.0003429] | 0.0001301 [0.0001221, 0.0001382] | 0.0001168 [0.0001097, 0.0001239]; close | close |
| `kurtosis_10s` | -0.6296 [-0.9634, 0.3695] | -0.3364 [-0.4246, -0.2482] | -0.2423 [-0.3627, -0.1219]; match | match |
| `acf_ret_1s_lag1` | -0.354 [-0.3873, -0.3137] | -0.3824 [-0.3897, -0.3751] | -0.2625 [-0.2751, -0.2499]; close | match |
| `acf_abs_10s_lag1` | -0.08165 [-0.2292, -0.007125] | 0.1003 [0.06622, 0.1344] | 0.02087 [-0.01262, 0.05436]; match | close |
| `acf_abs_10s_lag2` | -0.04553 [-0.2113, -0.002475] | -0.02213 [-0.04747, 0.003209] | -0.021 [-0.05789, 0.01589]; match | match |
| `acf_abs_10s_lag3` | 0.01444 [-0.1203, 0.1793] | -0.0004596 [-0.03776, 0.03684] | -0.02421 [-0.05774, 0.009326]; match | match |
| `acf_abs_10s_lag4` | -0.04962 [-0.2251, 0.1118] | -0.01166 [-0.04922, 0.02589] | -0.005434 [-0.04088, 0.03002]; match | match |
| `acf_abs_10s_lag5` | 0.06373 [-0.07189, 0.2811] | -0.007973 [-0.04999, 0.03405] | -0.007598 [-0.03879, 0.0236]; match | match |
| `acf_abs_10s_lag6` | -0.1291 [-0.2938, 0.03859] | 0.003501 [-0.02419, 0.03119] | -0.01486 [-0.04801, 0.01829]; match | match |
| `acf_abs_10s_lag7` | -0.0379 [-0.1813, 0.1073] | 0.0166 [-0.02347, 0.05668] | 0.01156 [-0.0185, 0.04163]; match | match |
| `acf_abs_10s_lag8` | 0.01928 [-0.1445, 0.1524] | -0.001312 [-0.03329, 0.03067] | -0.01428 [-0.04298, 0.01443]; match | match |
| `acf_abs_10s_lag9` | 0.03944 [-0.09029, 0.2006] | -0.02824 [-0.06337, 0.006882] | -0.01494 [-0.05517, 0.02528]; match | match |
| `acf_abs_10s_lag10` | -0.05842 [-0.2643, 0.1295] | 0.01379 [-0.02497, 0.05255] | 0.02646 [-0.0006733, 0.0536]; match | match |
| `sign_acf_lag1` | 0.07836 [0.0611, 0.0908] | -0.0009344 [-0.002381, 0.0005125] | -0.002515 [-0.006451, 0.001422]; miss (sim/data = -0.03209) | miss (sim/data = -0.01192) |
| `sign_acf_lag10` | 0.07938 [0.0488, 0.09942] | -0.001375 [-0.003842, 0.001091] | -0.002394 [-0.005772, 0.0009836]; close | close |
| `sign_acf_lag100` | 0.08269 [0.06219, 0.09778] | 0.001336 [-0.001224, 0.003896] | 0.00245 [-0.001723, 0.006623]; miss (sim/data = 0.02963) | miss (sim/data = 0.01616) |
| `fill_le0` | 0.8974 [0.8867, 0.9034] | 0.6695 [0.6677, 0.6713] | 0.8599 [0.8574, 0.8624]; miss (sim/data = 0.9582) | miss (sim/data = 0.746) |
| `fill_1_2` | 0.3391 [0.3275, 0.3558] | 0.3102 [0.3087, 0.3116] | 0.232 [0.2287, 0.2354]; miss (sim/data = 0.6843) | close |
| `fill_3_9` | 0.03646 [0.02434, 0.05136] | 0.005607 [0.005343, 0.005871] | 0.001511 [0.001285, 0.001737]; close | close |
| `fill_ge10` | n/a [n/a] | 0.25 [-0.5455, 1.046] | n/a [n/a]; n/a | n/a |
| `fill_all` | 0.207 [0.1926, 0.221] | 0.1861 [0.1854, 0.1867] | 0.1509 [0.1502, 0.1516]; miss (sim/data = 0.729) | close |
| `executed_share` | 0.2054 [0.1915, 0.2193] | 0.1845 [0.1839, 0.1851] | 0.1495 [0.1488, 0.1501]; miss (sim/data = 0.7276) | close |
| `lambda_limit_buy` | 47.06 [46.01, 47.75] | 47.76 [47.7, 47.81] | 21.23 [21.18, 21.28]; miss (sim/data = 0.4511) | match |
| `lambda_limit_sell` | 46.85 [46.06, 47.39] | 47.64 [47.57, 47.71] | 21.25 [21.2, 21.3]; miss (sim/data = 0.4536) | close |
| `lambda_limit_total` | 93.91 [92.13, 95.03] | 95.39 [95.32, 95.47] | 42.48 [42.42, 42.54]; miss (sim/data = 0.4524) | close |
| `lambda_market_buy` | 10.8 [8.366, 13.27] | 9.664 [9.632, 9.696] | 3.371 [3.351, 3.391]; miss (sim/data = 0.312) | match |
| `lambda_market_sell` | 8.552 [7.149, 9.604] | 9.084 [9.049, 9.12] | 3.363 [3.343, 3.384]; miss (sim/data = 0.3933) | match |
| `lambda_market_total` | 19.36 [17.77, 20.6] | 18.75 [18.71, 18.79] | 6.734 [6.708, 6.761]; miss (sim/data = 0.3479) | match |

### Cross-ticker scorecard: calibrated on SYNTHA, validated on SYNTHB (SYNTHETIC data, NOT real market data)

Verdicts, calibrated simulation: Calibrated: 15 match, 13 close, 24 miss, 1 undefined. Baseline: 16 match, 10 close, 26 miss, 1 undefined.

| metric | data (test) [95% interval] | calibrated sim [95% CI of mean] | baseline sim [95% CI of mean] | verdict |
|---|---|---|---|---|
| `spread_mean` | 2.032 [2.001, 2.059] | 1.91 [1.905, 1.915] | 2.225 [2.22, 2.23]; miss (sim/data = 1.095) | miss (sim/data = 0.94) |
| `spread_one_tick_share` | 0.01287 [0.001989, 0.03156] | 0.1839 [0.1798, 0.1881] | 0.0003555 [0.000162, 0.000549]; close | miss (sim/data = 14.29) |
| `one_sided_share` | 0 [0, 0] | 0 [0, 0] | 0 [0, 0]; match | match |
| `depth_buy_L1` | 116.9 [60.54, 168.6] | 76.55 [74.14, 78.96] | 66.87 [64.17, 69.58]; match | match |
| `depth_buy_L2` | 339.9 [204.1, 469.7] | 228.7 [223.4, 234] | 216.9 [210.1, 223.6]; match | match |
| `depth_buy_L3` | 500.3 [378, 618.9] | 310.4 [306.2, 314.6] | 318.6 [313.3, 323.8]; close | close |
| `depth_buy_L4` | 582 [505.9, 656.5] | 338.5 [335.6, 341.5] | 347.9 [345.6, 350.3]; miss (sim/data = 0.5979) | miss (sim/data = 0.5817) |
| `depth_buy_L5` | 633.7 [590.4, 681.3] | 343.9 [340.8, 347.1] | 350.5 [348.4, 352.6]; miss (sim/data = 0.5531) | miss (sim/data = 0.5427) |
| `depth_buy_L6` | 655.8 [638.7, 684] | 345.3 [342.7, 347.9] | 351.1 [348.8, 353.3]; miss (sim/data = 0.5353) | miss (sim/data = 0.5266) |
| `depth_buy_L7` | 655.2 [642.6, 676.8] | 344.9 [342.2, 347.7] | 351.3 [348.8, 353.8]; miss (sim/data = 0.5362) | miss (sim/data = 0.5264) |
| `depth_buy_L8` | 646.4 [625.6, 674.9] | 334.5 [331.8, 337.2] | 348.7 [345.6, 351.7]; miss (sim/data = 0.5394) | miss (sim/data = 0.5175) |
| `depth_buy_L9` | 594 [535.4, 660] | 295.8 [291.4, 300.2] | 320 [316, 324]; miss (sim/data = 0.5388) | miss (sim/data = 0.498) |
| `depth_buy_L10` | 478.4 [355.9, 604.1] | 196.4 [191.4, 201.4] | 216.4 [210.7, 222]; close | close |
| `depth_sell_L1` | 165.9 [97.4, 237.9] | 51.67 [50.34, 52.99] | 67.26 [64.38, 70.15]; close | close |
| `depth_sell_L2` | 442.2 [314.1, 578.6] | 168 [164.8, 171.2] | 215.9 [210.8, 221]; close | close |
| `depth_sell_L3` | 572.4 [494.5, 658] | 263.6 [260, 267.2] | 316.3 [312.8, 319.9]; miss (sim/data = 0.5526) | miss (sim/data = 0.4605) |
| `depth_sell_L4` | 638.5 [610.6, 675.4] | 315.1 [312.3, 317.9] | 347.8 [344.8, 350.8]; miss (sim/data = 0.5447) | miss (sim/data = 0.4935) |
| `depth_sell_L5` | 657.5 [647.1, 674] | 335.8 [333.5, 338.1] | 351.3 [348.8, 353.7]; miss (sim/data = 0.5342) | miss (sim/data = 0.5107) |
| `depth_sell_L6` | 655.7 [630.4, 682.7] | 345.3 [342.7, 347.8] | 351.4 [348.5, 354.2]; miss (sim/data = 0.5359) | miss (sim/data = 0.5266) |
| `depth_sell_L7` | 627.8 [588.9, 668.5] | 346.8 [345, 348.7] | 350.3 [347.4, 353.2]; miss (sim/data = 0.558) | miss (sim/data = 0.5525) |
| `depth_sell_L8` | 583.8 [515.8, 647.9] | 343.7 [341.7, 345.7] | 348.4 [345.1, 351.8]; miss (sim/data = 0.5968) | miss (sim/data = 0.5887) |
| `depth_sell_L9` | 509.1 [392, 619.5] | 324.5 [321.5, 327.5] | 321 [315.8, 326.2]; close | close |
| `depth_sell_L10` | 373.4 [218.9, 525.7] | 250.2 [246.2, 254.1] | 217.2 [210.2, 224.1]; match | match |
| `vol_1s` | 5.976e-05 [5.741e-05, 6.094e-05] | 6.699e-05 [6.625e-05, 6.774e-05] | 4.932e-05 [4.889e-05, 4.975e-05]; miss (sim/data = 0.8252) | miss (sim/data = 1.121) |
| `vol_10s` | 9.242e-05 [8.012e-05, 9.421e-05] | 0.0001009 [9.873e-05, 0.0001031] | 8.198e-05 [7.984e-05, 8.413e-05]; match | close |
| `vol_60s` | 0.0003196 [0.0001871, 0.0003429] | 0.0001261 [0.0001157, 0.0001364] | 0.0001168 [0.0001097, 0.0001239]; close | close |
| `kurtosis_10s` | -0.6296 [-0.9634, 0.3695] | -0.3222 [-0.4207, -0.2238] | -0.2423 [-0.3627, -0.1219]; match | match |
| `acf_ret_1s_lag1` | -0.354 [-0.3873, -0.3137] | -0.3221 [-0.3318, -0.3124] | -0.2625 [-0.2751, -0.2499]; close | match |
| `acf_abs_10s_lag1` | -0.08165 [-0.2292, -0.007125] | 0.07085 [0.02222, 0.1195] | 0.02087 [-0.01262, 0.05436]; match | close |
| `acf_abs_10s_lag2` | -0.04553 [-0.2113, -0.002475] | 0.02069 [-0.03084, 0.07223] | -0.021 [-0.05789, 0.01589]; match | match |
| `acf_abs_10s_lag3` | 0.01444 [-0.1203, 0.1793] | 0.01168 [-0.02077, 0.04412] | -0.02421 [-0.05774, 0.009326]; match | match |
| `acf_abs_10s_lag4` | -0.04962 [-0.2251, 0.1118] | -0.01718 [-0.05073, 0.01636] | -0.005434 [-0.04088, 0.03002]; match | match |
| `acf_abs_10s_lag5` | 0.06373 [-0.07189, 0.2811] | 0.01021 [-0.02069, 0.0411] | -0.007598 [-0.03879, 0.0236]; match | match |
| `acf_abs_10s_lag6` | -0.1291 [-0.2938, 0.03859] | -0.005871 [-0.04748, 0.03574] | -0.01486 [-0.04801, 0.01829]; match | match |
| `acf_abs_10s_lag7` | -0.0379 [-0.1813, 0.1073] | -0.01222 [-0.04761, 0.02317] | 0.01156 [-0.0185, 0.04163]; match | match |
| `acf_abs_10s_lag8` | 0.01928 [-0.1445, 0.1524] | 0.02965 [-0.005476, 0.06478] | -0.01428 [-0.04298, 0.01443]; match | match |
| `acf_abs_10s_lag9` | 0.03944 [-0.09029, 0.2006] | 0.001223 [-0.03136, 0.03381] | -0.01494 [-0.05517, 0.02528]; match | match |
| `acf_abs_10s_lag10` | -0.05842 [-0.2643, 0.1295] | 0.01412 [-0.02607, 0.05431] | 0.02646 [-0.0006733, 0.0536]; match | match |
| `sign_acf_lag1` | 0.07836 [0.0611, 0.0908] | 0.0003783 [-0.004642, 0.005398] | -0.002515 [-0.006451, 0.001422]; miss (sim/data = -0.03209) | miss (sim/data = 0.004828) |
| `sign_acf_lag10` | 0.07938 [0.0488, 0.09942] | 0.0005669 [-0.0042, 0.005333] | -0.002394 [-0.005772, 0.0009836]; close | close |
| `sign_acf_lag100` | 0.08269 [0.06219, 0.09778] | 0.0007156 [-0.0037, 0.005131] | 0.00245 [-0.001723, 0.006623]; miss (sim/data = 0.02963) | miss (sim/data = 0.008654) |
| `fill_le0` | 0.8974 [0.8867, 0.9034] | 0.6328 [0.6301, 0.6354] | 0.8599 [0.8574, 0.8624]; miss (sim/data = 0.9582) | miss (sim/data = 0.7051) |
| `fill_1_2` | 0.3391 [0.3275, 0.3558] | 0.269 [0.2668, 0.2711] | 0.232 [0.2287, 0.2354]; miss (sim/data = 0.6843) | miss (sim/data = 0.7932) |
| `fill_3_9` | 0.03646 [0.02434, 0.05136] | 0.005659 [0.005332, 0.005987] | 0.001511 [0.001285, 0.001737]; close | close |
| `fill_ge10` | n/a [n/a] | 0 [0, 0] | n/a [n/a]; n/a | n/a |
| `fill_all` | 0.207 [0.1926, 0.221] | 0.1655 [0.1646, 0.1664] | 0.1509 [0.1502, 0.1516]; miss (sim/data = 0.729) | close |
| `executed_share` | 0.2054 [0.1915, 0.2193] | 0.1632 [0.1624, 0.1639] | 0.1495 [0.1488, 0.1501]; miss (sim/data = 0.7276) | close |
| `lambda_limit_buy` | 47.06 [46.01, 47.75] | 24.04 [23.98, 24.09] | 21.23 [21.18, 21.28]; miss (sim/data = 0.4511) | miss (sim/data = 0.5107) |
| `lambda_limit_sell` | 46.85 [46.06, 47.39] | 23.86 [23.81, 23.91] | 21.25 [21.2, 21.3]; miss (sim/data = 0.4536) | miss (sim/data = 0.5092) |
| `lambda_limit_total` | 93.91 [92.13, 95.03] | 47.89 [47.83, 47.95] | 42.48 [42.42, 42.54]; miss (sim/data = 0.4524) | miss (sim/data = 0.51) |
| `lambda_market_buy` | 10.8 [8.366, 13.27] | 4.002 [3.98, 4.023] | 3.371 [3.351, 3.391]; miss (sim/data = 0.312) | close |
| `lambda_market_sell` | 8.552 [7.149, 9.604] | 4.299 [4.284, 4.314] | 3.363 [3.343, 3.384]; miss (sim/data = 0.3933) | miss (sim/data = 0.5027) |
| `lambda_market_total` | 19.36 [17.77, 20.6] | 8.301 [8.273, 8.328] | 6.734 [6.708, 6.761]; miss (sim/data = 0.3479) | miss (sim/data = 0.4288) |

The cross-ticker run uses the calibration estimated on SYNTHA's training half and starts the simulation at SYNTHB's median mid-price (10021 ticks); the yardstick is SYNTHB's test half.

### Distributions against the data's own halves (SYNTHETIC)

| run | quantity | data halves | halves, bootstrap upper | calibrated | baseline |
|---|---|---|---|---|---|
| SYNTHA | spread W1 (ticks) | 0.0713 | 0.09685 | 0.2257 | 0.2047 |
| SYNTHA | depth buy TV | 0.06724 | 0.1212 | 0.03534 | 0.02557 |
| SYNTHA | depth sell TV | 0.06547 | 0.1299 | 0.05147 | 0.01899 |
| SYNTHB | spread W1 (ticks) | 0.06423 | 0.0841 | 0.223 | 0.1933 |
| SYNTHB | depth buy TV | 0.148 | 0.1907 | 0.01916 | 0.03432 |
| SYNTHB | depth sell TV | 0.156 | 0.1969 | 0.03883 | 0.02997 |
| SYNTHA to SYNTHB | spread W1 (ticks) | 0.06423 | 0.0841 | 0.2201 | 0.1933 |
| SYNTHA to SYNTHB | depth buy TV | 0.148 | 0.1907 | 0.04406 | 0.03432 |
| SYNTHA to SYNTHB | depth sell TV | 0.156 | 0.1969 | 0.06119 | 0.02997 |

### What matches, what is close, what misses

Counts (calibrated flow; baseline in brackets): SYNTHA 30 match, 19 close, 3 miss, 1 undefined (29, 13, 10); SYNTHB 31 match, 16 close, 5 miss, 1 undefined (16, 10, 26); cross-ticker 15 match, 13 close, 24 miss, 1 undefined (16, 10, 26).

The undefined row in every table is `fill_ge10`: no limit order in the data was placed 10 or more ticks from the touch inside the window, so the data has no value for it.

**SYNTHA, matches.** Depth at every level on both sides matches or is close (the normalised depth profile is within the halves yardstick, as for the baseline). The lifetime curves are reproduced: median lifetime by distance bucket, training data / calibrated / baseline, in seconds: <=0 8.21 / 8.48 / 22.08; 1-2 13.54 / 13.29 / 21.14; 3-9 20.86 / 20.75 / 20.95 (the baseline's exponential lifetimes give about 21 s everywhere). Volatility at 10 s matches; kurtosis, the lag-1 bounce and every autocorrelation of |r| match.

**SYNTHA, close.** `vol_1s`, `vol_60s`, the sign autocorrelations, most fill probabilities, the order rates: the gaps are within twice the data interval's half-width. The trade-sign autocorrelation is small in the data (0.04395 at lag 1, from the informed trader) and about 0 in the simulation; it reads "close" only because the data interval is wide, not because the flow has sign memory. It is an expected miss by construction.

**SYNTHA, misses.** (1) `spread_mean`: data 2.02 ticks, calibrated 1.91 (ratio 0.9452). The baseline misses in the other direction (2.225). (2) `spread_one_tick_share`: data 0.01574 of two-sided time at one tick, calibrated 0.1839. The likely cause (not isolated by a separate run): in the data the market maker quotes keep the spread at two ticks or more, while the calibrated flow has no market maker and its orders improve the touch (negative placement distances, measured from the data), so the spread closes to one tick far more often. The spread PMF is beyond the halves yardstick for both models (W1 0.2257 calibrated, 0.2047 baseline, against an upper bound of 0.09685). (3) `fill_le0`: 0.8855 of the data's orders placed at or inside the touch were filled, 0.6328 in the calibrated simulation. We expect, but did not test, that the market maker's quotes, which sit at the touch and are hit by market orders, make up much of that difference.

**SYNTHB** (stronger informed trader). Same pattern for spread and one-tick share. In addition the informed trader's footprint shows: `sign_acf_lag1` is 0.07836 in the data and -0.0009344 in the calibrated simulation (miss), `sign_acf_lag100` 0.08269 against 0.001336 (miss), and `vol_1s` is 5.976e-05 in the data against 8.007e-05. These are the gaps the background flow alone cannot explain, and they are the expected ones. The baseline misses far more rows on SYNTHB (26 misses against 5), because the old noise traders' rates and lifetimes do not match this day, while the calibrated flow follows the measured rates.

**Cross-ticker, SYNTHA calibration on SYNTHB.** SYNTHB trades about twice as fast as SYNTHA (limit rate 93.91 against 47.89 per second in the simulation), so the rate rows miss by a factor of about two (`lambda_limit_total` ratio 0.51), and the deeper levels of the book are about half as full (`depth_buy_L6` ratio 0.5266). The spread rows miss as in the same-ticker run. This is the expected outcome of a calibration that does not carry over to a faster market: it measures how general the flow is, and here it is not.

Plots (spread PMF, depth profile, survival curves, volatility against sampling interval) are drawn by every run: `build/real_data/calibration/<pair>/plots/`. In them the data survival curve is the training period's Kaplan-Meier curve, because the estimator never reads the test period.

**Reading the verdicts.** With 20 seeds the simulation interval is narrow, and the rule asks for an overlap with the data's bootstrap interval, which is wide for 30 minutes of data; "close" is therefore a gap of up to twice that width. Nothing was tuned to improve any row.

## 9. Known gaps

Said plainly, with nothing tuned to hide them:

- **Trade-sign memory and volatility clustering are expected misses.** The calibrated flow is zero-intelligence: each order's side is drawn independently, so the autocorrelation of aggressive-order signs is about zero by construction, while real signs have long memory [9]. Volatility clustering needs state-dependent behaviour the flow does not have. On the synthetic days the sign autocorrelation of the data comes from the informed trader, which the calibrated flow runs without.
- **No state dependence.** Rates and placements do not depend on the book state, as in queue-reactive models [10], and there is no intraday rate profile. The measured rates are one average over the window.
- **What the dress rehearsal showed (synthetic).** The one-tick spread share (`spread_one_tick_share`) and `fill_le0` miss on both synthetic days, and the spread distribution is beyond the halves yardstick for both models. The likely reason, not isolated by a separate run: the fake days' traders price relative to the mid, and the market maker requotes and keeps the spread at two ticks or more, while the calibrated flow has no market maker and its orders improve the touch (negative placement distances, measured from the data), so its spread closes to one tick far more often. On SYNTHB the informed trader's footprint also shows in the sign autocorrelation and in `vol_1s`. These are gaps the background flow alone cannot explain.
- **Known estimator bias.** With price-window-filtered data, $\lambda_L$ and the far tail of the placement PMF are underestimated (section 3, measured in section 5).
- **A day where a spread bucket is never visited** gives the estimator nothing for that bucket; `validate` refuses a calibration the simulator cannot use rather than filling it in silently (D-382, and the nearest-bucket fallback of section 4).

## 10. Out of scope and stretch goals

Each is a good stretch goal and none was attempted:

1. intraday rate profiles (the U-shape);
2. queue-reactive or Hawkes state-dependent rates [10];
3. order-sign long memory, for example order-splitting metaorders;
4. calibrating the market maker;
5. multiple days.

## 11. C7 (optional) was not done

The optional study of whether the informed trader can be identified was not done, for lack of time. It would sweep the informed trader's rate (6 values) and threshold (4 values) over 10 seeds each with the calibrated background on, and plot the 10 s volatility, excess kurtosis and the autocorrelation of $|r|$ against each parameter. Steep, monotonic curves would mean the metrics pin the parameters down; flat or overlapping curves would mean they do not, as Platt and Gebbie found for behavioural parameters [5]. No result of that study exists.

## 12. Running it on real data

**Real-data calibration: pending data (no LOBSTER files yet).** Nothing in this file was computed from real files.

1. Put the LOBSTER pairs in `data/lobster/` (git-ignored; never commit market data), named `<TICKER>_<DATE>_34200000_57600000_message_<L>.csv` and `<TICKER>_<DATE>_34200000_57600000_orderbook_<L>.csv`.
2. Run the one command:

```
scripts/run_real_data.sh --calibrate
```

For every ticker it runs `lobsim_calibrate validate` with the windows train 36000-45900 s and test 45900-55800 s (10:00 to 12:45 and 12:45 to 15:30), or, when the data does not cover them, the data's span split in half, and it says which (D-383). Options: `--train A-B`, `--test C-D`, `--seeds N`, `--data-dir DIR`, `--report FILE`, `--out DIR`.

**What it writes, and where.** Each run's `validation.md`, `validation.json`, calibration file and plots go to `build/real_data/calibration/<pair>/`. The summary (windows, verdict counts, the leakage line and the full scorecard table for each pair) is written into [REAL_DATA.md](REAL_DATA.md) between the markers `<!-- CALIBRATION:BEGIN -->` and `<!-- CALIBRATION:END -->`; the REAL-DATA block of that file is not touched. With no data it writes "Calibration pending data" with the command and exits with code 3; it exits 1 if any run failed.

**Cross-ticker check.** With two or more tickers it also calibrates on the first (alphabetical) and validates on each of the others, as a separate table. It tests how general the measured flow is, not only whether it fits one stock. The simulation starts at the **validated** ticker's median mid, because log-return volatility depends on the tick size relative to the price and keeping the first ticker's price level would bias that row.
