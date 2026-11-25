# Module 13: Real market data

> **In one sentence:** lobsim can read a day of real Nasdaq order-book data in the LOBSTER format, rebuild the book message by message, check it against the published book after every message, and test whether lobsim's matching rule predicts the real executions.
>
> **You will be able to:** read LOBSTER message and orderbook files (including the Direction trap), explain seeding and seed attribution, the three replay modes, the matching-agreement check, and why price-window filtering creates stale orders; run the replayer on a fixture and on a simulated day; and state honestly what has and has not been tested.
>
> **Time:** about 4 hours. This module is advanced; it is fine to do it after the others.

> **Data status.** No real LOBSTER or Nasdaq data has been replayed with lobsim yet: the free sample files could no longer be downloaded when the project was written (REAL_DATA.md). Every number in this module is **synthetic**: it comes from lobsim's own simulator or from small hand-written files. The tool is written for real files; real results are pending.

## 13.1 Why bother with real data?

A simulator is built on rules. lobsim's matching rule (price-time priority, trade at the maker's price) is what textbooks and exchange documents describe, but is it what a real exchange's data shows, message after message? And the simulated traders are invented; real traders could be measured instead (Module 14). Both questions need real order-book data.

**LOBSTER** (lobsterdata.com) publishes order-book data reconstructed from Nasdaq's own message feed: for one stock and one day, every event, and the state of the book after every event.

## 13.2 The two files

**The message file**: one line per event, six columns (`include/replay/lobster.hpp`):

| Column | Meaning |
|---|---|
| Time | seconds after midnight, up to 9 decimals (34200.000000001 is 9:30:00 and one nanosecond) |
| Type | 1 new limit order, 2 partial cancel, 3 full delete, 4 execution of a visible order, 5 execution of a hidden order, 6 cross (auction) trade, 7 trading halt |
| Order ID | the exchange's reference number for the order |
| Size | shares |
| Price | dollars × 10,000 (91.14 dollars is 911400), always a whole number |
| Direction | −1 sell, +1 buy |

**The trap: for an execution (type 4), Direction is the side of the resting order that was hit, not the side of the aggressor.** A type-4 message with Direction +1 means a resting **buy** order was executed, so the aggressor was a **seller**. Getting this backwards is a classic bug, and one of lobsim's replay canaries injects exactly that bug (13.7).

**The orderbook file**: one line per message, 4 × L columns, where L is the number of levels: for level 1 the ask price, ask size, bid price and bid size, then level 2, and so on. **Line k of the orderbook file is the book right after line k of the message file.** A level with size 0 is empty, whatever its price column holds (files typically write placeholder prices like 9999999999 there).

File names carry the facts: `INTC_2012-06-21_34200000_57600000_message_10.csv` is ticker INTC, 21 June 2012, from 34,200,000 ms (9:30) to 57,600,000 ms (16:00), 10 levels.

**Nothing is read as a decimal fraction.** The time 34200.123456789 is parsed as text into the integer 34,200,123,456,789 ns, and prices stay integers in LOBSTER units (D-158 to D-160). A line that does not parse is counted and skipped, never guessed (`parse_rejected`). These parsers are fuzzed too (`fuzz_lobster_parse`, Module 6).

## 13.3 Rebuilding the book: apply, never re-match

The messages are the *result* of the exchange's matching. So the replayer does not match anything: it **places** new orders and **removes** quantity as the messages say. For that, both books gained "book-builder" operations (D-151 to D-155), with no events and no matching:

| Operation | Used for |
|---|---|
| `insert_passive(side, price, qty)` | type 1: place an order; refused with `WouldCross` if it would cross |
| `reduce(id, qty)` | types 2 and 4: take quantity away (all of it removes the order) |
| `remove(id)` | type 3: delete the order |
| `preview_match(taker_side, limit, qty)` | the matching check (13.5): which resting orders would an aggressive order hit? Read-only |
| `level_orders(side, price)` | the queue at one price, in priority order |

They are fuzzed against the reference book like everything else (`fuzz_diff_builder`).

`src/replay/replayer.cpp`, `Replayer::apply`:

