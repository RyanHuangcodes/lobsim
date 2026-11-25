# Module 11: Keeping score

> **In one sentence:** a silent observer watches the true market, keeps the market maker's books, measures how prices moved after each of its trades, and summarises the price's behaviour with a few statistics, and then the README reads those numbers honestly.
>
> **You will be able to:** compute mark-to-market PnL, markouts, spread capture and mid move; explain adverse selection with lobsim's numbers; say what a 95% confidence interval means and when it lies; compute log returns, excess kurtosis and autocorrelation; name the stylized facts and say which ones lobsim reproduces; and recompute the main numbers of `summary.json` yourself.
>
> **Time:** about 5 hours.

## 11.1 Measure the truth, not the beliefs

The market maker *believes* things (its inventory, its resting orders) from late messages. The scorekeeper, `MetricsObserver` (`src/sim/metrics.cpp`), is an observer (Module 8.7): it sees every engine event and the true top of book with zero delay, and computes everything from that ground truth (D-100). It never schedules, sends or draws random numbers, so attaching it changes nothing.

Most statistics cover only the **window** after the warm-up: from `warmup_s` = 60 s to the end. The first minute, when the book is still filling up (Lab 10.2), is left out.

## 11.2 Profit and loss, marked to market

The MM's money is tracked in two parts:

- **cash**, in ticks × lots, a whole number: selling 5 lots at 10,000 adds 50,000; buying subtracts;
- **inventory** `q`, in lots.

Plus **fees**, a decimal: `qty × maker_fee` when the MM was the maker, `qty × taker_fee` when it was the taker. In the default config the maker fee is −0.002 ticks per lot (a negative fee is a **rebate**: the exchange pays makers to provide liquidity) and the taker fee is 0.003. The MM uses post-only orders, so it is always the maker.

**Mark-to-market PnL** values the inventory at the current mid:

$$\text{PnL}(t) = \text{cash} + q \times \text{mid}(t) - \text{fees}$$

It answers "if everything I hold were worth the mid right now, how much have I made?" (D-102). The window PnL is PnL(end) − PnL(warm-up).

Code, `record_fill` (line 255):

```cpp
void MetricsObserver::record_fill(TimeNs now, const lob::Trade& t, lob::Side side, bool maker) {
  if (side == lob::Side::Buy) {
    cash_ -= t.price * t.qty;
    inventory_ += t.qty;
  } else {
    cash_ += t.price * t.qty;
    inventory_ -= t.qty;
  }
  fees_ += static_cast<double>(t.qty) * (maker ? opt_.maker_fee_ticks : opt_.taker_fee_ticks);
  ...
```

and `pnl_now` (line 182): `cash_ + inventory_ * last_mid_.value_or(0.0) - fees_`.

## 11.3 Markouts: did the price move against me?

The spread is only earned if the price does not move against you afterwards. A **markout** measures that, per lot, for each fill and a **horizon** h (1, 5 and 30 seconds in the default config):

- buy at price p at time t: markout(h) = mid(t + h) − p
- sell at price p: markout(h) = p − mid(t + h)

Positive means "a little later, the trade looks good". Negative means the MM bought just before the price fell, or sold just before it rose: it was **picked off**. SPEC.md 6: "Negative markouts = adverse selection."

lobsim splits each markout into two parts using `mid_before`, the mid in force just before the request that caused the trade (D-112 to D-115):

- **spread capture** = mid_before − p for a buy (p − mid_before for a sell): what the trade earned against the price at the moment it happened;
- **mid move**(h) = mid(t + h) − mid_before for a buy (the mirror for a sell): how the price moved afterwards.

They add up: markout = spread capture + mid move, exactly, for every fill.

### The default run's markouts (README)

| Horizon | Fills | Markout per lot | 95% interval | Spread capture | Mid move |
|---|---|---|---|---|---|
| 1 s | 7,576 | +0.499 | +0.483 to +0.515 | +0.959 | −0.460 |
| 5 s | 7,572 | +0.494 | +0.475 to +0.514 | +0.959 | −0.464 |
| 30 s | 7,524 | +0.518 | +0.485 to +0.551 | +0.959 | −0.441 |

