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
- [Dress rehearsal on synthetic LOBSTER-like data (NOT real market data)](#dress-rehearsal-on-synthetic-lobster-like-data-not-real-market-data)
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
orders (one per published level; they carry no external order id at all, the engine gives them ids from
its own dense range), and message 1 is counted as consumed by the seed. Later cancels and executions that name an order the seed holds are taken from a
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
input that exists but cannot be replayed, 2 strict failure, 3 a given file does not exist (the message names
the files to get and the folder `data/lobster/`).

Or, with the files in `data/lobster/`, run the whole sequence and write the results into the marked block of
the next section with one command: `scripts/run_real_data.sh` (options `--data-dir`, `--report`, `--out`,
`--bin`; exit 3 when the folder holds no pair).

To try the tool without LOBSTER data, export a simulator run in the same format and replay it (the
closed loop below does exactly this; `--filter-window` imitates LOBSTER's price-range filter):

```
build/release/lobsim_run --config configs/default.json --seed 7 --duration-s 60 --out out/run
build/release/lobsim_export_lobster --run out/run --levels 10 --out out/lobster [--filter-window]
build/release/lobsim_lobster --messages out/lobster/SIM_2012-06-21_34200000_57600000_message_10.csv \
  --orderbook out/lobster/SIM_2012-06-21_34200000_57600000_orderbook_10.csv --mode strict --compare-depth 10
```

## Real-data results: data not provided yet

<!-- REAL-DATA:BEGIN -->
### Data not provided yet

No LOBSTER files were found, so **no real-data results exist**. Nothing is reported here on purpose.

- Expected file names, one pair per ticker and day (L is the number of levels):
  `<TICKER>_<DATE>_34200000_57600000_message_<L>.csv` and
  `<TICKER>_<DATE>_34200000_57600000_orderbook_<L>.csv`, for example
  `INTC_2012-06-21_34200000_57600000_message_10.csv` and
  `INTC_2012-06-21_34200000_57600000_orderbook_10.csv`.
- Put them in the folder `data/lobster/` (it is not tracked by git).
- Run one command, which replays every pair in all three modes and replaces this block with the results:

```
scripts/run_real_data.sh
```

LOBSTER data needs a LOBSTER account, a university subscription, an approved trial, or the sample that comes with the book *Trades, Quotes and Prices*.
<!-- REAL-DATA:END -->

**No real data has been replayed, and the marked block above holds no numbers on purpose.**
`scripts/run_real_data.sh` replaces it with the results of every pair in `data/lobster/`. The results are
expected to cover, for at least two tickers, from the three modes above:

- exact-row percentage at depths 1, 2, 5 and L, before any correction;
- the corrections histogram (by level and by rank) in resync mode;
- every counter of `summary.json`, and the crossed-state percentage in report mode;
- messages per second;
- the matching-agreement numerator and denominator at trust depths 1, 3 and L, with every exclusion
  count, and three real examples of each mismatch class;
- how many messages each canary below needs to be detected on the real ticker.

The canary runs on a real ticker could not be done for the same reason.

**First things to check on real data.** These are risks that cannot be examined without files:

- If `price_mismatch` is large, revisit D-170. A type 2, 3 or 4 message whose price differs from its
  known order's price is skipped and counted. LOBSTER might print the execution price for an ITCH
  "execute with price" (C) message, which can differ from the resting order's price, so such executions
  would be skipped wrongly. The code assumes the message price is the resting order's price.
- If `unknown_id` or `direction_mismatch` is large, look at the first examples in `divergences.csv`
  and the strict-mode report before drawing any conclusion about deep levels.
- The empty-level placeholder of the real orderbook files was never verified (D-163): confirm that the
  parser counts no `oracle_malformed` rows.

## Dress rehearsal on synthetic LOBSTER-like data (NOT real market data)

**Everything in this section is synthetic.** `SYNTHA` and `SYNTHB` are made-up tickers: one hour of
lobsim's own simulator each (`SYNTHA`: default config; `SYNTHB`: informed traders x4, noise traders x2),
exported by `lobsim_export_lobster --lobster-like` with LOBSTER's quirks: the export starts after a 300 s
warm-up (the book is already full), messages outside the top-10 window are dropped (price-range
filtering), a Poisson stream of hidden executions (type 5), and one trading halt (type 7). The halt
encoding (type 7, price -1, size 0, id 0, direction 1) and the empty-level placeholder (price 0, size 0)
are **our own stand-ins**; real LOBSTER files may encode both differently. The replay skips any type 7
whatever its other fields hold, and treats any level of size 0 as empty.

The full real-data sequence ran on both days through the same code path real files will use:

```
scripts/make_fake_day.sh                    # writes data/synthetic/SYNTHA and data/synthetic/SYNTHB
scripts/run_real_data.sh --data-dir data/synthetic/SYNTHA --report <scratch file> --out <scratch dir>
scripts/run_real_data.sh --data-dir data/synthetic/SYNTHB --report <scratch file> --out <scratch dir>
tools/lobster_audit.py --audit data/synthetic/SYNTHA/SYNTHA_audit.csv --messages ... --orderbook ... \
    --summary <scratch dir>/SYNTHA_2012-06-21_34200000_57600000_10/resync/summary.json
```

**Conclusion.** Without price-window filtering, the fake day meets every expectation of the dress
rehearsal: no level-1 correction, every scored burst agrees, direction, price and delete-size mismatches
are 0, and the hidden executions and the halt are counted exactly as injected. **With** filtering it
cannot, under the replay rules as specified: a known order whose delete happens while its price is
outside the window gets no message and stays in the replayed book, and the resync rule ("too big: reduce
orders from the tail") then cuts the newest, real orders of its level instead of the stale one. That
produces level-1 corrections, delete-size mismatches, unknown ids and `queue_order` bursts. Every one of
them was traced, item by item, to a delete that the filter dropped (or to the opening row's depth), and
putting only the dropped deletes back makes the day as clean as the unfiltered one (no level-1
correction, every scored burst agrees, no delete-size mismatch). Real LOBSTER files drop the same messages, so
real data will show the same effect; other projects report 1.4-2% of orders never removed by any
message. No check was loosened and nothing was excluded to reach these numbers.

### The full fake days through `run_real_data.sh` (synthetic)

Exact rows before any correction, by depth (synthetic):

| day, mode (synthetic) | lines | depth 1 | depth 2 | depth 5 | depth 10 |
|---|---|---|---|---|---|
| SYNTHA strict (stops at line 30,305) | 30,305 | 99.99% (30,304) | 91.98% | 65.14% | 3.60% |
| SYNTHA report | 356,897 | 15.01% | 9.16% | 5.53% | 0.30% |
| SYNTHA resync | 356,897 | 99.21% (354,090) | 99.03% | 98.23% | 95.55% |
| SYNTHB strict (stops at line 23,943) | 23,943 | 99.99% (23,942) | 90.20% | 54.29% | 0.24% |
| SYNTHB report | 710,477 | 3.47% | 3.06% | 1.82% | 0.00% |
| SYNTHB resync | 710,477 | 98.98% (703,294) | 98.83% | 98.31% | 96.88% |

`crossed_state` is 0 in every run. Strict mode compares down to depth 1 by default and stops at the first
row that is not exact; report mode never corrects, so an uncorrected book drifts far.

Resync corrections histogram (synthetic):

| day (synthetic) | corrected, by oracle level 1 to 10 | removed, by rank 1 to 10 | removed beyond L | rows corrected |
|---|---|---|---|---|
| SYNTHA | 2,762, 637, 764, 927, 1,172, 1,213, 1,401, 1,629, 1,833, 3,472 | 45, 0, 0, 0, 0, 0, 0, 0, 0, 14 | 80 | 15,866 |
| SYNTHB | 7,071, 1,082, 1,160, 1,224, 1,306, 1,219, 1,314, 1,370, 1,480, 4,788 | 112, 0, 0, 0, 0, 0, 0, 0, 0, 6 | 100 | 22,131 |

Levels 1 and 10 have the most corrections (SYNTHB: level 1 the most), unlike the closed loop below: the
stale orders reach the touch when the price comes back.

Counters, resync run (synthetic):

| counter (synthetic) | SYNTHA | SYNTHB |
|---|---|---|
| messages (type 1 / 3 / 4 / 5 / 7) | 356,897 (170,864 / 133,927 / 51,414 / 691 / 1) | 710,477 (340,903 / 252,151 / 116,676 / 746 / 1) |
| submitted, deleted, executed | 170,864, 127,430, 49,343 | 340,902, 244,037, 111,460 |
| `seed_orders`, `consumed_by_seed` | 20, 1 | 20, 1 |
| `seed_attributed`, `unknown_id` | 997, 7,570 | 2,194, 11,136 |
| `delete_size_mismatch`, `oversized_reduce` | 6,243, 527 | 6,665, 1,543 |
| `direction_mismatch`, `price_mismatch`, `dup_id`, `submit_would_cross` | 0, 0, 0, 0 | 0, 0, 0, 0 |
| `hidden_exec` (injected) | 691 (691) | 746 (746) |
| `halt` (injected) | 1 (1) | 1 (1) |
| `parse_rejected`, `time_truncated`, `oracle_malformed`, `oracle_rejected` | 0, 0, 0, 0 | 0, 0, 0, 0 |

Matching agreement, resync run (synthetic; agree / scored, with every exclusion):

| day, trust depth T (synthetic) | agree / scored | exact | via synthetic | queue order | size split | price skip, length | excluded unknown | excluded untrusted |
|---|---|---|---|---|---|---|---|---|
| SYNTHA, T = 1 | 0 / 0 (n/a) | 0 | 0 | 0 | 0 | 0, 0 | 1,305 | 29,126 |
| SYNTHA, T = 3 (headline) | 25,334 / 28,881 (87.71%) | 25,204 | 130 | 3,547 | 0 | 0, 0 | 1,305 | 245 |
| SYNTHA, T = 10 | 24,497 / 27,977 (87.56%) | 24,367 | 130 | 3,480 | 0 | 0, 0 | 1,305 | 1,149 |
| SYNTHB, T = 1 | 0 / 0 (n/a) | 0 | 0 | 0 | 0 | 0, 0 | 3,471 | 65,603 |
| SYNTHB, T = 3 (headline) | 47,389 / 65,148 (72.74%) | 47,102 | 287 | 17,757 | 2 | 0, 0 | 3,471 | 455 |
| SYNTHB, T = 10 | 46,541 / 64,117 (72.58%) | 46,255 | 286 | 17,574 | 2 | 0, 0 | 3,471 | 1,486 |

SYNTHA has 30,431 bursts and SYNTHB 69,074. In strict and report mode every scored burst agrees (SYNTHA
T = 3: 2,043/2,043 and 2,074/2,074), but few are scored: a burst counts only when the row before it is
exact before correction, and without resync few rows stay exact.

### Taking the quirks apart (synthetic)

Each day was exported five times from one simulator run with the exporter's switches (`--no-filter-window`,
`--no-halt`, `--hidden-rate 0`, `--window-start-s 0`) and replayed in resync mode. Direction and price
mismatches are 0 in all ten runs.

| SYNTHA variant (synthetic) | lines | filtered | level-1 corrections | corrections by level 1 to 10 | `unknown_id` | delete-size mismatch | oversized | agreement, T = 3 | queue order | hidden / halt |
|---|---|---|---|---|---|---|---|---|---|---|
| (a) window start only | 364,599 | 0 | 0 | 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 | 31 | 0 | 0 | 30,403 / 30,403 (100.00%) | 0 | 0 / 0 |
| (b) (a) + hidden executions + halt | 365,291 | 0 | 0 | 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 | 31 | 0 | 0 | 30,403 / 30,403 (100.00%) | 0 | 691 / 1 |
| (c) filter only | 385,661 | 8,951 | 3,323 | 3,270, 745, 859, 1,049, 1,305, 1,308, 1,480, 1,699, 1,900, 3,739 | 8,359 | 6,760 | 643 | 26,935 / 31,094 (86.62%) | 4,159 | 0 / 0 |
| (d) the full fake day | 356,897 | 8,394 | 2,807 | 2,762, 637, 764, 927, 1,172, 1,213, 1,401, 1,629, 1,833, 3,472 | 7,570 | 6,243 | 527 | 25,334 / 28,881 (87.71%) | 3,547 | 691 / 1 |
| (e) no window start, no filter | 394,612 | 0 | 0 | all 0 | 0 | 0 | 0 | 32,897 / 32,897 (100.00%) | 0 | 0 / 0 |

| SYNTHB variant (synthetic) | lines | filtered | level-1 corrections | corrections by level 1 to 10 | `unknown_id` | delete-size mismatch | oversized | agreement, T = 3 | queue order | hidden / halt |
|---|---|---|---|---|---|---|---|---|---|---|
| (a) window start only | 733,994 | 0 | 0 | 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 | 29 | 0 | 0 | 69,041 / 69,041 (100.00%) | 0 | 0 / 0 |
| (b) (a) + hidden executions + halt | 734,741 | 0 | 0 | 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 | 29 | 0 | 0 | 69,041 / 69,041 (100.00%) | 0 | 746 / 1 |
| (c) filter only | 767,961 | 25,606 | 8,248 | 8,128, 1,195, 1,306, 1,400, 1,561, 1,549, 1,586, 1,634, 1,812, 5,377 | 13,166 | 7,779 | 1,697 | 51,265 / 69,751 (73.49%) | 18,486 | 0 / 0 |
| (d) the full fake day | 710,477 | 24,264 | 7,183 | 7,071, 1,082, 1,160, 1,224, 1,306, 1,219, 1,314, 1,370, 1,480, 4,788 | 11,136 | 6,665 | 1,543 | 47,389 / 65,148 (72.74%) | 17,757 | 746 / 1 |
| (e) no window start, no filter | 793,567 | 0 | 0 | all 0 | 0 | 0 | 0 | 74,254 / 74,254 (100.00%) | 0 | 0 / 0 |

(Level-1 corrections count corrected levels plus removed levels at rank 1. SYNTHB (d) also has 2
`size_split` bursts, explained below.)

The rehearsal found one real bug, now fixed (D-211): two aggressive orders at the same nanosecond that
split one resting order were scored as a `size_split`; it was the only mismatched burst of every
unfiltered variant before the fix.

Variants (a) and (b) are not entirely free of corrections: each has one correction at level 10 and 31
(SYNTHA) or 29 (SYNTHB) unknown ids. Every one of those messages names an order that rested **below the
10 published levels when the window opened** (SYNTHA's true book had 15 ask levels at line 1), referenced
before its price was ever inside the published window; the correction is such a level coming into view.
The opening row shows only L levels, so nothing in the files can tell the replay about those orders.
SYNTHA's variant (a) exported with 20 levels has no unknown id, no correction, and exactly 31 more seed
attributions. Real files have the same limit.

### Where every discrepancy of the filtered days comes from (synthetic)

`tools/lobster_audit.py` replays the files again independently, requires every number of `summary.json`
to come out the same, checks the exporter's ground truth (`--audit`) against every orderbook row and every
burst, and then traces each discrepancy to a root (see D-214 for the rules). It reproduced all ten runs
above exactly and left nothing unexplained. Full fake days (synthetic):

| item (synthetic) | SYNTHA | SYNTHB | root |
|---|---|---|---|
| level-1 corrections and removals | 2,807 | 7,183 | a stale order at that level, its delete filtered (all) |
| ... the message on that line: unknown id of an order cut by a resync trim | 2,085 | 5,175 | |
| ... an oversized execution of a trimmed order | 527 | 1,543 | |
| ... a delete of a trimmed order (size mismatch) | 195 | 465 | |
| corrected levels, every depth | 15,810 | 22,014 | stale order 15,805 / 21,988; synthetic order holding filtered-deleted quantity 4 / 22; filtered submission 0 / 2; order cut by an explained trim 0 / 1; opening depth 1 / 1 |
| removed levels, every rank | 139 | 218 | stale order 137 / 218; synthetic order holding filtered-deleted quantity 2 / 0 |
| `delete_size_mismatch` | 6,243 | 6,665 | an order cut by a resync trim (all) |
| `oversized_reduce` | 527 | 1,543 | an order cut by a resync trim (all) |
| `unknown_id` | 7,570 | 11,136 | an order cut by a resync trim 7,569 / 11,135; a seed order drawn down by trimmed orders 1 / 0; a filtered submission 0 / 1 |
| bursts whose comparison disagrees (scored or not) | 3,600 | 17,897 | stale order 3,600 / 17,785; synthetic order holding filtered-deleted quantity 0 / 112 |
| `excluded_unknown` bursts | 1,305 | 3,471 | an order cut by a resync trim (all) |

Every resync trim that cut a real order was itself explained: its level held a stale order whose delete
was filtered, or a synthetic order holding the quantity of orders deleted outside the window. The filter
dropped 8,393 deletes and 1 submission in SYNTHA (d), 24,258 deletes and 6
submissions in SYNTHB (d).

**Counterfactual (synthetic).** The same full days with only the filtered **deletes** put back (rows are
the true book after each message):

| (synthetic) | level-1 corrections | corrections by level | `unknown_id` | delete-size mismatch | agreement, T = 3 |
|---|---|---|---|---|---|
| SYNTHA, deletes put back | 0 | 0, ..., 0, 1 (level 10) | 32 | 0 | 30,403 / 30,403 (100.00%) |
| SYNTHB, deletes put back | 0 | 0, ..., 0, 7 (level 10), 1 removal at rank 10 | 30 | 0 | 69,041 / 69,041 (100.00%) |
| SYNTHA, submissions put back instead | 2,807 | as (d), one more at level 10 | 7,570 | 6,243 | 25,334 / 28,881 (87.71%) |

With the deletes back, each day is variant (b) plus the effect of its few filtered submissions (one more
unknown id each, and a few corrections at level 10).

**An alternative that was measured, not adopted.** Trimming an over-full level from its oldest orders
instead of the newest, measured with the independent replay of the audit tool, cuts SYNTHA's level-1
corrections from 2,807 to 1 (corrections by level 1, 4, 1, 7, 20, 56, 98, 189, 382, 2,325; delete-size
mismatches 187; agreement 30,402/30,403) and SYNTHB's from 7,183 to 71 (68,969/68,974). It departs from
the replay rule of spec upgrade 2, 4.4 ("reduce orders from the tail, which keeps older orders' queue
positions") and still does not reach zero, so the replayer keeps the tail rule (D-212, D-213). Whether
oldest-first would also help on real files cannot be checked without them.

**Short days vary.** Ten-minute runs of the default config (six seeds, 60 s or 300 s warm-up): without the
filter all twelve were clean; with it, five of twelve had level-1 corrections, four of twelve less than
100% agreement, and all twelve had delete-size mismatches. A test that asserted 0 and 100% on a filtered
day would pass or fail with the seed.

### The mutation canaries on the fake days (synthetic)

Strict mode, the canary check of the closed loop, does not work as a detector here: the bug-free replay
also stops, at line 7 when comparing all 10 levels (the opening row's depth) and, on the filtered days,
at line 30,305 (SYNTHA) and 23,943 (SYNTHB) when comparing level 1 (the filter). The natural detector is
the dress-rehearsal check in resync mode (the ctest below), which the bug-free build passes.

| canary (synthetic) | SYNTHA: caught by, first line | SYNTHB: caught by, first line |
|---|---|---|
| `DIRECTION_IS_AGGRESSOR` | level-1 divergence and `direction_mismatch` (24,035 on the full day), line 4 | the same (54,886), line 7 |
| `APPLY_HIDDEN` | `hidden_exec` 0, 691 injected; from line 31, the first type 5 | `hidden_exec` 0, 746 injected; from line 1,646, the first type 5 (1,674 in variant (b)) |
| `NO_SEED` | level-1 correction at line 1 | level-1 correction at line 1 |

All three are caught on both days, in variant (b) and on the full day. `APPLY_HIDDEN` never changes the
book here: a hidden execution of the fake day is at a price between the best bid and ask with a random
direction, and none met a synthetic order of its side and price, so each became an unknown id (`unknown_id`
rises by exactly the injected count) and strict mode cannot see it; the counter check does. The audit
tool also fails on each canary's summary of SYNTHA's full day (the independent replay does not reproduce
it).

### The CI test (synthetic)

`lobster_dress_rehearsal` builds a short fake day in the build tree (`configs/default.json`, seed
20120621, 600 simulated seconds, 60 s warm-up) and asserts: without the filter, no correction above level
10, no removed level, every scored burst agrees at every trust depth, no direction, price or delete-size
mismatch and no oversized reduction, `hidden_exec` and `halt` equal to the injected counts, and
`unknown_id` equal to the audit's count of references to orders below level 10 at the window start (13 on
macOS); on the full day, the injected counts, no direction or price mismatch, and the audit reproducing
every number and explaining every discrepancy (393 level-1 items on macOS). It fails for each of the three
canaries.

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

At T = 1 every burst is excluded: the rule needs `T` of at least the burst's number of distinct prices plus
one, and a burst has at least one price, so T = 1 is always 0/0 and its row carries no information (it is
printed so the rule can be seen). Lobsim matches itself exactly because the data was produced by the same rules; a real
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
in these synthetic runs; whether L = 10 is deep enough for a real ticker is not known. Over a whole hour
it is not deep enough even for the simulator: the synthetic fake days of the
[dress rehearsal](#dress-rehearsal-on-synthetic-lobster-like-data-not-real-market-data) have thousands of
level-1 corrections at L = 10, and every one traces to a delete the filter dropped.

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
exits with code 2. On the hour-long synthetic fake days all three are caught as well, by the
dress-rehearsal check in resync mode rather than by strict mode (see
[the dress rehearsal](#the-mutation-canaries-on-the-fake-days-synthetic)).

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
  mismatches are expected and say little about the engine. Over a long day the touch is affected too:
  orders whose deletes were dropped while their price was outside the window stay in the replay and reach
  the touch when the price comes back. The synthetic fake days show thousands of level-1 corrections
  with filtering and none without, every one traced to a dropped delete, so on real data a level-1
  correction is not by itself a sign of an engine bug, whatever the replay's one-screen summary says
  (it prints the interpretation rule of the specification).
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