```cpp
  switch (m.type) {
    case 1: {
      if (by_external_.count(m.order_id) != 0) {
        ++c.dup_id;
        return;
      }
      const lob::Result<lob::OrderId> r = book_.insert_passive(side_of(m.direction), m.price, m.size);
      if (r.ok()) {
        by_external_.emplace(m.order_id, r.value);
        external_of_.emplace(r.value, m.order_id);
        ++c.submitted;
      } else if (r.status == lob::Status::WouldCross) {
        ++c.submit_would_cross;
      } else {
        ++c.submit_bad_price;
      }
      return;
    }
    case 2:
    case 3:
    case 4: {
      const auto it = by_external_.find(m.order_id);
      if (it != by_external_.end()) apply_known(m, it->second);
      else attribute(m);
      return;
    }
    case 5: ++c.hidden_exec; return;
    case 6: ++c.cross_trade; return;
    case 7: ++c.halt; return;
    ...
```

- lobsim's engine gives the order its own dense id (Module 5: ids must be 1, 2, 3, ...). Two hash maps translate between the exchange's id and the engine's id.
- `apply_known` checks before changing anything: if the message's Direction or price disagrees with the order the id names, the message is skipped and counted (`direction_mismatch`, `price_mismatch`).
- Hidden executions, cross trades and halts do not change the visible book: counted, not applied.
- **Every message lands in exactly one counter.** Nothing is dropped silently; the fixture tests check that the counters add up to the number of messages.

### Seeding: the book is not empty at 9:30

At 9:30 the book already holds orders placed before the open, and the message file does not say who they are. The replayer reads **orderbook line 1** and places one **synthetic** order per published level (`seed()`). Message 1 is counted as `consumed_by_seed`, since its effect is already in line 1.

Later, a cancel or execution may name an order id the replayer has never seen: probably one of those pre-open orders. `attribute()` then takes the quantity from a synthetic order at exactly that price and side, if one can absorb it (`seed_attributed`); otherwise the message is `unknown_id`. The price of this trick: synthetic orders have no identity and no real queue position.

## 13.4 Checking the rebuilt book

After every message, the replayer compares its book with the published row (`compare`): how many levels, from the top, match exactly in price and size on both sides. The headline numbers are the **share of rows exact to depth 1, 2, ..., L, before any correction**. Three modes decide what happens next:

| Mode | What it does |
|---|---|
| `strict` | stops at the first row that is not exact to `--compare-depth` (exit code 2), and prints both books and the last messages |
| `report` | never changes the book; only counts |
| `resync` | counts, then corrects the book to the published row inside its window, and counts every correction by level |

The resync window (D-172) is every price at least as good as the published L-th level (the whole side if fewer than L levels are shown). Outside it, the data says nothing, so nothing is touched. Corrections are reported as a histogram by level: **a correction at level 1 is a warning sign** (the touch is always inside the window, so the book should never be wrong there), while corrections at the deepest levels are expected (13.6).

## 13.5 Does lobsim's matching rule agree with the data?

A **burst** is a run of consecutive type-4 messages with the same time and the same Direction: one aggressive order hitting several resting orders. **Before** applying a burst, the replayer asks `preview_match` which orders an aggressive order of that total size would hit under lobsim's rule, and compares the prediction with the burst fill by fill (`src/replay/replayer_match.cpp`). Each burst gets one class:

| Class | Meaning |
|---|---|
| `exact` | same orders, prices and sizes |
| `exact_via_synthetic` | the same if synthetic orders are compared by price and size only |
| `queue_order` | first difference: same price, a different order |
| `price_skip` | first difference: a different price |
| `size_split` | same order and price, different size |
| `length` | every compared fill equal, a different number of fills |
| `excluded_unknown`, `excluded_untrusted` | not scored: an unknown id, or the book before the burst was not exact to the **trust depth** |

Agreement = (exact + via synthetic) / scored, always printed as numerator and denominator. A burst is only scored if the row before it was exact to the trust depth (default 3) before any correction: comparing predictions made on a wrong book would test the replay, not the rule.

## 13.6 The filtering problem: orders the data forgets

LOBSTER files with L levels only contain messages for events **inside the top-L price window** at that moment. If an order sits deep in the book and is deleted while it is outside the window, **no message** is published. The replayer still holds it. When the price later comes back to that level, the stale order is still there, and the book is wrong at the touch.