The MM earns almost 1 tick per lot against the pre-trade mid, then loses about 0.46 ticks of it to the price moving. Is that adverse selection? Mostly not, the README argues. The mid move is already complete after 1 second and does not grow. When someone takes the MM's quote at the best price, that quote disappears, and the mid **mechanically** jumps half a tick or more away from the MM. Fills against noise traders, who know nothing, show the same −0.44. That is not information, it is the book's shape.

### Adverse selection appears when you split by counterparty

Average over everyone and the informed trader's effect is hidden. Split the MM's fills by who took them (README, 8 seeds, lot-weighted):

| Taker | Share of the MM's lots | Spread capture | Mid move 1 s | Mid move 30 s | Markout 30 s |
|---|---|---|---|---|---|
| informed | 0.168 | +0.940 | −0.667 | −1.617 | **−0.678** |
| noise | 0.832 | +0.951 | −0.445 | −0.266 | +0.685 |

Against the informed trader, the price keeps moving against the MM for at least 30 seconds: that is information, not bounce, and those fills lose 0.68 ticks per lot. They are only 17% of its volume, so the total stays positive. With a stronger informed trader (`lambda` 10, `size` 20), the informed fills lose 2.5 to 2.9 ticks per lot and PnL **per lot** falls from 0.497 (no informed trader) to 0.384, though total PnL does not fall, because the informed trader's orders also bring volume.

### Markouts are computed lazily

A markout at t + 30 s cannot be known at time t. `advance` (line 198) runs at every event and resolves, in time order, every fill whose horizon has passed:

```cpp
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    auto& queue = pending_[i];
    auto& head = pending_head_[i];
    while (head < queue.size() && fills_[queue[head]].time + opt_.markout_horizons_ns[i] < now) {
      Fill& f = fills_[queue[head]];
      if (mid_) f.markouts[i] = stats::markout_per_lot(f.side, f.price, *mid_);
      ++head;
    }
  }
```

One queue of fill indices per horizon, and a `head` position that only moves forward: each fill is resolved exactly once, and no search is needed, because fills arrive in time order. If the mid is undefined at t + h (a one-sided book), that sample is skipped and counted, never invented (D-106).

## 11.4 How sure are we? Confidence intervals

The table gives "+0.499, 95% interval +0.483 to +0.515". The interval is `mean ± 1.96 × sd / √n`: the standard deviation of the values divided by the square root of their count measures how much the **mean** would wobble if the experiment were repeated; 1.96 is the number for "95%" under a bell curve.

It rests on an assumption: that the fills are **independent**. They are not: fills come in clusters (one market order can hit the MM and others in a row), and 30-second horizons of neighbouring fills overlap, so they share the same price move. The README says so: the intervals "are likely too narrow". This is an example of what honest reporting looks like: give the number, give the uncertainty, and say what the uncertainty assumes. The multi-seed tables (8 seeds, with the range over seeds) are a sturdier check.

## 11.5 Time-weighted statistics

"What fraction of the time was the spread 2 ticks?" could be answered by sampling once a second. lobsim does better: at every state change, `advance` adds the exact number of nanoseconds since the previous change to a counter for the current spread (D-104):

```cpp
  const TimeNs lo = std::max(last_time_, opt_.warmup_ns);
  const TimeNs hi = std::min(now, opt_.end_ns);
  if (hi > lo) {
    const TimeNs dt = hi - lo;
    if (top_.two_sided()) spread_time_[top_.ask - top_.bid] += dt;
    if (quoting_both_) quoted_spread_time_[quoted_spread_] += dt;
  }
```

`spread_time_` is a `std::map` from spread (ticks) to nanoseconds. Its weighted mean is the time-weighted mean spread (1.94 ticks), and a weighted quantile gives the median (D-105). This is exact to the nanosecond, with no sampling. The same trick gives the MM's quoting fraction (99.93%) and average quoted spread (2.70).

## 11.6 Returns and the stylized facts

