# Build `lobsim`: a C++ limit order book, matching engine, and agent-based trading simulator

You are building a complete, tested, benchmarked C++20 project from scratch. Work autonomously from this spec. Where it is silent, make the simplest reasonable choice and record it in `DECISIONS.md` (one line per decision, with the reason). Do not stop to ask questions.

Bracketed numbers like [3] refer to the sources in the References section at the end. They explain *why* a rule exists; you do not need to open them.

---

## 0. How to work

1. Build in the phases of Section 9, in order. A phase is done only when its acceptance checks pass. Never weaken a test to make it pass; fix the code.
2. After each phase, run the full test suite (Debug + sanitizers) and commit. Commit messages describe the change only: **no co-author or attribution trailers.**
3. Correctness before speed. The simple `ReferenceBook` (Section 3.6) defines correct behaviour; the fast `OrderBook` must match it event for event.
4. Everything is deterministic: same config + same seed → byte-identical outputs.

---

## 1. Concepts (read first)

**Limit order book (LOB).** The list of resting buy orders (*bids*) and sell orders (*asks*) for one instrument. The *best bid* is the highest bid price, the *best ask* the lowest ask price; *spread* = best ask − best bid; *mid* = (best bid + best ask) / 2.

**Price-time priority.** An incoming order matches the best-priced resting order first; among orders at the same price, the earliest one fills first (FIFO).

**Maker / taker.** A resting order is the *maker*; the incoming order that trades against it is the *taker* (aggressor). **A trade executes at the maker's price.**

**Worked example (verify by hand).** Asks: order A sells 5 @ 101 (arrived first), B sells 3 @ 101, C sells 4 @ 102. A buy limit order for 10 @ 101 arrives:

- fills 5 vs A @ 101, then 3 vs B @ 101 (A before B: time priority);
- C @ 102 is above the buyer's limit, so matching stops;
- the remaining 2 rest as a bid at 101. Book after: best bid 101 × 2, best ask 102 × 4.

Had the buyer's limit been 105, the first two fills would still print at **101** (maker price), then 2 more vs C @ 102.

**Order types and time-in-force.**
- *Limit GTC*: match what crosses, rest the remainder.
- *Limit IOC* (immediate-or-cancel): match what crosses, cancel the remainder.
- *Limit FOK* (fill-or-kill): fill the full quantity immediately or do nothing at all.
- *Market*: match at any price, cancel any remainder (never rests).
- *Post-only*: a GTC limit that is rejected if it would match on arrival (guarantees maker status).

**Market making.** A market maker (MM) quotes a bid and an ask and earns the spread, but carries *inventory risk* (its position moves with price) and suffers *adverse selection*: its quotes tend to get filled exactly when the price is about to move against it [11][13].

**Avellaneda–Stoikov (A–S) model [14].** With mid $s$, inventory $q$ (lots, + long / − short), risk aversion $\gamma$, volatility $\sigma$, liquidity parameter $\kappa$, session end $T$ and current time $t$:

$$r = s - q\,\gamma\,\sigma^2\,(T-t)$$

$$\delta^a + \delta^b = \gamma\,\sigma^2\,(T-t) + \frac{2}{\gamma}\ln\!\left(1+\frac{\gamma}{\kappa}\right)$$

The *reservation price* $r$ shifts both quotes against inventory (long → quote lower to sell). The quotes sit symmetrically around $r$: bid $= r - \delta/2$, ask $= r + \delta/2$, where $\delta = \delta^a + \delta^b$ is the total spread.

**Zero-intelligence (ZI) order flow.** Random traders with no strategy. Gode & Sunder showed that random traders bound only by a budget constraint reach close to 100% allocative efficiency in a double auction [15]. Smith, Farmer et al. model order flow as Poisson streams of limit orders, market orders and cancellations [16]. ZI flow is our background market: simple, explainable, and a known baseline.