REAL_DATA.md shows this with a synthetic experiment you can reproduce (Lab 13.3): exporting the same simulated 10 minutes with a filter at L = 10 gives no level-1 corrections; at L = 5, 1,297 of them. Not an engine bug: a limit of filtered data, which real LOBSTER files share.

For the "dress rehearsal", the project built two synthetic days with LOBSTER's quirks (a full book at the start, hidden executions, a halt, the price-window filter) and wrote an independent audit tool (`tools/lobster_audit.py`) that re-replays the files and traces **every** discrepancy to a cause. On the filtered days, every level-1 correction, every unknown id and every disagreeing burst was traced to a delete that the filter dropped, and putting only the dropped deletes back made the days clean. REAL_DATA.md documents an alternative trimming rule that would have hidden most of the problem, measured it, and kept the spec's rule anyway, because it departs from the spec and still does not reach zero (D-212, D-213).

## 13.7 Can the replay fail? Its canaries

Three deliberate replayer bugs, behind `LOBSIM_REPLAY_BUG`, in a special build that CI runs:

| Canary | The bug |
|---|---|
| `DIRECTION_IS_AGGRESSOR` | an execution's Direction is read as the aggressor's side |
| `APPLY_HIDDEN` | type 5 hidden executions are applied like visible ones |
| `NO_SEED` | the opening book is not seeded |

CI passes only if each is caught (strict mode exits with code 2) on the hand-written fixtures. `APPLY_HIDDEN` is only caught by the fixture `hidden_exec`, which has a hidden execution at a price where seeded liquidity sits: data that never touches the visible book cannot reveal the bug. Choosing test data that can actually show a bug is half of testing.

## 13.8 Making LOBSTER files from the simulator

`lobsim_export_lobster` (`src/sim/lobster_export.cpp`) replays an `orders.log` and writes the two LOBSTER files: the **closed loop**. Replaying those files must reproduce lobsim's own book exactly, which tests the replayer's bookkeeping against a book it should match perfectly (not lobsim against a market). Options: `--filter-window` drops messages outside the top-L window, as LOBSTER does; `--lobster-like` adds a full book at the start, hidden executions and a halt; `--price-scale` converts ticks into LOBSTER price units.

**Skipped on purpose:** the report writers (`replayer_json.cpp`, `replayer_text.cpp`), the line reader with a bounded look-ahead buffer (`Replayer::Source`, which reads ahead to collect a burst and refuses to buffer more than 100,000 lines), `resync`'s removal and trimming details (`remove_level`, `trim_level`), and the dress-rehearsal scripts (`scripts/make_fake_day.sh`, `scripts/run_real_data.sh`, `tools/lobster_audit.py`).

## Lab 13.1: the closed loop

```sh
build/release/lobsim_run --config configs/default.json --seed 7 --duration-s 60 --out out/run7
build/release/lobsim_export_lobster --run out/run7 --levels 10 --out out/lobster7
head -5 out/lobster7/SIM_2012-06-21_34200000_57600000_message_10.csv
build/release/lobsim_lobster \
  --messages out/lobster7/SIM_2012-06-21_34200000_57600000_message_10.csv \
  --orderbook out/lobster7/SIM_2012-06-21_34200000_57600000_orderbook_10.csv \
  --mode strict --compare-depth 10
```

The first message lines, as written when this course was made:

```
34200.001984950,1,1,9,9995,1
34200.114413686,1,3,8,9997,1
34200.173274785,4,3,8,9997,1
34200.208692134,1,5,2,10003,-1
34200.208797576,1,6,5,9998,1
```

and the replay:

```
lines replayed       4621 (rows compared 4621, not exact at depth L 0)
exact rows before any correction (exact / rows):
  depth 1: 4621/4621 (100.00%)            depth 2: 4621/4621 (100.00%)
  ...
  3  370/370 (100.00%)    370   0       0     0 (0/0)              0    0      0             0  <- headline
```

1. Decode message 3 completely. Who was the aggressor, a buyer or a seller?
2. Why is 100% agreement expected here, and why would it be a surprise on real data?
3. Find the `README_SYNTHETIC.txt` the exporter wrote next to the files. Why does it exist?