Real markets share some statistical habits, called **stylized facts** (Cont, 2001; SPEC.md cites a recent re-check). lobsim **measures** them and does not tune anything to fake them. They are measured on the mid sampled once a second in the window.

- **Log return**: r_t = ln(m_t / m_{t−1}), the percentage-like change from one second to the next. lobsim computes a return only between two consecutive samples that both have a mid.
- **Heavy (fat) tails**: big moves are more common than a bell curve predicts. Measured by **excess kurtosis**, m4/m2² − 3 (m2 and m4 are the average squared and fourth-power deviations from the mean). It is 0 for a bell curve and positive for fat tails.
- **No autocorrelation of returns**: today's return says almost nothing about tomorrow's. The **autocorrelation** at lag k measures how much r_t and r_{t+k} move together: +1 always together, −1 always opposite, 0 unrelated.
- **Volatility clustering**: big moves follow big moves. Measured by the autocorrelation of |r_t|, which in real markets stays positive for a long time.

What lobsim found (README, 8 seeds):

| Stylized fact | Present? | Evidence |
|---|---|---|
| Heavy tails | Weakly | excess kurtosis 0.8 to 1.4, far below real high-frequency returns |
| No autocorrelation of returns | No, at lag 1 | lag-1 autocorrelation −0.24 to −0.37: **bid-ask bounce**. The mid lives on a half-tick grid with a 1 to 3 tick spread, and about half of all one-second returns are exactly 0. By lag 3 it is −0.02 to −0.05 |
| Volatility clustering | No, in the usual sense | |r| autocorrelation 0.14 to 0.18 at lag 1, but 0.01 by lag 10, and present with noise traders alone |

**Bid-ask bounce**, in plain words: if trades alternate between the bid and the ask, the price ticks up, down, up, down, so a rise is usually followed by a fall. That is negative autocorrelation produced by the book's mechanics, not by anyone's view. Copycat in Lab 9.4 traded *with* the bounce and lost.

The README's conclusion: zero-intelligence flow reproduces the bid-ask bounce and not fat tails or long-memory volatility, which is the expected result for this kind of model.

## 11.7 The output files, written deterministically

`summary.json` echoes the config, the seed and the engine event hash, then every statistic. Doubles are rounded through `"%.6f"` (or `"%.6e"` for tiny numbers) so the printed text never depends on the details of how a library prints the shortest form of a number, undefined values are `null`, and nothing machine-specific is written: no wall-clock time, no host name, no file path (D-109; `Determinism.TheSummaryHoldsNoWallClockPathOrHostName` tests it). Two runs with the same seed therefore produce byte-identical files.

**Skipped on purpose:** `summary()` (line 367) turns the collected data into the statistics, block by block (MM, markouts, spread capture, spread, returns, counts), all with the helper functions at the top of the file; `write_trades_csv`, `write_bbo_csv` and `write_mm_fills_csv` (lines 523 to 640) print one line per row; `summary_json` (line 642) builds the JSON. The helper functions in `namespace stats` (lines 19 to 120) each have a worked example in `tests/unit/sim_metrics_test.cpp`.

## Lab 11.1: recompute the market maker's PnL from the trades

```sh
python3 course/labs/py/mm_pnl.py out/run1
```

It replays the bookkeeping of `record_fill` over `trades.csv` and compares with `summary.json`. When this course was written:

```
MM owner id          12
lots traded          27164
cash                 351008   (summary: 351008)
inventory            -34   (summary: -34)
last mid             9964.0
fees                 -54.3280   (summary: -54.328)
PnL                  12286.3280   (summary: 12286.328)
PnL per lot traded   0.4523 ticks
```

1. The cash is +351,008 ticks and the PnL is about +12,286. Where did the other ~339,000 go?
2. Why are the fees negative, and do they matter much?
3. The README says +12,263 ticks for the market maker. Why is that different from 12,286?

<details><summary>Answers</summary>