**Stylized facts [17].** Statistical properties real returns tend to show: heavy tails, near-zero autocorrelation of returns, and volatility clustering (positive autocorrelation of $|r_t|$). We **measure** these; we do not tune the simulator to fake them.

---

## 2. Lessons from existing simulators → design rules

| What others did | Lesson | Rule for `lobsim` |
|---|---|---|
| Production-style engines use int64 ticks/lots and convert decimals only at the boundary [2]; a teaching engine notes that a real one would never use `std::map<double, …>` [3]. | Floating-point prices cause equality and rounding bugs. | **All prices are `int64_t` ticks, all quantities `int64_t` lots.** Decimals only for display. |
| The classic fast LOB design: a sorted structure of price levels, each holding a doubly linked FIFO list of orders, plus a lookup from order id to order [1]. | O(1) cancel needs direct access to the order node. | Price-level map + intrusive doubly linked list + id-indexed lookup (Section 3.5). |
| ABIDES: message-based design modeled on NASDAQ ITCH/OUCH, configurable pairwise latencies, tens of thousands of agents [6]. | Message passing + latency is the right architecture. | Discrete-event simulator; every interaction is a timestamped message with latency. |
| ABIDES's Python reference processes only about 14,000 events/s [7]; reimplementers had to reverse-engineer undocumented ordering of simultaneous events to reproduce it [8]. | Slow cores and undocumented tie-breaking make results hard to reproduce. | C++ core. **Ties broken by an explicit, documented sequence number.** Determinism tested. |
| hftbacktest models feed latency, order latency and **queue position** for fills [9]; bar-based backtests cannot simulate queue position [9]. | Fills depend on where you are in the queue and on how stale your view is. | Fills happen **only** through the matching engine (real queue position). Agents see the market only through delayed messages. |
| Pure historical replay cannot simulate the market's response to the tested agent's orders [10]. | Replay ignores your own impact. | Interactive agent-based simulation. (Replay of our own logs is used only for determinism checks.) |
| A textbook MM setup assumes limit orders fill with probability 1 and produces backtests that look profitable but fail in reality [11]; in one study, LOB models with unrealistic price dynamics overstated mean MM profit by about 50–60% [12]. | Optimistic fill models invent profit. | No assumed fills. Include an **informed trader** to create adverse selection. Report **markouts** (Section 6). |
| Cboe keeps time priority on a modify that only decreases quantity; other changes lose it [4]. | Modify semantics must be explicit. | Rule in Section 3.3. |
| Exchanges offer self-trade prevention (STP) modes such as cancel-oldest (cancel the resting order) and cancel-newest (cancel the incoming order) [5]. | Agents can accidentally cross themselves. | Two STP modes (Section 3.4). |
| Average latency hides the tail; closed-loop benchmarks suffer *coordinated omission* and underreport tail latency [18]. | Report distributions, and say what you measured. | Percentiles + max; README states it's per-message service time (Section 7). |

---

## 3. Part A: the matching engine (`lob` library)

### 3.1 Types
```cpp
using Price = int64_t;      // ticks, valid range [1, cfg.max_price]
using Qty = int64_t;        // lots, > 0
using OrderId = uint64_t;   // assigned by engine, starts at 1, strictly increasing
using OwnerId = uint32_t;
enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market };
enum class TIF : uint8_t { GTC, IOC, FOK };
enum class STP : uint8_t { CancelTaking, CancelResting };   // default CancelTaking
```
Requests: `NewOrder{owner, client_order_id, side, type, tif, price, qty, post_only, stp}`, `Cancel{owner, order_id}`, `Modify{owner, order_id, new_price, new_qty}` (`new_qty` = new **remaining** quantity).

