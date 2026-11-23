## Parameter recovery

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

Two of these choices were made after looking at the simulated market, because the first attempt could not test what it was meant to test:

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

The export real data resembles: the price-window filter at L = 10. The rates and the tails are biases to be measured; the judged rows use the filtered rules and all pass on this seed (the ≥ 10 median passes its 20% rule on 198 cancellations, which is luck as much as accuracy: see above).

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