1. The MM is short 34 lots worth 34 × 9,964 = 338,776 ticks at the last mid. Cash alone is not profit: the MM owes those shares.
2. Every MM fill was as maker, and makers get a rebate of 0.002 ticks per lot: 27,164 × 0.002 = 54.3 ticks paid *to* the MM. That is less than 0.5% of its PnL.
3. 12,263 is the **window** PnL (after the 60 s warm-up): `pnl_after_warmup`. 12,286 is the whole run.

</details>

## Lab 11.2: markouts and who picked off the market maker

```sh
python3 course/labs/py/markouts.py out/run1
```

When this course was written:

```
7577 market-maker fills after the warm-up

markout_1s   n 7576  mean +0.4991  95% interval [+0.4832, +0.5151]   (summary: +0.4991)
             = spread capture +0.9588 + mid move -0.4597
markout_5s   n 7572  mean +0.4945  95% interval [+0.4754, +0.5135]   (summary: +0.4945)
             = spread capture +0.9588 + mid move -0.4643
markout_30s  n 7524  mean +0.5179  95% interval [+0.4853, +0.5506]   (summary: +0.5179)
             = spread capture +0.9585 + mid move -0.4406

Split by the taker, 30 s horizon, weighted by lots:
  taker informed:   4389 lots, markout -0.677 ticks per lot
  taker noise   :  22111 lots, markout +0.676 ticks per lot
```

1. Why is n smaller for longer horizons (7,576, 7,572, 7,524)?
2. Change the script to also print the 1 s markout of the informed and noise groups. Is the informed trader's damage visible after 1 second already?
3. Make a run with a stronger informed trader (`lambda` 10 and `size` 20): `build/release/lobsim_run --config course/labs/configs/strong_informed.json --out out/strong_informed`. Run the script on `out/no_informed` (from Lab 7.4), `out/run1` and `out/strong_informed`. What happens to the 30 s markout of the noise group as the informed trader gets stronger, and why might that be?

<details><summary>Answers</summary>

1. A markout at t + h exists only if t + h is not after the end of the run: fills in the last h seconds are skipped (and counted as skipped). The longer the horizon, the more fills fall in that last stretch.
2. Partly: at 1 s the informed fills' mid move is about −0.67 against −0.45 for noise (README table), and it keeps growing to −1.6 at 30 s, while the noise fills' mid move shrinks. The information shows up over time.
3. It rises. When this course was written (seed 42): +0.485 with no informed trader, +0.676 in the default, +1.634 with the strong informed trader, whose own fills lost 3.27 ticks per lot. The README's 8-seed averages show the same pattern. A plausible reading: the informed trader pushes the price toward the fundamental, and a noise fill on the same side as the informed flow gains from that push. The point for you: markouts measure what happened after a fill, whatever caused it.

</details>

## Lab 11.3: stylized facts, by hand

```sh
python3 course/labs/py/returns_stats.py out/run1
```

When this course was written:

```
samples 3541, returns 3540   (summary: 3541, 3540)
share of returns exactly 0: 0.5655   (summary: 0.565537)
excess kurtosis:            1.0158   (summary: 1.01577)
return ACF, lags 1-5:       -0.353 -0.072 -0.005 -0.015 +0.003
   summary:                 -0.353 -0.072 -0.005 -0.015 +0.003
|return| ACF, lags 1-10:    +0.163 +0.103 +0.064 +0.058 +0.054 +0.026 +0.003 +0.003 +0.023 +0.027
```

1. Why are 3,541 samples taken in a 3,540-second window?
2. More than half the returns are exactly zero. What does that say about how often the mid moves in one second?
3. Which number shows the bid-ask bounce? Explain it with a three-second example of mids.
4. Run the script on `out/noise_symmetric_1` (Module 9, noise traders only). Is the bounce there too? What does that tell you about where it comes from?

<details><summary>Answers</summary>