### 3.2 Validation (checked before any state change, in exactly this order)
Reject reasons: `InvalidQty`, `InvalidPrice`, `InvalidFlags`, `UnknownOrder`, `NotOwner`, `NoChange`, `PostOnlyWouldCross`. The **first** failing check wins, so both books report the same reason:
- **New order:** `qty <= 0` → `InvalidQty`; `post_only` with `Market` or with `tif != GTC`, or `Market` with `tif != IOC` (market orders are IOC only) → `InvalidFlags`; limit price outside `[1, max_price]` → `InvalidPrice` (market price is ignored); post-only that would cross → `PostOnlyWouldCross`.
- **Cancel:** id not resting → `UnknownOrder`; owner mismatch → `NotOwner`.
- **Modify:** `UnknownOrder` → `NotOwner` → `new_qty <= 0` → `InvalidQty` → price outside range → `InvalidPrice` → same price and same qty → `NoChange` → post-only would cross → `PostOnlyWouldCross`.

A rejected request changes nothing and assigns no `OrderId`.

### 3.3 Matching rules
1. Accepted new orders get an `Ack` with a fresh `OrderId` **before** any other event for that order.
2. A buy matches asks with price ≤ its limit (market: any price), best price first, FIFO within a level. Sells mirror this. Each fill emits a `Trade` at the **maker's price**.
3. After matching: GTC remainder rests at the tail of its level. IOC/market remainder → `Cancelled(IOCRemainder)` or `Cancelled(NoLiquidity)` if nothing filled at all for a market order.
4. **FOK:** first a read-only dry run walks the book in priority order and sums fillable quantity (STP rules below apply to the dry run). If it is less than `qty`: `Ack` then `Cancelled(FOKUnfillable)`, with **no other side effects**. Otherwise execute normally.
5. **Post-only:** "would cross" means buy price ≥ best ask, or sell price ≤ best bid (see 3.2).
6. **Modify** (only resting orders, so only GTC):
   - same price, smaller qty → reduce in place, **keeps priority**;
   - qty increase or any price change → **loses priority**: removed and re-entered as a new arrival with the same `OrderId` at the new price. It may match on re-entry, as a taker.
   - post-only order whose new price would cross → `Reject(PostOnlyWouldCross)`, original order unchanged.
   - On re-entry the order keeps its `post_only` and `stp` settings.
   - Emits `Modified{order_id, new_price, new_qty, priority_kept}` before any trades it causes.
7. Cancel removes the order → `Cancelled(User)`.

### 3.4 Self-trade prevention
STP applies when the taker and a resting maker have the same `owner`. The **taker's** mode decides:
- `CancelTaking` (default): cancel the taker's remaining qty → `Cancelled(SelfTrade)`. Fills already made stand; the maker is untouched.
- `CancelResting`: cancel the maker → `Cancelled(SelfTrade)` for the maker; matching continues.

FOK dry run: a same-owner maker is skipped under `CancelResting`. Under `CancelTaking` it ends the dry run, and the FOK is fillable only if enough quantity was found before reaching it. A failed FOK cancels nothing else.

### 3.5 Fast `OrderBook` data structures
- Bids: `std::map<Price, Level, std::greater<Price>>`; asks: `std::map<Price, Level>`. `begin()` is the best price. `std::map` nodes have stable addresses, so orders may point to their `Level`.
- `Level { Price price; Qty total_qty; uint32_t count; Order* head; Order* tail; }`. Delete a level when it becomes empty.
- `Order { OrderId id; OwnerId owner; Side side; Price price; Qty remaining; STP stp; bool post_only; Order* prev; Order* next; Level* level; }` (intrusive FIFO list).
- Orders come from a **chunked object pool with a free list**. Never store orders in a `std::vector<Order>` that can reallocate (dangling pointers).
- Lookup: `std::vector<Order*>` indexed by `OrderId` (ids are dense), set to `nullptr` on removal. This avoids per-insert hash-node allocations of `std::unordered_map`.
- Complexity targets: cancel O(1) + level erase; add O(log L) for a new level, O(1) for an existing one (L = number of levels).
- Query API: `best_bid()`, `best_ask()` (as `std::optional<Price>`), `depth(side, n)` → top n `(price, total_qty)`, `order(id)`.