<details><summary>Answers</summary>

1. At 9:30:00.173274785, a type-4 execution of order 3: 8 shares at 9997 (this export uses ticks as prices). Direction +1 means the **resting** order was a buy, so the aggressor was a **seller** who sold 8 into it.
2. The data was produced by lobsim's own engine, so lobsim's rule is guaranteed to predict it. A real exchange has hidden orders, special order types and other details lobsim does not model.
3. To label the files synthetic, so no tool or person can mistake them for market data (the calibration estimator reads it, Module 14).

</details>

## Lab 13.2: trace a hand-written fixture

`tests/fixtures/lobster/basic_types/` is a 16-message day made up by hand to contain every message type. Print it and replay it:

```sh
cat tests/fixtures/lobster/basic_types/TEST_2012-06-21_34200000_57600000_message_3.csv
head -3 tests/fixtures/lobster/basic_types/TEST_2012-06-21_34200000_57600000_orderbook_3.csv
build/release/lobsim_lobster \
  --messages tests/fixtures/lobster/basic_types/TEST_2012-06-21_34200000_57600000_message_3.csv \
  --orderbook tests/fixtures/lobster/basic_types/TEST_2012-06-21_34200000_57600000_orderbook_3.csv \
  --mode resync
```

The replay reports (as printed):

```
applied              submitted 6, cancelled 1, deleted 1, executed 1
seed                 consumed_by_seed 1, seed orders 3 (rejected 0), seed_attributed 3, unknown_id 0
skipped              ... hidden_exec 1, cross_trade 1, halt 1
...
  3  2/3 (66.66%)         0     2       1     0 (0/0)              0    0      0             0  <- headline
```

Work through the 16 messages with a pencil, using orderbook line 1 for the opening book (`1000100,100,1000000,200,9999999999,0,999900,50,...`: ask 1000100 × 100, bid 1000000 × 200, and a second bid level 999900 × 50). For each message, say which counter it lands in. Then explain the matching line: 3 bursts, 2 agree "via synthetic", 1 is `queue_order`.

<details><summary>Answer</summary>

Seeding places three synthetic orders from line 1 (ask 1000100 × 100, bid 1000000 × 200, bid 999900 × 50) and message 1 (order 11, the bid at 999900) is `consumed_by_seed`. Then: message 2 (order 21 sells 30 at 1000200) `submitted`; 3 (order 22 buys 40 at 1000000) `submitted`; 4 (type 2, order 22 cancels 10) `cancelled`; 5 (type 4, unknown order 777, 60 at 1000100, Direction −1 = a resting sell) `seed_attributed` to the synthetic ask; 6 (type 5) `hidden_exec`; 7 (type 3, order 21) `deleted`; 8 (type 3, order 11, never seen because the seed consumed it) `seed_attributed` to the synthetic bid at 999900; 9 (type 6) `cross_trade`; 10 (type 7) `halt`; 11 (type 4, order 22, 30 at 1000000) `executed`; 12 (type 4, unknown 778, 200 at 1000000) `seed_attributed`; 13 to 16 `submitted`. Totals: 1 + 6 + 1 + 1 + 1 + 3 + 3 = 16.

Bursts are messages 5, 11 and 12 (11 and 12 have different times, so they are separate). Message 5: the prediction hits the synthetic ask at 1000100, and the data's order is unknown but attributable: `exact_via_synthetic`. Message 11: at the 1000000 bid level, the synthetic seed order (200) was placed first, so lobsim predicts it fills first, but the data executed order 22: `queue_order`. In real data this pattern would mean the true queue order differed from lobsim's guess, and here it shows exactly what seeding loses: synthetic orders have no real queue position. Message 12: the synthetic order fills, `exact_via_synthetic`.

</details>

Compare your understanding with the fixture's `expected_summary.json`, which `replay_tests` checks exactly.

## Lab 13.3: watch the filter create stale orders

