# Replaying LOBSTER data

> **Status: data pending.** No LOBSTER or Nasdaq data has been replayed with lobsim yet. LOBSTER's free
> 21 June 2012 sample files could not be downloaded when this was written (19 November 2026): they now
> need an account, a university subscription, an approved trial, or proof of owning the book
> *Trades, Quotes and Prices*. **Every number in this file is synthetic**: it comes from lobsim's own
> simulator (the "closed loop") or from small hand-written fixtures, and is labelled so where it
> appears. Nothing here says how the replay behaves on a real ticker. The tool is written for real
> files and has never been run on any; the section [Real-data results](#real-data-results-data-not-provided-yet) is empty until
> someone runs them.

## Contents

- [What the replay checks, and why](#what-the-replay-checks-and-why)
- [The LOBSTER files as the code reads them](#the-lobster-files-as-the-code-reads-them)
- [Getting data, and running the replay on it](#getting-data-and-running-the-replay-on-it)
- [Real-data results: data not provided yet](#real-data-results-data-not-provided-yet)
- [Synthetic results: the closed loop](#synthetic-results-the-closed-loop)
- [Synthetic results: the hand-written fixtures](#synthetic-results-the-hand-written-fixtures)
- [The matching-agreement check and its synthetic examples](#the-matching-agreement-check-and-its-synthetic-examples)
- [The small-L finding (synthetic)](#the-small-l-finding-synthetic)
- [Mutation canaries](#mutation-canaries)
- [What others did, and what lobsim does](#what-others-did-and-what-lobsim-does)
- [Limitations](#limitations)
- [References](#references)

## What the replay checks, and why

LOBSTER publishes, for one stock and one day, a **message file** (one row per event) and an
**orderbook file** whose row *k* is the book right after message *k*. `lobsim_lobster` replays the
messages into lobsim's own `OrderBook` and compares the book with the published row after every
message. Two questions are answered separately.

1. **Does the reconstructed book equal the published one?** Messages are applied as the stream says
   (a new order is placed, a cancel or execution reduces a resting order), never by re-matching, because
   the stream is already the result of matching. The share of rows that are exact to depth 1, 2, ... L,
   *before any correction*, is the headline. Deep levels are expected to be less exact than the touch
   (see [Limitations](#limitations)).
2. **Does lobsim's matching rule agree with the data?** Before each burst of executions is applied, a
   read-only `preview_match` predicts which resting orders an aggressive order of that size would hit,
   and the prediction is compared with the burst fill by fill and classified (exact, queue order,
   price skip, size split, length). Only bursts that start from a trusted book (the previous row exact
   to the trust depth) are scored; the rest are counted as excluded.

Three modes: `strict` stops at the first row that is not exact down to `--compare-depth` (exit code 2);
`report` never changes the book and only counts; `resync` counts, then corrects the book to the
published row inside its window (and counts every correction). No heuristic deletes orders. Every
message that is skipped is counted in a named counter, never dropped silently.

## The LOBSTER files as the code reads them

**Message file**, one line per event, six comma-separated columns:

| Column | Meaning |
|---|---|
| Time | seconds after midnight, with up to 9 decimals; a longer fraction is cut to 9 digits and counted in `time_truncated` |
| Type | 1 new limit order, 2 partial cancel, 3 full delete, 4 execution of a visible order, 5 execution of a hidden order, 6 cross (auction) trade, 7 trading halt |
| Order ID | the order reference number |
| Size | shares |
| Price | dollars x 10,000 (91.14 dollars is 911400); kept as an integer, never as a fraction |
| Direction | -1 sell, 1 buy; **for an execution (type 4) it is the side of the resting order that was hit, not the aggressor's side**. Types 6 and 7 may carry 0 (D-161) |

Hidden executions (5), cross trades (6) and halts (7) are not applied to the book; they are counted.

**Orderbook file**, one line per message, `4 x L` columns: for level 1 the ask price, ask size, bid price
and bid size, then level 2, and so on to level `L`. A level whose size is 0 is empty, whatever its price
column holds (lobsim's own files and fixtures write `9999999999` and `-9999999999` there; the parser does
not depend on that, D-163).

**File names and L.** `<TICKER>_<date>_34200000_57600000_message_<L>.csv` and `..._orderbook_<L>.csv`
(the two numbers are 9:30 and 16:00 in milliseconds after midnight). `L` is read from the orderbook file
name and must agree with its column count; the message file name's `L`, if it has one, must agree too.

**Line numbering and the opening book.** Lines are numbered from 1, the same in both files. The book is
not empty at 9:30, so line 1 of the orderbook file is used to **seed** the replay book with synthetic
orders (one per published level, with ids outside the real range), and message 1 is counted as
consumed by the seed. Later cancels and executions that name an order the seed holds are taken from a
synthetic order at that price on that side (*seed attribution*), which loses the order's identity and
queue position.

## Getting data, and running the replay on it

Routes to LOBSTER files (all outside this repository):

1. an account at LOBSTER and a data request (check their current terms);
2. a university subscription to LOBSTER data (some university libraries provide access);
3. an approved trial from LOBSTER;
4. access to the sample files on proof of owning the book *Trades, Quotes and Prices* (this was the
   condition when this was written).

The 21 June 2012 samples for four tickers were free earlier; if a copy is available to you it works the
same way. Put the files in `data/lobster/`, which is git-ignored (never commit them). For a 10-level
sample of one ticker the code expects exactly this pair:

```
data/lobster/INTC_2012-06-21_34200000_57600000_message_10.csv
data/lobster/INTC_2012-06-21_34200000_57600000_orderbook_10.csv
```

Build and run (from the repository root):

```
cmake --preset release && cmake --build --preset release -j
M=data/lobster/INTC_2012-06-21_34200000_57600000_message_10.csv
B=data/lobster/INTC_2012-06-21_34200000_57600000_orderbook_10.csv

build/release/lobsim_lobster --messages $M --orderbook $B --mode strict  --compare-depth 10 --out out/intc_strict
build/release/lobsim_lobster --messages $M --orderbook $B --mode report  --out out/intc_report
build/release/lobsim_lobster --messages $M --orderbook $B --mode resync  --out out/intc_resync
build/release/lobsim_lobster --messages $M --orderbook $B --mode report  --trust-depth 1   # also try 3 (default) and 10
```

`--compare-depth D` (1..L) is how deep a strict run must be exact. `--trust-depth T` (default 3) is the
depth to which the previous row must have been exact for a burst to be scored. With `--out`, the tool
writes `summary.json` (counters, exact rows per depth, corrections, matching counts and examples; the
deterministic part is what the fixture tests compare) and `divergences.csv` (the first 1,000 rows that
are not exact at depth `L`). It prints a one-screen summary. Exit codes: 0 done, 1 bad arguments or
input that cannot be replayed, 2 strict failure.

To try the tool without LOBSTER data, export a simulator run in the same format and replay it (the
closed loop below does exactly this; `--filter-window` imitates LOBSTER's price-range filter):

```
build/release/lobsim_run --config configs/default.json --seed 7 --duration-s 60 --out out/run
build/release/lobsim_export_lobster --run out/run --levels 10 --out out/lobster [--filter-window]
build/release/lobsim_lobster --messages out/lobster/SIM_2012-06-21_34200000_57600000_message_10.csv \
  --orderbook out/lobster/SIM_2012-06-21_34200000_57600000_orderbook_10.csv --mode strict --compare-depth 10
```

## Real-data results: data not provided yet

**No real data has been replayed. This block holds no numbers on purpose.** When files are available,
this is what to record here, for at least two tickers, from the three modes above:

- exact-row percentage at depths 1, 2, 5 and L, before any correction;
- the corrections histogram (by level and by rank) in resync mode;
- every counter of `summary.json`, and the crossed-state percentage in report mode;
- messages per second;
- the matching-agreement numerator and denominator at trust depths 1, 3 and L, with every exclusion
  count, and three real examples of each mismatch class;
- how many messages each canary below needs to be detected on the real ticker.

The canary runs on a real ticker could not be done for the same reason.

## Synthetic results: the closed loop

**These numbers are synthetic**: lobsim's simulator (default config, seed 7, 60 simulated seconds) is
exported to LOBSTER format with 10 levels and replayed, by the ctests `lobster_closed_loop_unfiltered`
and `lobster_closed_loop_filtered` (`tests/lobster_closed_loop.cmake`). Both the data and the replay use
lobsim's own matching engine, so this checks the replayer's bookkeeping against a book it should
reproduce, not lobsim against a market.

**Unfiltered export, strict mode at depth 10 (synthetic).** 4,621 messages, every row exact at every depth:

| depth (synthetic) | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| exact rows before correction | 4621/4621 (100.00%) | 100.00% | 100.00% | 100.00% | 100.00% | 100.00% | 100.00% | 100.00% | 100.00% | 100.00% |

Counters (synthetic): submitted 2,670, cancelled 0, deleted 1,298, executed 651, `consumed_by_seed` 1,
`seed_orders` 1, `seed_attributed` 1 (the one later reference to message 1's order, which the seed holds,
as the test requires); every other counter is 0 (`unknown_id`, `dup_id`, `submit_would_cross`,
`submit_bad_price`, `direction_mismatch`, `price_mismatch`, `delete_size_mismatch`, `oversized_reduce`,
`parse_rejected`, `time_truncated`, `oracle_malformed`, `oracle_rejected`, `crossed_state`, `hidden_exec`,
`cross_trade`, `halt`). Resync made no correction. The simulator has no hidden orders, so the export has no
type 5 messages.

**Matching agreement, same run (synthetic).** 370 bursts (651 executions):

| trust depth T (synthetic) | agree / scored | exact | via synthetic order | queue order | price skip | size split | length | excluded unknown | excluded untrusted |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 / 0 (n/a) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 370 |
| 3 (headline) | 370 / 370 (100.00%) | 370 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 10 | 370 / 370 (100.00%) | 370 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

At T = 1 every burst is excluded: a burst needs `T` greater than its number of distinct prices, and a
depth of 1 never is. Lobsim matches itself exactly because the data was produced by the same rules; a real
market is not expected to give 100%.

**Filtered export, resync mode at L = 10 (synthetic).** The export keeps only messages inside the top-10
window, as LOBSTER does: 38 of the 4,621 messages are dropped, 4,583 replayed.

| depth (synthetic) | 1 to 8 | 9 | 10 |
|---|---|---|---|
| exact rows before correction | 4583/4583 (100.00%) | 4582/4583 (99.97%) | 4560/4583 (99.49%) |

Counters (synthetic): submitted 2,670, deleted 1,246, executed 651, `seed_attributed` 1, `unknown_id` 14,
`delete_size_mismatch` 2, everything else 0. Corrections histogram (synthetic), 23 rows corrected:

| level / rank (synthetic) | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| levels corrected, by oracle level | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 21 |
| levels removed, by rank | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 |

Corrections sit at the deepest levels, which is the expected effect of price-range filtering; none is at
level 1. Matching agreement (synthetic): T = 3 370/370 (100.00%), T = 10 369/369 (100.00%, one burst
excluded as untrusted), T = 1 n/a (370 excluded untrusted).

Replay speed on the same files (synthetic data, Debug build, one core of the development machine):
about 52,000 to 53,000 messages per second; this is a Debug figure and says nothing about a real file.

## Synthetic results: the hand-written fixtures

**Synthetic**, written by hand from the LOBSTER format rules, not produced by the exporter, in
`tests/fixtures/lobster/<case>/` (with `expected_summary.json`, compared exactly by `replay_tests`).
"Exact" below is the share of rows exact at every depth, before correction.

| fixture (synthetic) | L | lines | mode | what it holds | outcome |
|---|---|---|---|---|---|
| `basic_types` | 3 | 16 | resync | types 1 to 7, seeding, references to a pre-open order and to message 1's order, a type 5 that must not change the book, empty-level placeholders | 100.00% exact; 3 seed attributions; matching at T = 3: 2 via synthetic, 1 queue order |
| `burst` | 3 | 12 | resync | a 3-execution burst over two prices, a 2-execution burst starting on a seed order | 100.00% exact; matching: 1 exact, 1 via synthetic |
| `anomalies` | 3 | 14 | report | duplicate id, crossing submission, oversized execution, delete size mismatch, direction and price mismatch, an unknown id, a rejected line, a truncated time | 5 rows not exact; one of each counter; matching: 1 price skip, 1 length |
| `resync` | 3 | 10 | resync | corrections by level, removals by rank, a level outside the window left alone, a side with fewer than L levels | 5 rows not exact before correction; corrected by level [1, 0, 2], removed by rank [1, 1, 1] |
| `strict_failure` | 2 | 4 | strict | a direction mismatch that leaves the book wrong | strict mode (compare depth 1) stops at line 4 and prints both books and the last messages; exit code 2 |
| `matching` | 3 | 27 | resync | one burst per matching class | 100.00% exact; 9 bursts, one of each class (see below) |
| `hidden_exec` | 3 | 5 | report | type 5 messages at a price where seed liquidity sits | 100.00% exact; 2 hidden executions skipped; needed to catch `APPLY_HIDDEN` |

## The matching-agreement check and its synthetic examples

A burst is a maximal run of consecutive type-4 messages with the same time and the same Direction, read
as one aggressive order. Classes (each burst gets one): `exact`; `exact_via_synthetic` (equal when
synthetic orders are compared by price and size only); `queue_order` (first difference at the same
price, a different order); `price_skip` (a different price, split into the prediction being at a better
or worse price for the taker); `size_split` (same order and price, a different size); `length` (every
compared fill equal, a different number of fills); `excluded_unknown` (a message names an id that is
neither resting nor attributable to a synthetic order); `excluded_untrusted` (the previous row was not
exact to the trust depth, or the depth is too shallow). Agreement is (exact + via synthetic) divided by
(bursts - both exclusions), always printed as numerator / denominator.

The spec asks for three real examples of each mismatch type. **They will be filled in from real data.**
Until then, these are the **synthetic** examples of the hand-written `matching` fixture (made up to
contain each class; they are hypotheses about what such bursts would look like, not observations):

| class (synthetic example) | burst in the data | lobsim's prediction | what it would suggest on real data |
|---|---|---|---|
| queue order | `5 10 @ 2000` | `2 10 @ 2000` | the data fills order 5 first although order 2 is ahead of it in lobsim's queue at the same price: a priority difference or a missed re-queue |
| price skip, better | `4 5 @ 2200` | `2 5 @ 2000` | the data fills at 2200 while lobsim would have filled at 2000: liquidity lobsim has but the data does not (a stale order) |
| price skip, worse | `6 5 @ 2000`, `4 5 @ 2000`, `3 5 @ 2100` | `6 5 @ 2000`, `3 10 @ 2100` | the data fills at a price where lobsim's book had less: lobsim is missing liquidity |
| size split | `2 10 @ 2000`, `3 5 @ 2100` | `2 15 @ 2000` | the same order is filled for less than lobsim predicts and the rest goes to the next price: a size or hidden-quantity difference |
| length | `2 5 @ 2000`, `4 5 @ 2000` | `2 5 @ 2000` | the data has one more fill than the prediction, with every fill before it equal |

## The small-L finding (synthetic)

Under window filtering, an order whose delete happens while its price is outside the window keeps a stale
entry in the replayed book. The resync only corrects a level's net size, and seed attribution only draws
on synthetic orders, so when that price later returns to the touch, the executions of the orders the
replay never saw are `unknown_id`, and the stale known orders are never reduced. This shows up as
**level-1 corrections** once the window is small. It follows from the stated rules under filtered data;
it is a limit of filtered data, not an engine bug, and no heuristic is used to hide it.

**Synthetic measurement**, 600 simulated seconds, seed 7, default config, exported with `--filter-window`
and replayed in resync mode (rows are before correction):

| L (synthetic) | messages replayed | filtered out | level-1 exact rows | level-1 corrections | `unknown_id` | corrections by level |
|---|---|---|---|---|---|---|
| 10 | 56,203 | 1,079 | 100.00% | 0 | 591 | 0, 0, 17, 56, 74, 102, 158, 179, 246, 462 |
| 5 | 32,878 | 24,404 | 96.03% | 1,297 | 2,817 | 1,297, 421, 508, 569, 722 |
| 3 | 23,180 | 34,102 | 91.42% | 1,978 | 2,576 | 1,978, 456, 657 |
| 1 | 13,928 | 43,354 | 86.05% | 1,941 | 1,081 | 1,941 |

At L = 10 the 600 s run (and the 60 s run above) had no level-1 correction. At L = 1 the touch is also the
deepest level, so every level that surfaces is corrected there. A level-1 correction in a filtered replay
is therefore a bug signal only when the window is deep relative to the book's churn, as it is at L = 10
in these synthetic runs; whether L = 10 is deep enough for a real ticker is not known.

## Mutation canaries

Three deliberate bugs in the replayer (`LOBSIM_REPLAY_BUG`, a build with only `lob`, `replay` and
`lobsim_lobster`, never combined with the normal tests) check that the replay can fail:

- `DIRECTION_IS_AGGRESSOR`: an execution's Direction is read as the aggressor's side;
- `APPLY_HIDDEN`: type 5 messages are applied like executions;
- `NO_SEED`: the opening book is not seeded (message 1 still counts as consumed by the seed).

Detection means `lobsim_lobster --mode strict --compare-depth L` exits with code 2; "messages to
detection" is the strict-mode row of the first row that is not exact. **Synthetic inputs only** (no real
ticker): "no" means the replay was exact to the end.

| input (synthetic) | no bug | DIRECTION_IS_AGGRESSOR | APPLY_HIDDEN | NO_SEED |
|---|---|---|---|---|
| fixture `basic_types` | exact | caught, row 5 | no | caught, row 1 |
| fixture `burst` | exact | caught, row 6 | no | caught, row 1 |
| fixture `matching` | exact | caught, row 8 | no | caught, row 1 |
| fixture `hidden_exec` | exact | caught, row 5 | caught, row 3 | caught, row 1 |
| closed loop (60 s, seed 7, 10 levels) | exact | caught, row 3 | no | caught, row 1 |

`APPLY_HIDDEN` can only be caught by data that contains type 5 messages that touch the visible book. The
simulator has no hidden orders, so its export has none; the hand-written fixture `hidden_exec` (a type 5
at a price where seed liquidity sits; the type 5 row of `basic_types` is at a price with nothing on that
side, so applying it changes nothing) is what catches it. Every canary is caught by at least one
committed fixture. CI runs the three canaries (job `replay-canary`) and passes only when each replay
exits with code 2.

## What others did, and what lobsim does

Other open projects that replayed LOBSTER data ([10], [11], [12]) and the libraries around it ([5]),
and what lobsim does about each point:

| Others | Good or bad | lobsim |
|---|---|---|
| Seeded the opening book from the first published row, using synthetic ids outside LOBSTER's id range [11] | Good | The same idea, simplified: one synthetic order per published level |
| Compared every published row, strict mode stops at the first unexplained level [11] | Good | `strict`, `report` and `resync` modes |
| Counted resync corrections by depth, since corrections at the touch would mean a real engine bug [11] | Good | The same, and the corrections histogram is a headline output |
| Applied executions as reductions, not by re-matching, because the stream is already post-match [11] | Good | The same, plus a separate read-only matching check |
| Validated every message before changing the book; a rejected message leaves the book unchanged [12] | Good | The same (direction and price mismatches are skipped and counted) |
| Counted ignored messages rather than dropping them silently [10] | Good | The same: every message lands in exactly one outcome counter |
| Ran mutation checks and reported where bugs were caught [10] | Good | The three mutation canaries above |
| Used synthetic fixtures in CI because real data cannot be redistributed [11] | Good | The same, plus fixtures generated by lobsim's own simulator |
| An "uncross" heuristic that deletes stale orders when the book is crossed, with accuracy not measured [12] | Bad | No heuristic deletions: resync uses only the published row and every correction is counted |
| Resync results that lean heavily on the published ladder [10] | Risk | Exact-row rates are reported before correction, and matching is scored only on rows where the book was trusted |
| MeatPy raises execution-priority exceptions but leaves interpretation to the user [5] | Partial | A full, classified matching-agreement report |

## Limitations

- **No real data yet.** Nothing here says how lobsim behaves on a real ticker.
- When data exists it will be one day (21 June 2012) of Nasdaq data, for the few tickers available: not
  other days, other exchanges or other regimes.
- Deep levels are structurally unreliable on filtered data: LOBSTER only emits messages for events inside
  the requested levels, so liquidity can enter or leave the window with no message. Deep-level
  mismatches are expected and say little about the engine; the touch is what the replay stands behind.
- Seed attribution loses order identity and queue position for everything resting at the open, and
  message 1's order lives inside a synthetic seed order.
- Hidden liquidity is excluded: type 5 executions are skipped, so anything they trade against is not in
  the book (and so cannot be in the prediction).
- Auction (cross) trades and halts are counted, not reconstructed.
- The synthetic results show the replayer is consistent with lobsim's own matching engine; they do not
  show that the engine's rules match any market's.

## References

1. Copenhagen U. course notes, LOBSTER file naming and structure: https://aef-course.ku.dk/exercises/high-frequency-econometrics.html
2. Bouchaud et al., *Trades, Quotes and Prices*, appendix on LOBSTER files: https://cambridge.org/core/books/trades-quotes-and-prices/appendix/D21F35ED820E7D53DDEC7BD641FE82F8
3. University of Vienna data access, LOBSTER source data: https://vdc.univie.ac.at/data-access/lobster
4. LOBSTER, reconstruction details (initial book, type C, broken trades): https://lobster-data.de/info/TEST.php and https://data.lobsterdata.com/info/help_reconstruction.php
5. MeatPy, getting started (ITCH parsing, execution-priority exceptions, sample files): https://meatpy.readthedocs.io/en/latest/getting_started.html
6. oguztecirlioglu/limit-order-book, LOBSTER format notes: https://github.com/oguztecirlioglu/limit-order-book
7. Nasdaq TotalView-ITCH 5.0 specification: https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
8. Databento, Nasdaq PSX TotalView-ITCH normalization: https://databento.com/docs/venues-and-datasets/xpsx-itch
9. Elite Trader thread, hidden execution messages in ITCH: https://www.elitetrader.com/et/threads/reconstructing-lob-from-nasdaq-itch-feed-hidden-execution-messages.293711
10. Tejas356/limit_order_book_engine: https://github.com/Tejas356/limit_order_book_engine
11. yashshelar466/High-Frequency-Limit-Order-Book: https://github.com/yashshelar466/High-Frequency-Limit-Order-Book
12. MouadQ793/orderbook-execution-sim: https://github.com/MouadQ793/orderbook-execution-sim