### 3.6 `ReferenceBook`
A second, deliberately naive implementation of the **identical** interface and semantics: one `std::vector` of resting orders; find the best match by linear scan using `(price, priority_seq)`, where `priority_seq` is a counter assigned on arrival and re-assigned when a modify loses priority. Readability over speed. It exists to catch bugs in `OrderBook`.

### 3.7 Events
```
Ack{order_id, owner, client_order_id}
Reject{owner, request_kind, client_order_id /*new orders, else 0*/, order_id /*cancel/modify, else 0*/, reason}
Trade{trade_id, price, qty, maker_order_id, taker_order_id, maker_owner, taker_owner, aggressor_side}
Cancelled{order_id, owner, qty_cancelled, reason}   // User, IOCRemainder, NoLiquidity, FOKUnfillable, SelfTrade
Modified{order_id, owner, new_price, new_qty, priority_kept}
```
`trade_id` starts at 1 and increments. Events are appended to a caller-owned buffer (no allocation per call once the buffer has grown). Within one request, events appear in the order they happen (`Ack`/`Modified` first, then trades and STP cancels in match order, then any final `Cancelled`).

**Event hash:** FNV-1a 64-bit over a fixed little-endian binary encoding of every field of every event, in order. Used by the determinism and replay checks.

### 3.8 Invariants (`validate()`, run after every message in Debug and tests)
- Book never crossed: if both sides are non-empty, best bid < best ask.
- Each level: `total_qty` = sum of its orders' `remaining`, `count` = list length, no empty levels, no order with `remaining <= 0`.
- The id index points exactly at the resting orders.
- Quantity conservation (checked in tests): for every order, filled + cancelled + resting = accepted qty, adjusted by the change in remaining qty at each modify.

---

## 4. Part B: tests (GoogleTest)

**Unit tests**, each a small hand-checkable scenario: the worked example in Section 1; trade at maker price; multi-level sweep; FIFO at one level; IOC remainder; market on empty side (`NoLiquidity`); FOK exact fill; FOK one lot short (no trades, book unchanged); post-only reject; modify reduce keeps priority; modify increase loses priority; modify to a crossing price trades; cancel unknown / wrong owner; both STP modes including the FOK dry-run cases; empty level removed; best price updates after a level empties.

**Differential fuzz test:** a seeded generator produces random valid **and invalid** requests (unknown ids, zero qty, crossing post-only, all order types, few owners so STP triggers). Feed the same stream to `OrderBook` and `ReferenceBook`; the event streams must be identical. At least 200 seeds × 10,000 messages in the normal suite; a `--long` mode for 10⁶+.

**Determinism test:** run the simulator twice with the same seed and assert identical output-file hashes; different seeds → different hashes.

**Replay test** (Section 5.7): live-run engine event hash == replayed hash.

**Sanitizers:** an `LOBSIM_SANITIZE=ON` build with ASan + UBSan runs the whole suite cleanly.

---

## 5. Part C: the simulator (`sim` library)

### 5.1 Time and events
- Simulation time is `int64_t` nanoseconds. No wall-clock time anywhere in the simulation.
- Event queue: min-heap on `(time_ns, seq)`, where `seq` is a global counter incremented on every schedule. **Equal times are processed in scheduling order.** Document this in the README.
- Single-threaded. Never let iteration order of an unordered container affect results.

### 5.2 Randomness
- Implement your own `SplitMix64` (seeding) + `xoshiro256**` generator, and your own distributions: uniform int (rejection sampling, no modulo bias), uniform double in [0,1), exponential via $-\ln(1-u)/\lambda$, normal via Box–Muller. (`std::` distributions are implementation-defined and differ across standard libraries.)
- One master seed; each agent gets its own stream derived from it, so adding an agent never changes the others' draws.
- Results are bit-identical for the same build and platform; across platforms, `libm` (`log`) may differ in the last bit. Document this.