```sh
build/release/lobsim_run --config configs/default.json --seed 7 --duration-s 600 --out out/run7_600
for L in 10 5; do
  build/release/lobsim_export_lobster --run out/run7_600 --levels $L --filter-window --out out/lob600_L$L
  build/release/lobsim_lobster \
    --messages out/lob600_L$L/SIM_2012-06-21_34200000_57600000_message_$L.csv \
    --orderbook out/lob600_L$L/SIM_2012-06-21_34200000_57600000_orderbook_$L.csv \
    --mode resync | grep -E "depth 1:|corrected by level|unknown_id"
done
```

When this course was written (matching REAL_DATA.md's table):

```
  depth 1: 56203/56203 (100.00%)          depth 2: 56203/56203 (100.00%)
seed                 consumed_by_seed 1, seed orders 1 (rejected 0), seed_attributed 1, unknown_id 591
  corrected by level [0, 0, 17, 56, 74, 102, 158, 179, 246, 462]
  depth 1: 31576/32878 (96.03%)           depth 2: 31155/32878 (94.75%)
seed                 consumed_by_seed 1, seed orders 1 (rejected 0), seed_attributed 756, unknown_id 2817
  corrected by level [1297, 421, 508, 569, 722]
```

1. At L = 10, where do the corrections sit, and why there?
2. At L = 5, why do level-1 corrections appear, although the engine and replayer are unchanged?
3. A colleague runs real data at L = 10 and sees 500 level-1 corrections. Is the engine broken?

<details><summary>Answers</summary>

1. At the deepest levels (462 at level 10): orders enter and leave the edge of the window without messages, which is the expected effect of filtering.
2. With a narrow window, many orders are deleted while outside it (24,404 messages were filtered out). Their stale copies stay in the replayed book, and when the price returns they are at the touch.
3. Not necessarily. Over a whole day the same effect can reach level 1 even at L = 10 (the hour-long synthetic days showed thousands). The right next step is the audit approach: trace each correction to a cause before concluding anything.

</details>

## Common questions

**Why seed from line 1 instead of starting empty?** The real book at 9:30 already holds many orders. Starting empty would make every early row wrong; the `NO_SEED` canary is exactly that mistake.

**Why not just fix the book whenever it differs (always resync) and report 100%?** Because then the numbers would describe the corrections, not the engine. lobsim reports exactness *before* any correction, counts every correction, and scores the matching check only on rows that were exact before correction.

**What is the "trust depth"?** How many levels must have been exact on the row before a burst for that burst to be scored. A burst that spans 2 prices needs at least 3 trusted levels, so trust depth 1 can never score anything (it is printed so you can see the rule work).

**Will real data agree 100%?** It is not expected to. Real exchanges have hidden and reserve orders, auctions, special order types, and LOBSTER's own reconstruction. The point is to measure the disagreements and classify them, not to hide them.

**Where are the real results?** Pending: REAL_DATA.md has a marked block that `scripts/run_real_data.sh` fills in once LOBSTER files are put in `data/lobster/` (never committed: market data cannot be redistributed).

## Quiz

1. A type-4 message has Direction −1. Was the aggressor a buyer or a seller?
2. Why does the replayer use `insert_passive`, `reduce` and `remove` instead of `submit`?
3. What is seed attribution, and what does it lose?
4. What do the three modes do differently?
5. Why is a level-1 correction more alarming than a level-10 one?
6. Why does the `APPLY_HIDDEN` canary need special test data?

<details><summary>Answers</summary>

1. A buyer: Direction −1 means the resting order was a sell, so the aggressive order bought.
2. The messages are already the result of matching. Re-matching would trade orders against each other a second time; the replay must only place and remove what the messages say.
3. Taking a cancel or execution for an unknown id from a synthetic seed order at that price and side. It loses the order's identity and its true place in the queue.
4. Strict stops at the first row not exact to the compare depth; report only counts; resync counts and then corrects the book to the published row inside its window.
5. The touch is always inside the published window, so the replay should never be wrong there; deep levels are expected to drift under filtering.
6. A hidden execution changes the visible book only if it lands where visible liquidity sits. Data whose hidden executions never touch the visible book cannot reveal the bug.

</details>

## What's next

Module 14 uses the replayer for something new: measuring how real traders place, size and cancel their orders, building a simulated trader that copies those measurements, and testing it on data it has never seen.
