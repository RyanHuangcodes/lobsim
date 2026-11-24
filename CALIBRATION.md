# Calibrating the simulated market to data

lobsim's background traders used to have hand-picked settings. This page describes how they are now **measured** from a LOBSTER day, how the simulator reproduces what was measured, and how the result is **tested on data it was not fitted to**.

> **Data status.** No real LOBSTER files have been run yet (the free samples are no longer available). Every number in sections 6 to 9 was computed from the two synthetic fake days `SYNTHA` and `SYNTHB` and is labelled synthetic. No number here says anything about a real market. When real files are placed in `data/lobster/`, one command runs the same code on them with no code changes (see [Running it on real data](#12-running-it-on-real-data)).

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

**Placement.** The PMF $P(\Delta \mid \text{spread bucket}, \text{side})$ for the buckets $s = 1$, $s = 2$ and $s \ge 3$, over the integers from $-(s-1)$ to the largest observed $\Delta$ (D-306). Orders that cannot be placed are excluded and counted by reason (no previous row, unparsed or malformed or one-sided or crossed previous row, best quote off the tick grid, would cross); such an order is still a submission, so it counts in $\lambda_L$ and in the size PMF. The simulator uses the empirical PMF. For the report only, a power law $P(\Delta) \propto 1/(\Delta_0 + \Delta)^{1+\mu}$ for $\Delta \ge 0$ [2][3] is fitted by discrete maximum likelihood (D-314). On three of the four sides of the fake days the fit runs into its bound (the noise trader's uniform distance is not power-law shaped), and the report says so.

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
               "owner_count": 50, "latency_us": {"entry": 0, "response": 0, "feed": 0, "jitter": 0}, "start_price": 9984}
```

`file` is resolved against the config file's directory; `owner_count` defaults to 50, latency to 0, and `start_price` to the calibration's `median_mid_ticks`. Unknown keys are an error, and so is a calibration the agent cannot sample from (for example an empty size PMF with a positive rate), with the field named. **Defaults are off:** without the section nothing changes, `to_json(cfg)` has no `background` key, and every existing config, hash and output is identical. The informed trader and market maker are off in the validation runs because they have no data to be calibrated from; users can switch them back on.

**Sampling rules** (D-324). Limit orders arrive as a Poisson process at $\lambda_L$ with the measured side probabilities. The spread bucket comes from the agent's local view of the book, $\Delta$ is drawn from that bucket's PMF, the price is best $\mp \Delta$ ticks, and the size comes from the limit-size PMF (GTC). Aggressive orders arrive at $\lambda_M$, with measured side probabilities and sizes, as market orders (IOC).

- **Integer cumulative tables.** Each PMF becomes a table of cumulative `uint64` counts. A draw is the project's `uniform_int(0, total - 1)` plus a binary search, which is exact and portable. Totals above $2^{63}$ use raw rejection sampling. Only the project's RNG is used, never a `std::` distribution.
- **Inverse CDF for lifetimes.** At the order's acknowledgment one `uniform01` $u$ is drawn from a separate stream, and the lifetime is the smallest $t$ with $\hat S(t) \le u$ in the order's distance bucket.
- **The no-cancel tail.** If $u$ is below the last survival value (the curve never goes that low) or the curve is empty, no cancel is scheduled: the order lives until it is filled or the session ends. Orders in the tail are counted. Test `CalibratedFlow.NoCancelTailLeavesAboutFortyPercentOfOrdersAlone`.

**Crossing redraws and the clamp.** If $\Delta < -(s-1)$ the placement would cross. The agent redraws, up to 100 times, then uses $-(s-1)$ and counts `placement_clamped`. Tests: `CalibratedFlow.CrossingPlacementsAreRedrawn`, `ClampsToTheCrossingBoundAfter100Redraws`. Prices outside the valid range are moved to it and counted (`price_clamped`).

**Fallbacks when the local book is not two-sided** (D-325). Own side present, other side absent: the reference is the own best, the bucket is `3+`, and nothing can cross (counted `fallback_one_sided`). Own side absent, other side present: the reference is the other best $\mp$ 1 tick, a virtual spread of 1 (bucket `1`, bound 0). Empty book: the reference is the start price, bucket `1`, bound 0 (`fallback_empty`). In addition, when the bucket the agent is in has no samples for that side, the agent uses the **nearest non-empty bucket** (decision D-385; it is what lets a calibration from a day that never visited one spread bucket still run). Tests: `CalibratedFlow.EmptyBookUsesTheStartPriceAndIsCounted`, `OneSidedBookFallbacks`.

**Several owner ids.** One agent sends the whole flow under `owner_count` owner ids (default 50, used in turn), meant to make self-trade prevention almost never trigger. **50 is not enough for that** (measured in section 5: at 50, 6.3% of aggressive orders are truncated), so `lobsim_calibrate validate` and `configs/calibrated.json` use 10,000; the configuration default stays 50 (D-393). The simulator gives the agent its own id plus extra ids from $2^{30}$ upwards, so no existing agent's id, event order or hash changes. Private events for an extra id go to the agent, and a cancel carries the owner of the order's placement (D-323). The agent runs after every other agent and has its own random stream role (D-324). The exported order log works with the exporter and the replayer unchanged (`calibrated_flow_closed_loop`).

## 5. Parameter recovery

**SYNTHETIC. Everything in this section describes simulations of a hand-made calibration, not market data.**

The estimator is checked before anything is estimated from data: choose the parameters ourselves, simulate them, estimate them back, and compare. The one committed truth is `calibration/recovery_truth.json`, in the same schema as an estimate, so the simulator reads it exactly as it would read a real calibration.

### The truth

`tools/make_recovery_truth.py` writes the file (`--check` verifies that the committed one is up to date, a ctest entry). Both sides are alike.

| Input | Value |
|---|---|
| Rates | 40 limit and 4 aggressive orders per second, half on each side |
| Placement, s = 1 | P(Δ) ∝ (1 + Δ)^-1.6 on Δ = 0..30 (s = 1 allows no Δ < 0) |
| Placement, s = 2 | 90% of the mass on that law, 10% at Δ = -1 |
| Placement, s ≥ 3 | 90% on that law, 10% spread uniformly over Δ = -1..-4 |
| Lifetimes (every distance bucket) | S(t) = 0.3·1[t < 0.5 s] + 0.7·e^(-t / 20 s), then S = 0 at 600 s so every order gets a cancel |
| Limit sizes | 100 (40%), 200 (20%), 300 (10%), 500 (10%), 1000 (5%), and 15% odd lots on 1, 10, 20, 25, 50, 75, 99; mean 217 |
| Aggressive sizes | 100 (15%), 500 (20%), 1000 (30%), 2000 (15%), 5000 (5%), and the same 15% odd lots; mean 972 |

Weights are integers at a scale of 10^9, rounded half up. The survival grid is 10 ms wide around the median and coarse in the far tail; the median on the grid is 6.730 s against the exact 20·ln(1.4) = 6.7294 s (+0.008%).

Three choices were made after looking at the simulated market. Two of them, because the first attempt could not test what it was meant to test, are the aggressive sizes and the owner ids; the third is the number of exported levels, L = 40, chosen after one check failed at L = 10 (see "Why L = 40" below and D-364, D-394):

- **Aggressive sizes.** With aggressive orders sized like limit orders, the price never moved: 20 minutes at a 1-tick spread, zero samples outside the s = 1 bucket. Six size distributions were tried (short runs), looking at the spread and the number of samples per spread bucket; the one above is the first that fills all three buckets without the spread running away (median 1 tick, 90th percentile 4). The choice decides what is tested, not whether it passes.
- **10,000 owner ids** in `configs/calibrated.json` (the default is 50). One agent produces the whole flow, and a market order that meets a resting order of its own id is cut short by self-trade prevention. Measured directly, with the same seed and nothing else changed:

| `owner_count` | aggressive-size TV (tolerance 0.02) | aggressive orders with a size that is not in the truth | λ_M error (buy, sell) |
|---|---|---|---|
| 50 (the default) | **0.066, fails** | 6.3% | -1.6%, -1.9% |
| 10,000 | 0.0045 | 0.03% | +0.2%, +1.2% |

So with the default, self-trade prevention is not rare in a market like this one. Anything that simulates a calibrated flow with the default 50 should expect truncated aggressive orders.

### How the estimate is compared

`lobsim_calibrate compare` applies the rules of the specification: rates within 3%; placement total variation (TV) ≤ 0.03 per side for each spread bucket with at least 2,000 estimated samples; size TV ≤ 0.02; lifetime medians within 10% per distance bucket (20% filtered).

- **Placement is compared like with like.** The simulator redraws a Δ below -(s - 1), so the truth is first restricted to the bucket's allowed range, [0, ∞) for s = 1 and [-1, ∞) for s = 2, and renormalized. The s ≥ 3 bucket mixes spreads with different ranges; it is compared with the s = 3 range [-2, ∞) and reported only.
- **Filtered:** rates are reported as biases. Placement is compared on Δ up to the 95th percentile of the window depth: per side, over the rows where that side shows all L levels, the distance in ticks from the best to the L-th level, by nearest rank. The share of each bucket above it is reported.
- A bucket with too few samples is *skipped*, not passed. A lifetime curve that never reaches 0.5 *fails*.

### Results: 2 hours, unfiltered, L = 40

`scripts/run_recovery.sh` (seed 20240101, 7,200 s window after 300 s of warm-up, hidden executions and the halt left on because the estimator ignores types 5 and 7; 21 s for all variants in a release build). All unfiltered tolerances pass:

| check | truth | estimate | value | tolerance | verdict |
|---|---|---|---|---|---|
| rate: limit buy | 20.000 /s | 20.024 /s (144171) | relative error 0.12% | ≤ 3.00% | PASS |
| rate: limit sell | 20.000 /s | 19.965 /s (143750) | relative error -0.17% | ≤ 3.00% | PASS |
| rate: aggressive buy | 2.000 /s | 2.004 /s (14431) | relative error 0.22% | ≤ 3.00% | PASS |
| rate: aggressive sell | 2.000 /s | 2.024 /s (14574) | relative error 1.21% | ≤ 3.00% | PASS |
| placement: buy s=1 | truth on [0, inf], renormalized | 101555 samples | TV 0.0050 | ≤ 0.0300 | PASS |
| placement: buy s=2 | truth on [-1, inf], renormalized | 16678 samples | TV 0.0061 | ≤ 0.0300 | PASS |
| placement: buy s=3+ | truth on [-2, inf], renormalized | 25928 samples | TV 0.0271 | - | info |
| placement: sell s=1 | truth on [0, inf], renormalized | 100877 samples | TV 0.0040 | ≤ 0.0300 | PASS |
| placement: sell s=2 | truth on [-1, inf], renormalized | 16966 samples | TV 0.0177 | ≤ 0.0300 | PASS |
| placement: sell s=3+ | truth on [-2, inf], renormalized | 25898 samples | TV 0.0279 | - | info |
| size: limit_size | 12 values | 287921 samples | TV 0.0027 | ≤ 0.0200 | PASS |
| size: aggressive_size | 12 values | 29005 samples | TV 0.0045 | ≤ 0.0200 | PASS |
| lifetime median: <=0 | 6.730 s | 6.720 s | relative error -0.15% | ≤ 10.00% | PASS |
| lifetime median: 1-2 | 6.730 s | 6.770 s | relative error 0.59% | ≤ 10.00% | PASS |
| lifetime median: 3-9 | 6.730 s | 6.590 s | relative error -2.08% | ≤ 10.00% | PASS |
| lifetime median: >=10 | 6.730 s | 6.640 s | relative error -1.34% | ≤ 10.00% | PASS |
| fit: buy | delta0 1.000, mu 0.600 | delta0 1.019, mu 0.609 | mu difference 0.0088 | - | info |
| fit: sell | delta0 1.000, mu 0.600 | delta0 1.000, mu 0.598 | mu difference -0.0021 | - | info |

`result: PASS (14 passed, 0 failed, 4 informational, 0 skipped)`

**Is it luck of the seed?** Ten more seeds (1-10), two hours each: all 14 judged checks pass in 10 of 10.

| Lifetime bucket | mean error of the median over 10 seeds | standard deviation | standard error of the mean |
|---|---|---|---|
| ≤ 0 | -0.09% | 1.39% | 0.44% |
| 1-2 | +0.55% | 1.33% | 0.42% |
| 3-9 | +0.16% | 1.25% | 0.40% |
| ≥ 10 | +0.31% | 1.44% | 0.46% |

Pooling the placement counts of those ten runs tests for a small systematic difference, which a tolerance cannot:

| Bucket | pooled samples | pooled TV | TV of exact sampling (mean) | χ² p-value |
|---|---|---|---|---|
| buy, s = 1 | 1,033,746 | 0.0012 | 0.0013 | 0.60 |
| buy, s = 2 | 163,190 | 0.0032 | 0.0034 | 0.24 |
| sell, s = 1 | 1,034,361 | 0.0015 | 0.0013 | 0.73 |
| sell, s = 2 | 163,808 | 0.0034 | 0.0034 | 0.73 |

The estimated placement is indistinguishable from exact sampling of the truth.

**Why L = 40.** The published window is what the censoring rule looks at: an order priced worse than the L-th level is censored. Here almost every tick is occupied, so with L = 10 an order 10 or more ticks behind the best is outside the window the moment it is submitted. At L = 40 the window holds (almost) the whole book and no judged bucket loses more than 76 orders. The same day exported with `--no-filter-window` at **L = 10** fails one check:

| check | truth | estimate | value | tolerance | verdict |
|---|---|---|---|---|---|
| lifetime median: <=0 | 6.730 s | 6.720 s | relative error -0.15% | ≤ 10.00% | PASS |
| lifetime median: 1-2 | 6.730 s | 6.770 s | relative error 0.59% | ≤ 10.00% | PASS |
| lifetime median: 3-9 | 6.730 s | 6.590 s | relative error -2.08% | ≤ 10.00% | PASS |
| lifetime median: >=10 | 6.730 s | 7.840 s | relative error 16.49% | ≤ 10.00% | FAIL |

`result: FAIL (13 passed, 1 failed, 4 informational, 0 skipped)`

In the ≥ 10 bucket 26,374 of 26,591 orders are censored at once and 198 cancellations are left. Over the ten seeds its median is off by +10.3% on average with a standard deviation of 26.4% (standard error 8.4%) on 153-255 cancellations, and only 2 of 10 runs pass: too few events to show a bias or to rule one out. This is what the choice of L decides, so it is stated here rather than hidden: **at L = 10 the lifetime of deep orders cannot be estimated from two hours of this market.** A real L = 10 file of a stock whose book is as dense would share the limit.

### Results: filtered, L = 10

The export real data resembles: the price-window filter at L = 10. The rates and the tails are biases to be measured; the judged rows use the filtered rules and all pass on this seed. **That is luck on one bucket:** over seeds 1 to 10 the filtered comparison passes in 5 and fails in 5 (seeds 4, 6, 8, 9 and 10), every time only on the ≥ 10 median (20% rule, about 200 cancellations), which is off by 20% to 54%. The other nine filtered checks pass in all ten.

| check | truth | estimate | value | tolerance | verdict |
|---|---|---|---|---|---|
| rate: limit buy | 20.000 /s | 18.230 /s (131255) | relative bias -8.85% | - | info |
| rate: limit sell | 20.000 /s | 18.181 /s (130902) | relative bias -9.10% | - | info |
| rate: aggressive buy | 2.000 /s | 2.004 /s (14431) | relative bias 0.22% | - | info |
| rate: aggressive sell | 2.000 /s | 2.024 /s (14574) | relative bias 1.21% | - | info |
| placement: buy s=1 | truth on [0, 10], renormalized | 92248 samples | TV 0.0100 | ≤ 0.0300 | PASS |
| placement tail: buy s=1 Delta>10 | 8.41% | 0.05% | relative bias -99.37% | - | info |
| placement: buy s=2 | truth on [-1, 10], renormalized | 15281 samples | TV 0.0097 | ≤ 0.0300 | PASS |
| placement tail: buy s=2 Delta>10 | 7.57% | 0.05% | relative bias -99.40% | - | info |
| placement: buy s=3+ | truth on [-2, 10], renormalized | 23620 samples | TV 0.0245 | - | info |
| placement tail: buy s=3+ Delta>10 | 7.97% | 0.17% | relative bias -97.88% | - | info |
| placement: sell s=1 | truth on [0, 11], renormalized | 91549 samples | TV 0.0190 | ≤ 0.0300 | PASS |
| placement tail: sell s=1 Delta>11 | 7.51% | 0.04% | relative bias -99.45% | - | info |
| placement: sell s=2 | truth on [-1, 11], renormalized | 15501 samples | TV 0.0189 | ≤ 0.0300 | PASS |
| placement tail: sell s=2 Delta>11 | 6.76% | 0.06% | relative bias -99.14% | - | info |
| placement: sell s=3+ | truth on [-2, 11], renormalized | 23753 samples | TV 0.0291 | - | info |
| placement tail: sell s=3+ Delta>11 | 7.11% | 0.18% | relative bias -97.46% | - | info |
| size: limit_size | 12 values | 262157 samples | TV 0.0031 | ≤ 0.0200 | PASS |
| size: aggressive_size | 12 values | 29005 samples | TV 0.0045 | ≤ 0.0200 | PASS |
| lifetime median: <=0 | 6.730 s | 6.720 s | relative error -0.15% | ≤ 20.00% | PASS |
| lifetime median: 1-2 | 6.730 s | 6.770 s | relative error 0.59% | ≤ 20.00% | PASS |
| lifetime median: 3-9 | 6.730 s | 6.590 s | relative error -2.08% | ≤ 20.00% | PASS |
| lifetime median: >=10 | 6.730 s | 7.840 s | relative error 16.49% | ≤ 20.00% | PASS |
| fit: buy | delta0 1.000, mu 0.600 | delta0 3.867, mu 2.715 | mu difference 2.1152 | - | info |
| fit: sell | delta0 1.000, mu 0.600 | delta0 4.004, mu 2.811 | mu difference 2.2108 | - | info |

`result: PASS (10 passed, 0 failed, 14 informational, 0 skipped)`

What the filter does to the estimates, and why:

- **λ_L is underestimated by 9%.** The filter hid 25,764 of 287,921 submissions (8.95%), and every one of them was placed 10 or more ticks behind the best: the number of orders in the ≤ 0, 1-2 and 3-9 buckets is the same in both exports (141,956, 68,236 and 51,119). λ_M is not touched: executions happen inside the window.
- **The far tail of the placement PMF is lost.** Of 26,591 orders at Δ ≥ 10, 827 remain. Above the window's 95th percentile depth (10 ticks on the bid side, 11 on the ask side) the truth holds 6.8-8.4% of each bucket and the estimate 0.04-0.18%. The power-law fit, which sees only a truncated tail, gives μ of 2.7 and 2.8 instead of 0.6.
- **Inside the window the placement is right.** No order with Δ < 10 was hidden, so the filtered TV up to the 95th percentile (0.010-0.019) is what is missing at the edge of that range: Δ = 10 on the bid side, 10 and 11 on the ask side (0.009-0.019 of it). With Δ ≥ 10 left out, the filtered TV is 0.002-0.013, the same as the unfiltered day's on the same range (0.002-0.014).
- **Lifetimes are barely affected.** The medians of the three shallow buckets are the same as unfiltered. Orders that leave the window are censored (and their later deletes are not cancellations); what the filter removes are submissions that the L = 10 rule would have censored at lifetime 0 anyway. Only the ≥ 10 bucket is left too thin to measure.

### What this does and does not show

- **The check can fail.** Nine estimator faults were put in a copy of the sources, one at a time, and run through the CI check. Caught (eight): the Δ taken from the row after the message, a spread one tick too wide, the buy Δ measured from the ask, an execution counted as a cancellation, executed orders dropped instead of censored, every execution message counted as its own aggressive order, only the first execution of a burst counted, and a doubled lifetime clock. The unmodified estimator passes.
- **It is blind to a swap of sides.** The ninth fault, reading the aggressor as the Direction side, is *not* caught, because the truth is the same on both sides. The unit tests of the estimator cover it.
- **The tolerance of 0.03 is near the sampling noise at small samples.** Exact sampling of the s = 2 truth has a mean TV of 0.033 at 2,000 samples (above 0.03 in 67% of draws), 0.027 at 3,000 (27%), 0.021 at 5,000 (2%) and 0.015 at 10,000 (0%). A 20-minute run at the specification's tolerances fails one such bucket (sell, s = 2: TV 0.038 on 2,972 samples, seed 20240101); the counts pooled over ten seeds above show no mismatch there. The 2-hour run has at least 16,000 samples in every judged bucket.
- **The check covers one market.** The s ≥ 3 bucket is reported only, and the truth has no state-dependent rates or sign memory.

### The CI version

The ctest `calibration_recovery` runs 20 minutes (seed 20240101, unfiltered, L = 40), with twice the tolerances, in 11 s in a debug build. It also fails if any rate, size or lifetime check was skipped or if fewer than four placement buckets had enough samples to be judged. `recovery_truth_up_to_date` checks that the committed truth is what the script writes.

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
# cross-ticker: both days in one folder (hard links or symlinks), so the script sees two tickers
scripts/run_real_data.sh --calibrate --data-dir data/synthetic_both --report <scratch>.md
```

Wall time of the script (release build, 8 threads, 20 + 20 seeds of 1800 s measured each, including the 1000-resample bootstrap of the data and the estimator): 10.2 s for SYNTHA, 16.5 s for SYNTHB, 37.6 s for the two-ticker folder with its cross-ticker run. Times vary with machine load. The runs are deterministic: the two-ticker run reproduced each single-folder `validation.json` exactly.

### Why the result is partly circular

The fake days were generated by the old hand-picked noise traders **plus** an informed trader and an Avellaneda-Stoikov market maker (`configs/default.json`; SYNTHB: informed rate x4, noise rate x2). The calibrated flow is estimated from those files, but it replaces the whole background with one zero-intelligence agent and runs **without** the informed trader and the market maker, as the validation specifies. So it should match the spread and depth the noise traders and the market maker produced, and it can miss what the informed trader added: volatility, trade-sign memory. That gap is a useful result: it shows what the background flow alone cannot explain. It is also not a test of calibration against an independent truth, because the data is itself a lobsim run.

### Scorecard: SYNTHA (SYNTHETIC data, NOT real market data)

Verdicts, calibrated simulation: Calibrated: 31 match, 17 close, 4 miss, 1 undefined. Baseline: 29 match, 12 close, 11 miss, 1 undefined.

| metric | data (test) [95% interval] | calibrated sim [95% CI of mean] | baseline sim [95% CI of mean]; its verdict | calibrated verdict |
|---|---|---|---|---|
| `spread_mean` | 2.02 [1.99, 2.049] | 1.916 [1.912, 1.921] | 2.225 [2.22, 2.23]; miss (sim/data = 1.101) | miss (sim/data = 0.9485) |
| `spread_one_tick_share` | 0.01574 [0.001509, 0.03533] | 0.1818 [0.1784, 0.1852] | 0.0003555 [0.000162, 0.000549]; close | miss (sim/data = 11.55) |
| `one_sided_share` | 0 [0, 0] | 0 [0, 0] | 0 [0, 0]; match | match |
| `depth_buy_L1` | 56.83 [40.07, 74.82] | 73.94 [72.41, 75.47] | 66.87 [64.17, 69.58]; match | match |
| `depth_buy_L2` | 177.6 [132.7, 221.9] | 221.6 [217.8, 225.4] | 216.9 [210.1, 223.6]; match | match |
| `depth_buy_L3` | 279.9 [238.9, 319.1] | 306 [302.4, 309.6] | 318.6 [313.3, 323.8]; match | match |
| `depth_buy_L4` | 324.1 [296.3, 346.6] | 336.6 [334, 339.2] | 347.9 [345.6, 350.3]; match | match |
| `depth_buy_L5` | 339.7 [325.4, 351] | 342.2 [339.8, 344.5] | 350.5 [348.4, 352.6]; match | match |
| `depth_buy_L6` | 340.9 [332.7, 350.7] | 343.2 [341.1, 345.3] | 351.1 [348.8, 353.3]; match | match |
| `depth_buy_L7` | 340.9 [335.9, 349.2] | 343.8 [342, 345.6] | 351.3 [348.8, 353.8]; match | match |
| `depth_buy_L8` | 332.4 [329.4, 339.6] | 332.5 [330.1, 334.9] | 348.7 [345.6, 351.7]; close | match |
| `depth_buy_L9` | 311 [297.5, 328.7] | 291.7 [288.3, 295] | 320 [316, 324]; match | close |
| `depth_buy_L10` | 249.4 [205.5, 292.7] | 194.9 [190.9, 198.8] | 216.4 [210.7, 222]; match | close |
| `depth_sell_L1` | 78.7 [56.9, 101.7] | 50.56 [49.06, 52.05] | 67.26 [64.38, 70.15]; match | close |
| `depth_sell_L2` | 223.3 [179.8, 269.6] | 163.8 [160.9, 166.8] | 215.9 [210.8, 221]; match | close |
| `depth_sell_L3` | 308.9 [282.3, 335.8] | 259.2 [255.6, 262.8] | 316.3 [312.8, 319.9]; match | close |
| `depth_sell_L4` | 337 [327.7, 348.2] | 314.6 [311.5, 317.8] | 347.8 [344.8, 350.8]; match | close |
| `depth_sell_L5` | 343.1 [333.4, 349] | 337.3 [334.8, 339.8] | 351.3 [348.8, 353.7]; match | match |
| `depth_sell_L6` | 343.7 [336.8, 350.1] | 345.4 [343.1, 347.7] | 351.4 [348.5, 354.2]; match | match |
| `depth_sell_L7` | 336.1 [330.3, 342] | 346.4 [344.1, 348.8] | 350.3 [347.4, 353.2]; close | close |
| `depth_sell_L8` | 324 [307.4, 343.1] | 342.1 [340.1, 344.1] | 348.4 [345.1, 351.8]; close | match |
| `depth_sell_L9` | 285.4 [249.6, 324] | 320.2 [317.9, 322.5] | 321 [315.8, 326.2]; match | match |
| `depth_sell_L10` | 200.6 [152, 255.7] | 247.5 [244.6, 250.5] | 217.2 [210.2, 224.1]; match | match |
| `vol_1s` | 6.179e-05 [5.826e-05, 6.571e-05] | 6.807e-05 [6.737e-05, 6.877e-05] | 4.953e-05 [4.91e-05, 4.996e-05]; miss (sim/data = 0.8016) | close |
| `vol_10s` | 8.479e-05 [7.437e-05, 9.027e-05] | 0.0001036 [0.0001006, 0.0001067] | 8.234e-05 [8.018e-05, 8.449e-05]; match | close |
| `vol_60s` | 0.0001904 [0.0001326, 0.0002237] | 0.000131 [0.0001226, 0.0001394] | 0.0001173 [0.0001101, 0.0001245]; close | match |
| `kurtosis_10s` | -0.2402 [-0.795, 0.2264] | -0.1854 [-0.3461, -0.02459] | -0.2423 [-0.3627, -0.1219]; match | match |
| `acf_ret_1s_lag1` | -0.39 [-0.4237, -0.3496] | -0.3116 [-0.3202, -0.3029] | -0.2625 [-0.2751, -0.2499]; miss (sim/data = 0.6731) | close |
| `acf_abs_10s_lag1` | -0.04674 [-0.2069, 0.02653] | 0.07772 [0.04267, 0.1128] | 0.02087 [-0.01262, 0.05436]; match | close |
| `acf_abs_10s_lag2` | 0.03943 [-0.03946, 0.1491] | 0.01327 [-0.02659, 0.05313] | -0.021 [-0.05789, 0.01589]; match | match |
| `acf_abs_10s_lag3` | -0.04422 [-0.1161, 0.1202] | -0.01707 [-0.0537, 0.01955] | -0.02421 [-0.05774, 0.009326]; match | match |
| `acf_abs_10s_lag4` | -0.08117 [-0.302, 0.06674] | -0.02732 [-0.05733, 0.002679] | -0.005434 [-0.04088, 0.03002]; match | match |
| `acf_abs_10s_lag5` | 0.1816 [0.0637, 0.271] | 0.01684 [-0.01618, 0.04985] | -0.007598 [-0.03879, 0.0236]; close | close |
| `acf_abs_10s_lag6` | 0.04174 [-0.1625, 0.129] | 0.01427 [-0.01875, 0.04728] | -0.01486 [-0.04801, 0.01829]; match | match |
| `acf_abs_10s_lag7` | -0.09412 [-0.2547, 0.03904] | -0.04006 [-0.07471, -0.00542] | 0.01156 [-0.0185, 0.04163]; match | match |
| `acf_abs_10s_lag8` | -0.1578 [-0.3924, 0.06801] | -0.002652 [-0.04126, 0.03596] | -0.01428 [-0.04298, 0.01443]; match | match |
| `acf_abs_10s_lag9` | -0.1147 [-0.2718, 0.1193] | -0.01 [-0.03632, 0.01632] | -0.01494 [-0.05517, 0.02528]; match | match |
| `acf_abs_10s_lag10` | 0.06443 [-0.08972, 0.2156] | -0.01046 [-0.03986, 0.01894] | 0.02646 [-0.0006733, 0.0536]; match | match |
| `sign_acf_lag1` | 0.04395 [0.02664, 0.06279] | -0.0003188 [-0.003347, 0.00271] | -0.002515 [-0.006451, 0.001422]; close | close |
| `sign_acf_lag10` | 0.03483 [0.01645, 0.04239] | 0.0008624 [-0.002351, 0.004075] | -0.002394 [-0.005772, 0.0009836]; close | close |
| `sign_acf_lag100` | 0.05712 [0.04184, 0.08096] | -0.002384 [-0.005369, 0.0006005] | 0.00245 [-0.001723, 0.006623]; close | miss (sim/data = -0.04174) |
| `fill_le0` | 0.6882 [0.6608, 0.7244] | 0.6352 [0.6338, 0.6366] | 0.8599 [0.8574, 0.8624]; miss (sim/data = 1.25) | close |
| `fill_1_2` | 0.2536 [0.2488, 0.2619] | 0.2786 [0.2761, 0.2812] | 0.232 [0.2287, 0.2354]; miss (sim/data = 0.915) | miss (sim/data = 1.099) |
| `fill_3_9` | 0.01372 [0.007527, 0.02011] | 0.00635 [0.005958, 0.006741] | 0.001511 [0.001285, 0.001737]; close | close |
| `fill_ge10` | n/a [n/a] | 0 [0, 0] | n/a [n/a]; n/a | n/a |
| `fill_all` | 0.1742 [0.1708, 0.1779] | 0.1695 [0.1688, 0.1702] | 0.1509 [0.1502, 0.1516]; miss (sim/data = 0.8661) | close |
| `executed_share` | 0.1702 [0.1668, 0.1742] | 0.1668 [0.1661, 0.1674] | 0.1495 [0.1488, 0.1501]; miss (sim/data = 0.8783) | match |
| `lambda_limit_buy` | 23.57 [23.17, 23.98] | 24 [23.96, 24.04] | 21.23 [21.18, 21.28]; miss (sim/data = 0.9008) | match |
| `lambda_limit_sell` | 23.47 [22.99, 23.96] | 23.86 [23.81, 23.91] | 21.25 [21.2, 21.3]; miss (sim/data = 0.9056) | match |
| `lambda_limit_total` | 47.03 [46.27, 47.88] | 47.86 [47.81, 47.92] | 42.48 [42.42, 42.54]; miss (sim/data = 0.9032) | match |
| `lambda_market_buy` | 4.437 [3.868, 5.001] | 4.057 [4.032, 4.082] | 3.371 [3.351, 3.391]; close | match |
| `lambda_market_sell` | 4.024 [3.67, 4.472] | 4.357 [4.335, 4.38] | 3.363 [3.343, 3.384]; close | match |
| `lambda_market_total` | 8.462 [8.176, 8.695] | 8.415 [8.385, 8.444] | 6.734 [6.708, 6.761]; miss (sim/data = 0.7959) | match |

### Scorecard: SYNTHB (SYNTHETIC data, NOT real market data)

Verdicts, calibrated simulation: Calibrated: 36 match, 12 close, 4 miss, 1 undefined. Baseline: 16 match, 12 close, 24 miss, 1 undefined.

| metric | data (test) [95% interval] | calibrated sim [95% CI of mean] | baseline sim [95% CI of mean]; its verdict | calibrated verdict |
|---|---|---|---|---|
| `spread_mean` | 2.032 [2.001, 2.059] | 1.961 [1.958, 1.965] | 2.225 [2.22, 2.23]; miss (sim/data = 1.095) | close |
| `spread_one_tick_share` | 0.01287 [0.001989, 0.03156] | 0.1614 [0.1586, 0.1642] | 0.0003555 [0.000162, 0.000549]; close | miss (sim/data = 12.54) |
| `one_sided_share` | 0 [0, 0] | 0 [0, 0] | 0 [0, 0]; match | match |
| `depth_buy_L1` | 116.9 [60.54, 168.6] | 98.1 [94.62, 101.6] | 66.87 [64.17, 69.58]; match | match |
| `depth_buy_L2` | 339.9 [204.1, 469.7] | 345.5 [338.5, 352.5] | 216.9 [210.1, 223.6]; match | match |
| `depth_buy_L3` | 500.3 [378, 618.9] | 548.1 [541.5, 554.7] | 318.6 [313.3, 323.8]; close | match |
| `depth_buy_L4` | 582 [505.9, 656.5] | 649 [644.2, 653.9] | 347.9 [345.6, 350.3]; miss (sim/data = 0.5979) | match |
| `depth_buy_L5` | 633.7 [590.4, 681.3] | 677.6 [673.2, 681.9] | 350.5 [348.4, 352.6]; miss (sim/data = 0.5531) | match |
| `depth_buy_L6` | 655.8 [638.7, 684] | 684.9 [681.2, 688.6] | 351.1 [348.8, 353.3]; miss (sim/data = 0.5353) | match |
| `depth_buy_L7` | 655.2 [642.6, 676.8] | 685.1 [681.7, 688.5] | 351.3 [348.8, 353.8]; miss (sim/data = 0.5362) | close |
| `depth_buy_L8` | 646.4 [625.6, 674.9] | 671.9 [669.4, 674.3] | 348.7 [345.6, 351.7]; miss (sim/data = 0.5394) | match |
| `depth_buy_L9` | 594 [535.4, 660] | 607.2 [602.6, 611.8] | 320 [316, 324]; miss (sim/data = 0.5388) | match |
| `depth_buy_L10` | 478.4 [355.9, 604.1] | 439.1 [431.9, 446.3] | 216.4 [210.7, 222]; close | match |
| `depth_sell_L1` | 165.9 [97.4, 237.9] | 98.87 [96.11, 101.6] | 67.26 [64.38, 70.15]; close | match |
| `depth_sell_L2` | 442.2 [314.1, 578.6] | 345.8 [339.3, 352.3] | 215.9 [210.8, 221]; close | match |
| `depth_sell_L3` | 572.4 [494.5, 658] | 548.6 [542.5, 554.7] | 316.3 [312.8, 319.9]; miss (sim/data = 0.5526) | match |
| `depth_sell_L4` | 638.5 [610.6, 675.4] | 644.7 [640.4, 648.9] | 347.8 [344.8, 350.8]; miss (sim/data = 0.5447) | match |
| `depth_sell_L5` | 657.5 [647.1, 674] | 672.6 [668.9, 676.3] | 351.3 [348.8, 353.7]; miss (sim/data = 0.5342) | match |
| `depth_sell_L6` | 655.7 [630.4, 682.7] | 682.8 [679.9, 685.8] | 351.4 [348.5, 354.2]; miss (sim/data = 0.5359) | match |
| `depth_sell_L7` | 627.8 [588.9, 668.5] | 685.1 [682.5, 687.8] | 350.3 [347.4, 353.2]; miss (sim/data = 0.558) | close |
| `depth_sell_L8` | 583.8 [515.8, 647.9] | 667.6 [664.6, 670.6] | 348.4 [345.1, 351.8]; miss (sim/data = 0.5968) | close |
| `depth_sell_L9` | 509.1 [392, 619.5] | 590.4 [584.9, 595.9] | 321 [315.8, 326.2]; close | match |
| `depth_sell_L10` | 373.4 [218.9, 525.7] | 410.9 [403.5, 418.3] | 217.2 [210.2, 224.1]; match | match |
| `vol_1s` | 5.976e-05 [5.741e-05, 6.094e-05] | 8.122e-05 [8.06e-05, 8.184e-05] | 4.932e-05 [4.889e-05, 4.975e-05]; miss (sim/data = 0.8252) | miss (sim/data = 1.359) |
| `vol_10s` | 9.242e-05 [8.012e-05, 9.421e-05] | 0.0001096 [0.0001068, 0.0001124] | 8.198e-05 [7.984e-05, 8.413e-05]; match | close |
| `vol_60s` | 0.0003196 [0.0001871, 0.0003429] | 0.0001388 [0.000131, 0.0001466] | 0.0001168 [0.0001097, 0.0001239]; close | close |
| `kurtosis_10s` | -0.6296 [-0.9634, 0.3695] | -0.3694 [-0.4771, -0.2618] | -0.2423 [-0.3627, -0.1219]; match | match |
| `acf_ret_1s_lag1` | -0.354 [-0.3873, -0.3137] | -0.3819 [-0.3907, -0.3732] | -0.2625 [-0.2751, -0.2499]; close | match |
| `acf_abs_10s_lag1` | -0.08165 [-0.2292, -0.007125] | 0.06218 [0.02588, 0.09848] | 0.02087 [-0.01262, 0.05436]; match | close |
| `acf_abs_10s_lag2` | -0.04553 [-0.2113, -0.002475] | -0.03554 [-0.06615, -0.004927] | -0.021 [-0.05789, 0.01589]; match | match |
| `acf_abs_10s_lag3` | 0.01444 [-0.1203, 0.1793] | 0.01162 [-0.01891, 0.04215] | -0.02421 [-0.05774, 0.009326]; match | match |
| `acf_abs_10s_lag4` | -0.04962 [-0.2251, 0.1118] | 0.006364 [-0.03957, 0.0523] | -0.005434 [-0.04088, 0.03002]; match | match |
| `acf_abs_10s_lag5` | 0.06373 [-0.07189, 0.2811] | 0.008437 [-0.02591, 0.04279] | -0.007598 [-0.03879, 0.0236]; match | match |
| `acf_abs_10s_lag6` | -0.1291 [-0.2938, 0.03859] | 0.002883 [-0.03878, 0.04454] | -0.01486 [-0.04801, 0.01829]; match | match |
| `acf_abs_10s_lag7` | -0.0379 [-0.1813, 0.1073] | 0.009232 [-0.02505, 0.04351] | 0.01156 [-0.0185, 0.04163]; match | match |
| `acf_abs_10s_lag8` | 0.01928 [-0.1445, 0.1524] | 0.002603 [-0.0411, 0.0463] | -0.01428 [-0.04298, 0.01443]; match | match |
| `acf_abs_10s_lag9` | 0.03944 [-0.09029, 0.2006] | -0.01646 [-0.05045, 0.01753] | -0.01494 [-0.05517, 0.02528]; match | match |
| `acf_abs_10s_lag10` | -0.05842 [-0.2643, 0.1295] | -0.01761 [-0.05656, 0.02134] | 0.02646 [-0.0006733, 0.0536]; match | match |
| `sign_acf_lag1` | 0.07836 [0.0611, 0.0908] | -0.0006877 [-0.003252, 0.001877] | -0.002515 [-0.006451, 0.001422]; miss (sim/data = -0.03209) | miss (sim/data = -0.008776) |
| `sign_acf_lag10` | 0.07938 [0.0488, 0.09942] | -0.001791 [-0.004074, 0.0004915] | -0.002394 [-0.005772, 0.0009836]; close | close |
| `sign_acf_lag100` | 0.08269 [0.06219, 0.09778] | -0.0007549 [-0.003443, 0.001933] | 0.00245 [-0.001723, 0.006623]; miss (sim/data = 0.02963) | miss (sim/data = -0.009129) |
| `fill_le0` | 0.6932 [0.6676, 0.7233] | 0.6737 [0.6722, 0.6753] | 0.8599 [0.8574, 0.8624]; miss (sim/data = 1.241) | match |
| `fill_1_2` | 0.284 [0.2643, 0.3081] | 0.3201 [0.3185, 0.3216] | 0.232 [0.2287, 0.2354]; close | close |
| `fill_3_9` | 0.03646 [0.02434, 0.05136] | 0.006325 [0.006072, 0.006578] | 0.001511 [0.001285, 0.001737]; close | close |
| `fill_ge10` | n/a [n/a] | 0 [0, 0] | n/a [n/a]; n/a | n/a |
| `fill_all` | 0.1964 [0.1817, 0.2107] | 0.1901 [0.1895, 0.1907] | 0.1509 [0.1502, 0.1516]; miss (sim/data = 0.7685) | match |
| `executed_share` | 0.1935 [0.1785, 0.2079] | 0.1885 [0.1879, 0.1891] | 0.1495 [0.1488, 0.1501]; close | match |
| `lambda_limit_buy` | 47.06 [46.01, 47.75] | 47.77 [47.71, 47.83] | 21.23 [21.18, 21.28]; miss (sim/data = 0.4511) | match |
| `lambda_limit_sell` | 46.85 [46.06, 47.39] | 47.63 [47.54, 47.73] | 21.25 [21.2, 21.3]; miss (sim/data = 0.4536) | close |
| `lambda_limit_total` | 93.91 [92.13, 95.03] | 95.4 [95.3, 95.51] | 42.48 [42.42, 42.54]; miss (sim/data = 0.4524) | close |
| `lambda_market_buy` | 10.8 [8.366, 13.27] | 9.794 [9.753, 9.836] | 3.371 [3.351, 3.391]; miss (sim/data = 0.312) | match |
| `lambda_market_sell` | 8.552 [7.149, 9.604] | 9.207 [9.172, 9.243] | 3.363 [3.343, 3.384]; miss (sim/data = 0.3933) | match |
| `lambda_market_total` | 19.36 [17.77, 20.6] | 19 [18.95, 19.06] | 6.734 [6.708, 6.761]; miss (sim/data = 0.3479) | match |

### Cross-ticker scorecard: calibrated on SYNTHA, validated on SYNTHB (SYNTHETIC data, NOT real market data)

Verdicts, calibrated simulation: Calibrated: 16 match, 14 close, 22 miss, 1 undefined. Baseline: 16 match, 12 close, 24 miss, 1 undefined.

| metric | data (test) [95% interval] | calibrated sim [95% CI of mean] | baseline sim [95% CI of mean]; its verdict | calibrated verdict |
|---|---|---|---|---|
| `spread_mean` | 2.032 [2.001, 2.059] | 1.916 [1.912, 1.921] | 2.225 [2.22, 2.23]; miss (sim/data = 1.095) | miss (sim/data = 0.9432) |
| `spread_one_tick_share` | 0.01287 [0.001989, 0.03156] | 0.1818 [0.1784, 0.1852] | 0.0003555 [0.000162, 0.000549]; close | miss (sim/data = 14.13) |
| `one_sided_share` | 0 [0, 0] | 0 [0, 0] | 0 [0, 0]; match | match |
| `depth_buy_L1` | 116.9 [60.54, 168.6] | 73.94 [72.41, 75.47] | 66.87 [64.17, 69.58]; match | match |
| `depth_buy_L2` | 339.9 [204.1, 469.7] | 221.6 [217.8, 225.4] | 216.9 [210.1, 223.6]; match | match |
| `depth_buy_L3` | 500.3 [378, 618.9] | 306 [302.4, 309.6] | 318.6 [313.3, 323.8]; close | close |
| `depth_buy_L4` | 582 [505.9, 656.5] | 336.6 [334, 339.2] | 347.9 [345.6, 350.3]; miss (sim/data = 0.5979) | miss (sim/data = 0.5784) |
| `depth_buy_L5` | 633.7 [590.4, 681.3] | 342.2 [339.8, 344.5] | 350.5 [348.4, 352.6]; miss (sim/data = 0.5531) | miss (sim/data = 0.5399) |
| `depth_buy_L6` | 655.8 [638.7, 684] | 343.2 [341.1, 345.3] | 351.1 [348.8, 353.3]; miss (sim/data = 0.5353) | miss (sim/data = 0.5233) |
| `depth_buy_L7` | 655.2 [642.6, 676.8] | 343.8 [342, 345.6] | 351.3 [348.8, 353.8]; miss (sim/data = 0.5362) | miss (sim/data = 0.5247) |
| `depth_buy_L8` | 646.4 [625.6, 674.9] | 332.5 [330.1, 334.9] | 348.7 [345.6, 351.7]; miss (sim/data = 0.5394) | miss (sim/data = 0.5144) |
| `depth_buy_L9` | 594 [535.4, 660] | 291.7 [288.3, 295] | 320 [316, 324]; miss (sim/data = 0.5388) | miss (sim/data = 0.491) |
| `depth_buy_L10` | 478.4 [355.9, 604.1] | 194.9 [190.9, 198.8] | 216.4 [210.7, 222]; close | close |
| `depth_sell_L1` | 165.9 [97.4, 237.9] | 50.56 [49.06, 52.05] | 67.26 [64.38, 70.15]; close | close |
| `depth_sell_L2` | 442.2 [314.1, 578.6] | 163.8 [160.9, 166.8] | 215.9 [210.8, 221]; close | close |
| `depth_sell_L3` | 572.4 [494.5, 658] | 259.2 [255.6, 262.8] | 316.3 [312.8, 319.9]; miss (sim/data = 0.5526) | miss (sim/data = 0.4529) |
| `depth_sell_L4` | 638.5 [610.6, 675.4] | 314.6 [311.5, 317.8] | 347.8 [344.8, 350.8]; miss (sim/data = 0.5447) | miss (sim/data = 0.4928) |
| `depth_sell_L5` | 657.5 [647.1, 674] | 337.3 [334.8, 339.8] | 351.3 [348.8, 353.7]; miss (sim/data = 0.5342) | miss (sim/data = 0.513) |
| `depth_sell_L6` | 655.7 [630.4, 682.7] | 345.4 [343.1, 347.7] | 351.4 [348.5, 354.2]; miss (sim/data = 0.5359) | miss (sim/data = 0.5268) |
| `depth_sell_L7` | 627.8 [588.9, 668.5] | 346.4 [344.1, 348.8] | 350.3 [347.4, 353.2]; miss (sim/data = 0.558) | miss (sim/data = 0.5518) |
| `depth_sell_L8` | 583.8 [515.8, 647.9] | 342.1 [340.1, 344.1] | 348.4 [345.1, 351.8]; miss (sim/data = 0.5968) | miss (sim/data = 0.586) |
| `depth_sell_L9` | 509.1 [392, 619.5] | 320.2 [317.9, 322.5] | 321 [315.8, 326.2]; close | close |
| `depth_sell_L10` | 373.4 [218.9, 525.7] | 247.5 [244.6, 250.5] | 217.2 [210.2, 224.1]; match | match |
| `vol_1s` | 5.976e-05 [5.741e-05, 6.094e-05] | 6.777e-05 [6.708e-05, 6.847e-05] | 4.932e-05 [4.889e-05, 4.975e-05]; miss (sim/data = 0.8252) | miss (sim/data = 1.134) |
| `vol_10s` | 9.242e-05 [8.012e-05, 9.421e-05] | 0.0001032 [0.0001001, 0.0001062] | 8.198e-05 [7.984e-05, 8.413e-05]; match | close |
| `vol_60s` | 0.0003196 [0.0001871, 0.0003429] | 0.0001305 [0.0001221, 0.0001388] | 0.0001168 [0.0001097, 0.0001239]; close | close |
| `kurtosis_10s` | -0.6296 [-0.9634, 0.3695] | -0.1854 [-0.3461, -0.02459] | -0.2423 [-0.3627, -0.1219]; match | match |
| `acf_ret_1s_lag1` | -0.354 [-0.3873, -0.3137] | -0.3116 [-0.3202, -0.3029] | -0.2625 [-0.2751, -0.2499]; close | match |
| `acf_abs_10s_lag1` | -0.08165 [-0.2292, -0.007125] | 0.07772 [0.04267, 0.1128] | 0.02087 [-0.01262, 0.05436]; match | close |
| `acf_abs_10s_lag2` | -0.04553 [-0.2113, -0.002475] | 0.01327 [-0.02659, 0.05313] | -0.021 [-0.05789, 0.01589]; match | match |
| `acf_abs_10s_lag3` | 0.01444 [-0.1203, 0.1793] | -0.01707 [-0.0537, 0.01955] | -0.02421 [-0.05774, 0.009326]; match | match |
| `acf_abs_10s_lag4` | -0.04962 [-0.2251, 0.1118] | -0.02732 [-0.05733, 0.002679] | -0.005434 [-0.04088, 0.03002]; match | match |
| `acf_abs_10s_lag5` | 0.06373 [-0.07189, 0.2811] | 0.01684 [-0.01618, 0.04985] | -0.007598 [-0.03879, 0.0236]; match | match |
| `acf_abs_10s_lag6` | -0.1291 [-0.2938, 0.03859] | 0.01426 [-0.01875, 0.04728] | -0.01486 [-0.04801, 0.01829]; match | match |
| `acf_abs_10s_lag7` | -0.0379 [-0.1813, 0.1073] | -0.04006 [-0.07471, -0.00542] | 0.01156 [-0.0185, 0.04163]; match | match |
| `acf_abs_10s_lag8` | 0.01928 [-0.1445, 0.1524] | -0.002652 [-0.04126, 0.03596] | -0.01428 [-0.04298, 0.01443]; match | match |
| `acf_abs_10s_lag9` | 0.03944 [-0.09029, 0.2006] | -0.01 [-0.03632, 0.01632] | -0.01494 [-0.05517, 0.02528]; match | match |
| `acf_abs_10s_lag10` | -0.05842 [-0.2643, 0.1295] | -0.01046 [-0.03986, 0.01894] | 0.02646 [-0.0006733, 0.0536]; match | match |
| `sign_acf_lag1` | 0.07836 [0.0611, 0.0908] | -0.0003188 [-0.003347, 0.00271] | -0.002515 [-0.006451, 0.001422]; miss (sim/data = -0.03209) | miss (sim/data = -0.004069) |
| `sign_acf_lag10` | 0.07938 [0.0488, 0.09942] | 0.0008624 [-0.002351, 0.004075] | -0.002394 [-0.005772, 0.0009836]; close | close |
| `sign_acf_lag100` | 0.08269 [0.06219, 0.09778] | -0.002384 [-0.005369, 0.0006005] | 0.00245 [-0.001723, 0.006623]; miss (sim/data = 0.02963) | miss (sim/data = -0.02883) |
| `fill_le0` | 0.6932 [0.6676, 0.7233] | 0.6352 [0.6338, 0.6366] | 0.8599 [0.8574, 0.8624]; miss (sim/data = 1.241) | close |
| `fill_1_2` | 0.284 [0.2643, 0.3081] | 0.2786 [0.2761, 0.2812] | 0.232 [0.2287, 0.2354]; close | match |
| `fill_3_9` | 0.03646 [0.02434, 0.05136] | 0.00635 [0.005958, 0.006741] | 0.001511 [0.001285, 0.001737]; close | close |
| `fill_ge10` | n/a [n/a] | 0 [0, 0] | n/a [n/a]; n/a | n/a |
| `fill_all` | 0.1964 [0.1817, 0.2107] | 0.1695 [0.1688, 0.1702] | 0.1509 [0.1502, 0.1516]; miss (sim/data = 0.7685) | close |
| `executed_share` | 0.1935 [0.1785, 0.2079] | 0.1668 [0.1661, 0.1674] | 0.1495 [0.1488, 0.1501]; close | close |
| `lambda_limit_buy` | 47.06 [46.01, 47.75] | 24 [23.96, 24.04] | 21.23 [21.18, 21.28]; miss (sim/data = 0.4511) | miss (sim/data = 0.51) |
| `lambda_limit_sell` | 46.85 [46.06, 47.39] | 23.86 [23.81, 23.91] | 21.25 [21.2, 21.3]; miss (sim/data = 0.4536) | miss (sim/data = 0.5093) |
| `lambda_limit_total` | 93.91 [92.13, 95.03] | 47.86 [47.81, 47.92] | 42.48 [42.42, 42.54]; miss (sim/data = 0.4524) | miss (sim/data = 0.5097) |
| `lambda_market_buy` | 10.8 [8.366, 13.27] | 4.057 [4.032, 4.082] | 3.371 [3.351, 3.391]; miss (sim/data = 0.312) | close |
| `lambda_market_sell` | 8.552 [7.149, 9.604] | 4.357 [4.335, 4.38] | 3.363 [3.343, 3.384]; miss (sim/data = 0.3933) | miss (sim/data = 0.5095) |
| `lambda_market_total` | 19.36 [17.77, 20.6] | 8.415 [8.385, 8.444] | 6.734 [6.708, 6.761]; miss (sim/data = 0.3479) | miss (sim/data = 0.4347) |

The cross-ticker run uses the calibration estimated on SYNTHA's training half and starts the simulation at SYNTHB's median mid-price (10021 ticks); the yardstick is SYNTHB's test half.

### Distributions against the data's own halves (SYNTHETIC)

| run | quantity | data halves | halves, bootstrap upper | calibrated | baseline |
|---|---|---|---|---|---|
| SYNTHA | spread W1 (ticks) | 0.0713 | 0.09685 | 0.228 | 0.2047 |
| SYNTHA | depth buy TV | 0.06724 | 0.1212 | 0.03305 | 0.02557 |
| SYNTHA | depth sell TV | 0.06547 | 0.1299 | 0.05246 | 0.01899 |
| SYNTHB | spread W1 (ticks) | 0.06423 | 0.0841 | 0.2267 | 0.1933 |
| SYNTHB | depth buy TV | 0.148 | 0.1907 | 0.01966 | 0.03432 |
| SYNTHB | depth sell TV | 0.156 | 0.1969 | 0.04242 | 0.02997 |
| SYNTHA to SYNTHB | spread W1 (ticks) | 0.06423 | 0.0841 | 0.2224 | 0.1933 |
| SYNTHA to SYNTHB | depth buy TV | 0.148 | 0.1907 | 0.042 | 0.03432 |
| SYNTHA to SYNTHB | depth sell TV | 0.156 | 0.1969 | 0.06145 | 0.02997 |

### What matches, what is close, what misses

Counts (calibrated flow; baseline in brackets): SYNTHA 31 match, 17 close, 4 miss, 1 undefined (29, 12, 11); SYNTHB 36 match, 12 close, 4 miss, 1 undefined (16, 12, 24); cross-ticker 16 match, 14 close, 22 miss, 1 undefined (16, 12, 24).

The undefined row in every table is `fill_ge10`: no limit order in the data was placed 10 or more ticks from the touch inside the window, so the data has no value for it.

**SYNTHA, matches (31).** Most depth levels on both sides (the rest are close; the normalised depth profile is within the halves yardstick, as for the baseline), `vol_60s`, kurtosis, eight of the ten autocorrelations of |r| (lags 1 and 5 are close), `executed_share` and all six order-rate rows. The lifetime curves are reproduced: median lifetime by distance bucket, training data / calibrated / baseline, in seconds: <=0 8.21 / 7.69 / 22.08; 1-2 13.54 / 13.63 / 21.14; 3-9 20.86 / 20.77 / 20.95 (the baseline's exponential lifetimes give about 21 s everywhere).

**SYNTHA, close (17).** `vol_1s`, `vol_10s`, the lag-1 bounce, the sign autocorrelations at lags 1 and 10, `fill_le0` (0.6882 in the data, 0.6352 in the simulation), `fill_3_9`, `fill_all` and the depth levels not listed above: the gaps are within twice the data interval's half-width. The trade-sign autocorrelation is small in the data (0.04395 at lag 1, from the informed trader: it disappears without it, see the ablation below) and about 0 in the simulation; it reads "close" only because the data interval is wide, not because the flow has sign memory. It is an expected miss by construction.

**SYNTHA, misses (4).** (1) `spread_mean`: data 2.02 ticks, calibrated 1.916 (ratio 0.9485). The baseline misses in the other direction (2.225). (2) `spread_one_tick_share`: 0.01574 of two-sided time at one tick in the data, 0.1818 in the calibrated simulation. The spread PMF is beyond the halves yardstick for both models (W1 0.228 calibrated, 0.2047 baseline, against an upper bound of 0.09685). (3) `fill_1_2`: 0.2536 of the data's orders placed 1 or 2 ticks away were filled, 0.2786 in the simulation. (4) `sign_acf_lag100`: 0.05712 against -0.002384, an expected miss.

**SYNTHB, misses (4)** (stronger informed trader). `spread_one_tick_share` misses as on SYNTHA; `spread_mean` is close (2.032 against 1.961) and `fill_le0` matches (0.6932 against 0.6737). In addition `sign_acf_lag1` is 0.07836 in the data and -0.0006877 in the calibrated simulation, `sign_acf_lag100` is 0.08269 against -0.0007549, and `vol_1s` is 5.976e-05 in the data against 8.122e-05 in the simulation. The last one is too *high* in the simulation, so it is not volatility that the flow lacks. The baseline misses far more rows on SYNTHB (24 against 4), mostly because it runs the default noise traders' rates on a day whose noise rate was doubled; it is not a like-for-like contest on this day.

**Cross-ticker, SYNTHA calibration on SYNTHB.** SYNTHB trades about twice as fast as SYNTHA (limit rate 93.91 against 47.86 per second in the simulation), so the rate rows miss by a factor of about two (`lambda_limit_total` ratio 0.5097), and the deeper levels of the book are about half as full (`depth_buy_L6` ratio 0.5233). Both spread rows miss. This is the expected outcome of a calibration that does not carry over to a faster market: it measures how general the flow is, and here it is not.

### What causes the misses: an ablation (SYNTHETIC)

`scripts/calibration_ablation.sh` takes each synthetic day apart: the full day, the same day without the market maker, without the informed trader, and with the noise traders alone. Each variant is calibrated on its own first half and validated on its own second half; the `full` column is the dress rehearsal above. One simulated day per variant, so it shows which ingredient a miss comes with, not how large the effect is on average.

**SYNTHA** (SYNTHETIC data, NOT real market data; 20 seeds)

Each cell: data (test half) / calibrated simulation, verdict.

| metric | full | no_market_maker | no_informed | noise_only |
|---|---|---|---|---|
| `spread_mean` | 2.02 / 1.916, miss | 2.348 / 2.385, close | 1.993 / 1.862, close | 2.229 / 2.243, match |
| `spread_one_tick_share` | 0.01574 / 0.1818, miss | 0.0002525 / 0.01076, miss | 0.028 / 0.1978, miss | 0.0001917 / 0.001124, match |
| `fill_le0` | 0.6882 / 0.6352, close | 0.8784 / 0.8893, close | 0.7194 / 0.6638, close | 0.8577 / 0.8591, match |
| `executed_share` | 0.1702 / 0.1668, match | 0.1814 / 0.1832, match | 0.1419 / 0.1424, match | 0.1484 / 0.1531, close |
| `vol_1s` | 6.179e-05 / 6.807e-05, close | 5.431e-05 / 6.096e-05, close | 5.169e-05 / 5.579e-05, match | 4.847e-05 / 5.099e-05, close |
| `vol_60s` | 0.0001904 / 0.000131, match | 0.0002094 / 0.0001414, close | 0.0001279 / 0.0001212, match | 0.0001164 / 0.0001229, match |
| `sign_acf_lag1` | 0.04395 / -0.0003188, close | 0.04585 / 0.0005688, close | 0.007292 / 0.003771, match | 0.005217 / -0.0008536, match |

**SYNTHB** (SYNTHETIC data, NOT real market data; 20 seeds)

Each cell: data (test half) / calibrated simulation, verdict.

| metric | full | no_market_maker | no_informed | noise_only |
|---|---|---|---|---|
| `spread_mean` | 2.032 / 1.961, close | 2.314 / 2.495, miss | 2.014 / 1.886, miss | 2.194 / 2.188, match |
| `spread_one_tick_share` | 0.01287 / 0.1614, miss | 0.0009608 / 0.002162, match | 0.01035 / 0.1652, miss | 0.0005709 / 0.001615, match |
| `fill_le0` | 0.6932 / 0.6737, match | 0.8865 / 0.9133, miss | 0.738 / 0.6949, close | 0.8713 / 0.8761, match |
| `executed_share` | 0.1935 / 0.1885, match | 0.2074 / 0.2063, match | 0.1436 / 0.1416, match | 0.149 / 0.15, match |
| `vol_1s` | 5.976e-05 / 8.122e-05, miss | 6.029e-05 / 7.563e-05, miss | 5.934e-05 / 6.086e-05, match | 5.577e-05 / 5.559e-05, match |
| `vol_60s` | 0.0003196 / 0.0001388, close | 0.0003268 / 0.0001471, close | 0.0001042 / 0.0001128, match | 0.0001259 / 0.0001145, match |
| `sign_acf_lag1` | 0.07836 / -0.0006877, miss | 0.08739 / 9.425e-05, miss | -0.005137 / 0.001001, match | 0.0004896 / 0.001081, match |

- **The market maker is behind the one-tick share.** With it, the calibrated simulation is at one tick for 16 to 20% of the time against 1.0 to 2.8% of the data (test half); without it both are at one tick for about 1% of the time or less. With the noise traders alone no row of the full scorecard misses on either day (SYNTHA 42 match, 10 close; SYNTHB 52 match), so the generator reproduces a flow of the kind it models. The fill rows show no large market-maker effect: the calibrated `fill_le0` differs from the data by -7.7% (SYNTHA) and -2.8% (SYNTHB) with the market maker, and by +1.2% and +3.0% without it (SYNTHB's still reads miss: both intervals are narrow). An earlier version of this section reported a 25 to 28% gap and blamed the market maker; that was a measurement error in the fingerprint, which ignored orders that reuse the id of an ended order (D-395).
- **The synthetic days are not stationary.** The one-tick share falls from 33% (SYNTHA) and 36% (SYNTHB) of the first five minutes to 0% of the last fifteen (17.2% of SYNTHA's training half, 1.6% of its test half). It needs the market maker, whose inventory skew is proportional to the time left in the session, which is consistent with the decline; that mechanism was not isolated further. The spread of the two full days is therefore driven by a day that changes as much as by the flow. The calibrated simulation (18.2%) reproduces the half it was fitted on; the test half had changed. An out-of-sample miss of this kind is a property of the day as much as of the flow.
- **The informed trader is behind the sign memory, and behind `vol_1s` on SYNTHB.** Without it the sign autocorrelation at lag 1 is about zero in the data and matches, and `vol_1s` matches. With it, the simulation is noisier than the data at 1 s and less volatile at 60 s, where the data's volatility is 1.5 times (SYNTHA) and 2.3 times (SYNTHB) the simulation's. A likely reading, not tested: its orders enter the calibrated flow at their measured rate but without their direction, so they add noise instead of trend. The 60 s rows read "match" or "close" only because their data interval is wide.

Plots (spread PMF, depth profile, survival curves, volatility against sampling interval) are drawn by every run: `build/real_data/calibration/<pair>/plots/`. In them the data survival curve is the training period's Kaplan-Meier curve, because the estimator never reads the test period.

**Reading the verdicts.** With 20 seeds the simulation interval is narrow, and the rule asks for an overlap with the data's bootstrap interval, which is wide for 30 minutes of data; "close" is a gap between the intervals of up to twice the data interval's half-width, that is, one full width. Nothing was tuned to improve any row.

## 9. Known gaps

Said plainly, with nothing tuned to hide them:

- **Trade-sign memory and volatility clustering are expected misses.** The calibrated flow is zero-intelligence: each order's side is drawn independently, so the autocorrelation of aggressive-order signs is about zero by construction, while real signs have long memory [9]. Volatility clustering needs state-dependent behaviour the flow does not have. On the synthetic days the sign autocorrelation of the data comes from the informed trader, which the calibrated flow runs without.
- **No state dependence.** Rates and placements do not depend on the book state, as in queue-reactive models [10], and there is no intraday rate profile. The measured rates are one average over the window.
- **What the dress rehearsal showed (synthetic).** `spread_one_tick_share` misses on both synthetic days and the spread distribution is beyond the halves yardstick for both models. The ablation in section 8 ties that row to the market maker, and the share of time at one tick also changes within the day, so part of that miss is the test half differing from the training half. With the noise traders alone no row of the full scorecard misses. On SYNTHB the informed trader's footprint shows in the sign autocorrelation and in `vol_1s`, which the simulation overstates.
- **Known estimator bias.** With price-window-filtered data, $\lambda_L$ and the far tail of the placement PMF are underestimated (section 3, measured in section 5).
- **A day where a spread bucket is never visited** gives the estimator nothing for that bucket: the simulator draws from the nearest bucket of the same side that has samples, counts every such draw, and `validate` names the empty buckets (D-385). A side with no placement samples at all is still refused.

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