### 5.3 Latency (per agent, from config)
- `entry_latency`: agent → exchange. `response_latency`: exchange → owner (acks, fills, cancels). `feed_latency`: exchange → agent (public market data).
- Each = constant + optional uniform jitter `[0, jitter]`.
- **Per-agent FIFO:** a message never arrives before an earlier message on the same channel: `arrival = max(sampled_arrival, last_arrival_on_channel)`.

### 5.4 Exchange
Wraps one `OrderBook`. When a request arrives it is processed instantly; then:
- private events go to their owners after `response_latency` (a `Trade` goes to **both** maker and taker owners);
- public market data goes to every subscriber after its `feed_latency`: a `BBO{bid, bid_qty, ask, ask_qty}` message whenever top-of-book changed, and a `TradePrint{price, qty, aggressor_side}` per trade.
- Every request arriving at the exchange is appended to `orders.log` (time + request) for replay.

### 5.5 Fundamental value
A latent value $V$ (in ticks, `double`), starting at `initial_price`. Every $\Delta t$ = `fundamental.dt_s` seconds: $V \leftarrow V + \sigma_f \sqrt{\Delta t}\, Z$, with $\sigma_f$ = `fundamental.sigma_ticks_per_sqrt_s` and $Z \sim N(0,1)$. Only the informed trader sees it.

### 5.6 Agents
Interface: `on_start(ctx)`, `on_wakeup(ctx)`, `on_market_data(ctx, msg)`, `on_private(ctx, event)`. `ctx` gives `now()`, `send(request)`, `schedule_wakeup(t)`, and an RNG. Agents keep their **own** view of the book (last received BBO) and their own orders (from acks/fills/cancels). Agents never read the exchange's book directly.