1. Samples are taken at 60, 61, ..., 3,600 s, both ends included: 3,541 samples, so 3,540 returns.
2. In most seconds the best bid and ask end where they started (or the mid returns to the same value); the mid only changes in steps of half a tick.
3. The lag-1 autocorrelation of −0.353. Example: mids 100.0, 100.5, 100.0: an up-move followed by a down-move, because the last trade or quote change went to the ask and then back to the bid.
4. Yes (README: −0.24 to −0.28 with noise traders only). It comes from the book's mechanics (a discrete grid, a spread, random arrivals), not from any trader's intelligence.

</details>

## Lab 11.4 (optional): plots and the 89-run sweep

The plotting tool needs pandas and matplotlib. In a Python virtual environment:

```sh
python3 -m venv .venv && .venv/bin/pip install pandas matplotlib
.venv/bin/python tools/plot.py --run out/run1          # writes out/run1/plots/*.png
```

It draws the five plots in `docs/sample_run/`: mid against fundamental, MM inventory, MM PnL, the spread histogram and the return autocorrelations. Open `out/run1/plots/mm_pnl.png`: where does the PnL curve grow fastest, and does that match Lab 10.3?

The sweep needs only the standard library:

```sh
python3 tools/sweep.py --bin build/release/lobsim_run --config configs/default.json --out out/sweep
```

It runs 89 one-hour simulations (46 seconds on the machine used for this course) and prints the four multi-seed tables of the README. When this course was written it reproduced the README's tables exactly. Pick one table and write two sentences on what it shows, in your own words.

## Common questions

**Why mark inventory at the mid and not at the fundamental?** The fundamental is hidden: nobody in a real market could mark at it. The mid is the market's public price. Marking at the fundamental would credit the informed trader with knowledge nobody else has.

**Why is the 1 s mid move negative even against noise traders?** Mechanics: the MM's quote was at the best price and was just taken, so the best price on that side moves away and the mid moves against the MM by half a tick or more. It does not mean the noise traders knew anything.

**Is a 95% interval "95% sure the true value is inside"?** Roughly: if the experiment were repeated many times and the assumptions held, 95% of such intervals would contain the true mean. When the assumptions fail (here: dependent fills), the real coverage is lower.

**Why log returns instead of price differences?** A log return is a relative change, so it compares across price levels, and log returns over consecutive periods add up. With prices near 10,000 and moves of half a tick, the two are nearly proportional here anyway.

**Why does the README say the MM's profit is not evidence of anything real?** Its counterparties are mostly zero-intelligence traders who pay the spread for no reason. Real markets have much more informed flow and many market makers competing for queue position. The model shows the *mechanisms* (spread capture, adverse selection, fading inventory control), not real profitability.

## Quiz

1. Cash +100,000 ticks, inventory −10 lots, mid 9,990, fees −2. What is the PnL?
2. The MM sells 5 lots at 10,002 when the mid was 10,001; 30 s later the mid is 10,004. Compute spread capture, mid move and markout per lot.
3. Why is the overall markout positive even though the informed trader picks off the MM?
4. What does excess kurtosis 0 mean? What does lobsim's ≈ 1 suggest?
5. Explain bid-ask bounce in one sentence.
6. Why does `MetricsObserver` integrate the spread over time instead of sampling it?

<details><summary>Answers</summary>

1. 100,000 + (−10 × 9,990) − (−2) = 100,000 − 99,900 + 2 = 102 ticks.
2. Sell: spread capture = 10,002 − 10,001 = +1; mid move = mid_before − mid later = 10,001 − 10,004 = −3; markout = 10,002 − 10,004 = −2 = 1 + (−3).
3. Informed fills are only about 17% of its lots; the other 83% (noise) have positive markouts large enough to outweigh them.
4. Tails like a bell curve. About 1 means somewhat more extreme moves than a bell curve, but far less than real high-frequency returns show.
5. Trades and quote changes alternate between the bid side and the ask side, so the measured price ticks up and down and a rise tends to be followed by a fall.
6. Integrating adds the exact time spent at each spread, to the nanosecond, so short episodes between samples are counted correctly; a once-a-second sample would miss or overweight them.

</details>

## What's next

Module 12 looks at the order log that makes every run repeatable, the event hash that proves it, and how lobsim measures its own speed without fooling itself.