1. **NoiseTrader** (ZI, after [16]). Wakes as a Poisson process with rate `lambda`. Picks limit with probability `p_limit`, otherwise market; side 50/50.
   - Reference price `ref` = integer mid of its local BBO (floor), else last trade, else `initial_price`.
   - Limit buy at `ref − d`, sell at `ref + d`, with `d` uniform in `[1, D]`, clamped to `[1, max_price]`, GTC. Size uniform in `[1, max_size]`. Its view is delayed, so a limit order can occasionally cross the real book and trade. That is expected.
   - Market: size uniform in `[1, max_size]`.
   - **Cancellation:** when an order is acked and still resting, draw a lifetime $\sim \text{Exp}(1/\text{order\_lifetime\_mean\_s})$ and schedule a cancel then (if it's filled first, the `UnknownOrder` reject is ignored). This gives each resting order a constant cancel rate, so the book reaches a steady size instead of growing forever.
2. **InformedTrader.** Wakes at Poisson rate `lambda`. If $V - \text{ask} \ge \theta$: market buy `size`; if $\text{bid} - V \ge \theta$: market sell. Uses its delayed local BBO. This is the source of adverse selection.
3. **AvellanedaStoikovMM.** One bid and one ask, each `quote_size`, post-only GTC.
   - Recompute on every BBO update and every `requote_interval`, using its local mid $s$, inventory $q$ (from its own fill messages), the formulas in Section 1, $\sigma$ from config (units: ticks per $\sqrt{\text{s}}$), and $T = $ `duration_s`, with $T-t$ in seconds.
   - bid = `floor(r − δ/2)`, ask = `ceil(r + δ/2)`. Clamp so bid ≤ local best ask − 1 and ask ≥ local best bid + 1. Ensure bid < ask.
   - Don't quote the bid if $q \ge Q_{\max}$; don't quote the ask if $q \le -Q_{\max}$. Don't quote if its local BBO lacks either side.
   - Per side: no working order → `NewOrder`; working order at a different price → `Modify`; side should not be quoted → `Cancel`.
   - **Never send a new request for a side while a previous one on that side is still unanswered** (no `Ack`/`Modified`/`Cancelled`/`Reject` yet). This prevents the classic stale-state bug. Handle `PostOnlyWouldCross` and `UnknownOrder` rejects (e.g. a modify racing a fill) without crashing or desyncing.
   - The simulation ends at $T$. Final PnL marks the remaining inventory to the exchange's last mid.

### 5.7 Replay
`lobsim_replay --log out/<run>/orders.log` feeds the logged requests straight into a fresh `OrderBook` and prints the hash of the engine event stream. It must equal the hash recorded by the live run.

### 5.8 Config (`configs/default.json`, nlohmann/json)
Starting values, not tuned results. Keep them, then tune and record any change in `DECISIONS.md`:
```json
{
  "seed": 42, "duration_s": 3600, "warmup_s": 60, "max_price": 1000000, "initial_price": 10000,
  "tick_display": 0.01, "fundamental": {"sigma_ticks_per_sqrt_s": 1.0, "dt_s": 0.1},
  "noise_traders": {"count": 10, "lambda": 5.0, "p_limit": 0.85, "order_lifetime_mean_s": 30.0,
                    "D": 10, "max_size": 10,
                    "latency_us": {"entry": 200, "response": 200, "feed": 200, "jitter": 50}},
  "informed": {"lambda": 2.0, "theta_ticks": 2, "size": 5,
               "latency_us": {"entry": 100, "response": 100, "feed": 100, "jitter": 20}},
  "mm": {"gamma": 0.0001, "kappa": 1.5, "sigma_ticks_per_sqrt_s": 1.0, "quote_size": 5, "q_max": 50,
         "requote_interval_ms": 100, "maker_fee_ticks": -0.002, "taker_fee_ticks": 0.003,
         "latency_us": {"entry": 50, "response": 50, "feed": 50, "jitter": 10}},
  "markout_horizons_s": [1, 5, 30], "sample_interval_s": 1.0
}
```
Sanity check of these numbers:
- At $t=0$: $\gamma\sigma^2 T = 0.0001 \cdot 1 \cdot 3600 = 0.36$ ticks, and $\frac{2}{\gamma}\ln(1+\gamma/\kappa) = 20000 \cdot \ln(1.0000667) \approx 1.33$ ticks. So the total spread starts near 1.7 ticks and narrows to about 1.33 ticks by the end.
- Each lot of inventory shifts $r$ by at most 0.36 ticks, so one 5-lot fill shifts it about 1.8 ticks.
- Noise flow is $10 \times 5 = 50$ wakeups/s, so about 42 limit orders/s. With a 30 s mean lifetime, the steady book is on the order of 1,000 orders, minus fills.

CLI: `lobsim_run --config <file> [--seed N] --out <dir>`. CLI flags override the config.

---

## 6. Part D: metrics and outputs

Measure all statistics after `warmup_s`, using the exchange's true book (ground truth).

- **MM accounting** (cash in ticks × lots as `int64`, fees as `double`): fills, volume, final inventory, max |inventory|, mark-to-market PnL = cash + q × mid − fees, fraction of time quoting both sides, average quoted spread.
- **Markouts:** for each MM fill at time $t$ and horizon $h$, buy: $m_{t+h} - p$; sell: $p - m_{t+h}$ (per lot, ticks; $m$ = exchange mid). Report the mean and a 95% confidence interval per horizon. (The interval assumes independent fills, which overlapping horizons violate; say so in the README.) Skip and count samples where the mid is undefined. Negative markouts = adverse selection.
- **Market stats:** spread distribution; from mids sampled every `sample_interval_s`, log returns $r_t = \ln(m_t / m_{t-1})$: excess kurtosis, ACF of $r_t$ at lags 1–5, ACF of $|r_t|$ at lags 1–10.
- **Files** in `--out`: `trades.csv`, `bbo.csv` (sampled), `mm_fills.csv` (with markouts), `summary.json` (all metrics + config + seed + engine event hash), `orders.log`.
- `tools/plot.py` (pandas + matplotlib, optional to run): mid vs fundamental, MM inventory, MM PnL, spread histogram, return ACF plots.

---

## 7. Part E: benchmarks

- **Microbenchmarks** (Google Benchmark), with the book pre-filled to 1k and 100k orders: add non-crossing, cancel, aggressive order matching 1 level, modify-reduce, best-price query.
- **Throughput harness** `lobsim_bench`: 10M seeded messages from a realistic mix (similar to noise-trader flow) through `OrderBook`. Report msgs/s and a per-message service-time histogram (p50, p90, p99, p99.9, max; never only the mean). Run `ReferenceBook` on 100k messages for comparison.
- Measure and report the timer's own overhead; time batches if it's significant.
- Count heap allocations per message by overriding global `operator new` in the bench binary only. Expected: allocations appear only when a new price level is created (a `std::map` node) or when a pool/vector grows. Report the real number.
- Print CPU model, compiler and flags. Release build (`-O3`, no sanitizers).
- The README must say these numbers are **in-process service time with a closed loop, not end-to-end latency under load** (see coordinated omission [18]).

---

## 8. Project layout and build

```
lobsim/
  CMakeLists.txt  README.md  DECISIONS.md
  configs/default.json
  include/lob/   types.hpp events.hpp pool.hpp order_book.hpp reference_book.hpp
  src/lob/       order_book.cpp reference_book.cpp
  include/sim/   rng.hpp event_queue.hpp latency.hpp exchange.hpp agent.hpp metrics.hpp config.hpp
                 agents/noise_trader.hpp agents/informed_trader.hpp agents/as_market_maker.hpp
  src/sim/       *.cpp
  apps/          run_main.cpp replay_main.cpp bench_main.cpp
  tests/         unit/*.cpp fuzz_diff.cpp determinism.cpp replay.cpp
  bench/         micro_bench.cpp
  tools/plot.py
```
- C++20, CMake ≥ 3.20, GCC ≥ 11 or Clang ≥ 14. `-Wall -Wextra -Wpedantic`, zero warnings.
- Dependencies via `FetchContent` at pinned tags: googletest `v1.14.0`, google benchmark `v1.8.3` (set `BENCHMARK_ENABLE_TESTING OFF`), nlohmann/json `v3.11.3`. No other dependencies in C++.
- Options: `LOBSIM_SANITIZE` (ASan+UBSan). `ctest` runs all tests.
- The `lob` library must not depend on `sim`.

---

## 9. Phases and acceptance checks

1. **Skeleton:** CMake, dependencies, types, an empty test that passes. ✔ builds in Debug, Release and Sanitize.
2. **ReferenceBook** + all unit tests from Section 4 passing against it.
3. **OrderBook** (pool, maps, intrusive lists, index) + `validate()`. ✔ the same unit tests pass; the differential fuzz test passes; sanitizer build clean.
4. **Benchmarks** (Section 7). ✔ results table pasted into the README.
5. **Simulator core:** RNG, event queue, latency, exchange, fundamental, NoiseTrader. ✔ a 1-hour run completes, the book is never crossed, determinism test passes.
6. **InformedTrader + A–S MM.** ✔ the MM fills on both sides; $|q| \le Q_{\max} + \text{quote\_size}$ at all times (one resting order per side means at most one quote's overshoot); the pending-request rule holds (add a test).
7. **Metrics, outputs, replay.** ✔ replay hash matches; `summary.json` complete; plots render.
8. **README:** what it is; architecture diagram (ASCII is fine); the matching rules; design decisions and trade-offs; benchmark table; a sample run's results (MM PnL, markouts, stylized facts) with an **honest** reading. State which stylized facts appear and which don't, and that ZI models are known simplifications. List what's not modeled (Section 10). Include how to build, test and run.

**Definition of done:** all phases ✔; `ctest` green in Debug and Sanitize; zero warnings; README complete; every behaviour in Section 3 covered by at least one test.

---

## 10. Out of scope (mention in the README; do not build)
Multiple instruments, networking/FIX, multithreading, opening/closing auctions, stop/iceberg/hidden orders, decrement-and-cancel STP, real market data ingestion. Good stretch goals afterwards: an array-indexed price ladder compared against `std::map`, LOBSTER data replay, a reinforcement-learning environment wrapper.

---

## References
1. W. K. Selph, "How to Build a Fast Limit Order Book" (mirror): https://gist.github.com/halfelf/db1ae032dc34278968f8bf31ee999a25
2. intrepidkarthi/orderbook (Go CLOB, integer ticks/lots): https://pkg.go.dev/github.com/intrepidkarthi/orderbook@v0.3.0
3. "Building a price-time-priority order book in C++": https://tuna.is-a.dev/blog/order-book-matching-engine
4. Cboe Futures Exchange BOEv3, Modify Order: https://www.cboe.com/document/tech-spec/content/technical-specifications/cboe-titanium-cboe-futures-exchange-boev3-specification/application-messages/tph-to-cfe/modify-order
5. Coinbase Exchange matching engine, self-trade prevention: https://docs.cdp.coinbase.com/exchange/docs/matching-engine
6. Byrd, Hybinette, Balch, "ABIDES: Towards High-Fidelity Market Simulation for AI Research": https://arxiv.org/abs/1904.12066
7. "Exact First, Then Fast: An Event-for-Event C++ Rebuild of the Agenthon ABIDES Market" (NeurIPS 2026): https://neurips.cc/virtual/2026/160270
8. "Bit-Exact on Every Public Scenario and About 90× Faster" (NeurIPS 2026): https://neurips.cc/virtual/2026/160267
9. hftbacktest documentation: https://www.mintlify.com/nkaz001/hftbacktest/concepts/backtesting and https://hftbacktest.readthedocs.io/en/py-v2.0.0/
10. Coletta et al., "Towards Realistic Market Simulations: a Generative Adversarial Networks Approach": https://arxiv.org/pdf/2110.13287.pdf
11. "The Negative Drift of a Limit Order Fill": https://arxiv.org/pdf/2407.16527
12. "Market Making under a Weakly Consistent Limit Order Book Model": https://arxiv.org/pdf/1903.07222
13. Lalor & Swishchuk, "Market Simulation under Adverse Selection": https://arxiv.org/abs/2409.12721
14. Avellaneda & Stoikov (2008), formulas as given in Hummingbot's technical deep dive: https://hummingbot.org/blog/technical-deep-dive-into-the-avellaneda--stoikov-strategy/ and https://hummingbot.org/academy-content/technical-deep-dive-into-the-avellaneda--stoikov-strategy/
15. Gode & Sunder (1993), "Allocative Efficiency of Markets with Zero-Intelligence Traders": https://econpapers.repec.org/RePEc:ucp:jpolec:v:101:y:1993:i:1:p:119-37
16. Smith, Farmer, Gillemot, Krishnamurthy, "Statistical theory of the continuous double auction": https://arxiv.org/abs/cond-mat/0210475 ; Daniels et al., "A quantitative model of trading and price formation": https://arxiv.org/abs/cond-mat/0112422
17. Cont (2001) stylized facts, as summarized in Ratliff-Crain et al., "Revisiting Cont's Stylized Facts for Modern Stock Markets": https://arxiv.org/abs/2311.07738
18. Coordinated omission: https://bravenewgeek.com/benchmarking-message-queue-latency/ ; G. Tene, mechanical-sympathy thread: https://groups.google.com/g/mechanical-sympathy/c/icNZJejUHfE/m/BfDekfBEs_sJ
