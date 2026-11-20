# Fuzzing

lobsim is fuzzed with libFuzzer, which feeds a program huge numbers of generated inputs and keeps
the ones that reach new code (coverage-guided), then mutates those to go deeper. A rare path such
as a fill-or-kill order meeting self-trade prevention during a modify is found far faster this way
than by blind random generation. This file says what the three fuzzers check, how to build and run
them, what to do with a crash, and what CI runs.

## Contents

- [What the fuzzers check](#what-the-fuzzers-check)
- [The input format of `fuzz_diff`](#the-input-format-of-fuzz_diff)
- [The input format of `fuzz_diff_builder`](#the-input-format-of-fuzz_diff_builder)
- [Building](#building)
- [Running, replaying, minimizing and merging](#running-replaying-minimizing-and-merging)
- [When the fuzzer finds a crash](#when-the-fuzzer-finds-a-crash)
- [Seeds and the regression test](#seeds-and-the-regression-test)
- [What CI runs](#what-ci-runs)
- [The nightly corpus routine](#the-nightly-corpus-routine)
- [Measured runs](#measured-runs)
- [Injected-bug canaries](#injected-bug-canaries)

## What the fuzzers check

**`fuzz_diff`: differential fuzzing of the matching engine.** The input bytes are decoded into
requests (new order, cancel, modify). Each request is applied to a fresh `OrderBook` (the fast
engine) and a fresh `ReferenceBook` (a deliberately naive implementation of the same rules). After
every request the two event lists must be identical, field for field; `validate()` must pass on
both books (it is called explicitly, so this holds in every build type); and both books must report
the same depth on both sides, the same number of resting orders and, for a cancel or modify, the same
`order()` for the target id. Any difference prints a report to stderr (what differed, the request
index, the decoded request, both event lists) and calls `std::abort()`, which libFuzzer records as a
crash. An exception from either book, for example from the engine's own `validate()` calls in a
non-`NDEBUG` build, is caught, reported with the name of the book and the message, and aborts too.
A crash therefore means a bug in one of the two books.

**`fuzz_diff_builder`: differential fuzzing of the book-builder operations.** The same records as
`fuzz_diff`, with four more kinds: `insert_passive`, `reduce`, `remove` and `preview_match`, the
operations a feed replayer uses to build a book without matching (`include/lob/order_book.hpp`).
They run on a fresh `OrderBook` and a fresh `ReferenceBook`. Each operation's return value (the
`Status`, the new id of an insert, the whole `Fill` list of a preview) and the event buffers must be
identical; then the same checks as `fuzz_diff` run (`validate()` on both, depth, `resting_orders`,
`order()` of the target), plus `level_orders(side, price)` of both books, which compares the queue
order inside the level the operation touched. Any difference aborts with a report that names the
operation.

**`fuzz_replay_parse`: the `orders.log` parser.** The input is treated as the text of an
`orders.log`. The real parser (`sim::parse_order_log`) either accepts it or rejects it with
`sim::ReplayError`; that error is the only clean rejection. Any other exception (`bad_alloc`,
`length_error`, `logic_error`, ...), a sanitizer report or a hang is a crash. If the text parses, it is
formatted again with `sim::format_order_log` and parsed a second time: the second parse must not
throw, must equal the first, and must format to the same text. This is the double round trip: the
codec's own output must always be accepted and must not change.

**`fuzz_lobster_parse`: the LOBSTER line parsers.** The input is split on newlines and every line is
tried as a message line (`replay::parse_message_line`) and as an orderbook line
(`replay::parse_book_line`); a rejection is a clean result and nothing is printed. A message line that
parses is formatted with `replay::format_message_line` and parsed again: the second parse must succeed,
must equal the first and must not report a truncated time. An orderbook line is parsed with
`L` = columns / 4 when the column count is a positive multiple of 4, else 10, and a row that parses must
have `L` asks and `L` bids. A crash, a hang or a sanitizer report is a failure, as above.

All three fuzz builds are compiled with AddressSanitizer and UndefinedBehaviorSanitizer and
`-fno-sanitize-recover=all`, so a memory error or undefined behaviour in the engine or the parser
is a crash as well. The engine and the parsers are instrumented, not only the harness, otherwise
libFuzzer would get no coverage feedback from them.

## The input format of `fuzz_diff`

Input bytes are read as consecutive 8-byte records; trailing bytes (fewer than 8) are ignored. Each
record is one request. `b0` to `b7` are unsigned bytes.

| Byte | Meaning |
|---|---|
| `b0` | `kind = b0 % 3`: 0 = New, 1 = Cancel, 2 = Modify |
| `b1` | `owner = 1 + (b1 % 3)` (only 3 owners, so self-trades happen often) |
| `b2` | flags: bit0 side (0 Buy, 1 Sell); bit1 `post_only`; bit2 STP (0 CancelTaking, 1 CancelResting); bits3-4 order type: 0 Limit/GTC, 1 Limit/IOC, 2 Limit/FOK, 3 Market/IOC; bit5 `raw_price`; bit6 `raw_qty`; bit7 `bad_tif` |
| `b3`, `b4` | `p16 = (int16_t)(b3 \| (b4 << 8))`. Price = `raw_price ? p16 : 95 + ((uint16_t)p16 % 11)` (normal band 95-105) |
| `b5` | Qty = `raw_qty ? (int8_t)b5 : 1 + (b5 % 10)` (raw allows 0 and negatives) |
| `b6`, `b7` | `t = b6 \| (b7 << 8)`. Target id for Cancel/Modify: if `t % 8 == 7` or no ids are known yet, use `t` itself (probably unknown); otherwise `known_ids[t % known_ids.size()]` |

- `bad_tif`: if the type is Market, the time in force is set to GTC (invalid). Otherwise it is ignored.
- Other invalid combinations (post-only with IOC, FOK or Market) already arise from the flag bits.
- For Modify, `new_price` and `new_qty` come from the same price and quantity rules.
- `known_ids` are the `OrderId`s of every `Ack` emitted by the fast book so far, in order.
- `client_order_id` is the record's index.
- The books' `max_price` is 1000 while fuzzing, so raw prices of 0, negatives and above 1000 test the reject paths.

Worked example, two requests in 16 bytes:

```
00 00 00 64 00 05 00 00   New, owner 1, Buy,  Limit GTC, price 95 + (100 % 11) = 96, qty 1 + (5 % 10) = 6
00 01 01 64 00 03 00 00   New, owner 2, Sell, Limit GTC, price 96, qty 4: trades 4 @ 96 against the buy
```

Both books emit `Ack(1)`, `Ack(2)`, `Trade(price 96, qty 4, maker 1, taker 2)`, and the buy keeps 2
lots resting at 96. The `lobsim_fuzz_decode` tool prints any input in this readable form.

`fuzz_replay_parse` has no structure of its own: the input is the text of an `orders.log` (a header
line `# lobsim orders.log v1 max_price=<n>`, then one request per line; see `include/sim/order_log.hpp`).

## The input format of `fuzz_diff_builder`

Same 8-byte records and the same decoding of `b1` to `b7` as `fuzz_diff`; only the kind differs.
`fuzz_diff`'s own format and corpus are not affected.

| `kind = b0 % 7` | Operation |
|---|---|
| 0, 1, 2 | New, Cancel, Modify, decoded exactly as in `fuzz_diff` |
| 3 | `insert_passive(side = bit0 of b2, price, qty, owner)`; the returned id joins `known_ids` |
| 4 | `reduce(target id, qty)` |
| 5 | `remove(target id)` |
| 6 | `preview_match(taker side = bit0 of b2, limit = the price, qty)` |

`known_ids` holds the id of every Ack and of every successful `insert_passive`, in order, so a
target `t` picks `known_ids[t % known_ids.size()]` unless `t % 8 == 7`. Price and qty follow the
`raw_price` and `raw_qty` bits of `b2`, so zero, negative and out-of-range values reach the
validation paths (`InvalidQty`, `InvalidPrice`, `WouldCross`, `Oversized`, `UnknownOrder`).
`lobsim_fuzz_decode --builder <file>` prints such an input, with each operation's result.

## Building

The fuzz targets need Clang with libFuzzer (GCC does not support `-fsanitize=fuzzer`). They are built
in a build directory of their own, `build-fuzz/`, which configures only the `lob`, `orderlog` and `replay`
libraries, the drivers and the four fuzz executables: no tests, apps or benchmarks, so no instrumented
object is ever linked into a non-fuzzer executable. `LOBSIM_FUZZ` cannot be combined with
`LOBSIM_SANITIZE` (a fuzz build already includes ASan and UBSan), and CMake stops with a message if the
compiler is not LLVM Clang.

**Linux (Ubuntu).** Ubuntu ships the fuzzer runtime in its own package:

```sh
sudo apt-get install -y clang-18 llvm-18 libclang-rt-18-dev
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DLOBSIM_FUZZ=ON
cmake --build build-fuzz -j
```

**macOS.** Apple's Xcode Clang does not include libFuzzer (linking fails with
`libclang_rt.fuzzer_osx.a` not found), so use Homebrew LLVM:

```sh
brew install llvm
cmake -S . -B build-fuzz -DLOBSIM_FUZZ=ON \
      -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
      -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"
cmake --build build-fuzz -j
```

Plain Homebrew `clang++` linked the fuzzers without errors: no `lld` and no libc++ library-path flag
were needed (D-131). One exception so far: Apple's linker once rejected an instrumented object of the
`replay` library with `invalid r_symbolnum` (Homebrew LLVM 23, Apple ld 1167.5), which depended on the
size and shape of the translation unit; splitting `src/replay/lobster.cpp` into two files made it link
(D-158). If that error comes back, the documented fallback is Homebrew's linker: `brew install lld`,
then add `-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld` to the configure line (not needed so far, so not
yet tried here).

The executables are `build-fuzz/fuzz/fuzz_diff`, `build-fuzz/fuzz/fuzz_diff_builder`,
`build-fuzz/fuzz/fuzz_replay_parse` and `build-fuzz/fuzz/fuzz_lobster_parse`. The fuzz
build defaults to Debug, so `NDEBUG` is off and the engine's own `validate()` calls run on every
request. `lobsim_fuzz_decode` is built by the normal build (for example `build/debug/fuzz/lobsim_fuzz_decode`).

## Running, replaying, minimizing and merging

libFuzzer writes new inputs to the first corpus directory, so fuzz into a scratch directory and give
the committed seeds as the second one:

```sh
mkdir -p fuzz-work/diff fuzz-work/diff_builder fuzz-work/replay_parse fuzz-artifacts
./build-fuzz/fuzz/fuzz_diff fuzz-work/diff fuzz/corpus/diff \
  -max_len=4096 -max_total_time=300 -timeout=10 \
  -artifact_prefix=fuzz-artifacts/ -print_final_stats=1
./build-fuzz/fuzz/fuzz_diff_builder fuzz-work/diff_builder fuzz/corpus/diff_builder \
  -max_len=4096 -max_total_time=300 -timeout=10 \
  -artifact_prefix=fuzz-artifacts/ -print_final_stats=1
./build-fuzz/fuzz/fuzz_replay_parse fuzz-work/replay_parse fuzz/corpus/replay_parse \
  -max_len=4096 -max_total_time=300 -timeout=10 \
  -artifact_prefix=fuzz-artifacts/ -print_final_stats=1
```

`-max_len=4096` caps an input at 512 requests for `fuzz_diff`. A crash is saved as
`fuzz-artifacts/crash-<sha1>`. `fuzz-work/`, `fuzz-artifacts/` and `build-fuzz*/` are ignored by git.

Replay one crash without fuzzing (the binary takes a file as its argument):

```sh
./build-fuzz/fuzz/fuzz_diff fuzz-artifacts/crash-<sha1>
./build-fuzz/fuzz/fuzz_diff_builder fuzz-artifacts/crash-<sha1>
./build-fuzz/fuzz/fuzz_replay_parse fuzz-artifacts/crash-<sha1>
```

Shrink a crash:

```sh
./build-fuzz/fuzz/fuzz_diff -minimize_crash=1 -runs=10000 -exact_artifact_path=min.bin fuzz-artifacts/crash-<sha1>
```

Shrink the committed corpus while keeping its coverage (`-merge=1` copies into the first directory
only the inputs that add coverage), then replace it and regenerate the named seeds:

```sh
mkdir -p fuzz/corpus/diff_min
./build-fuzz/fuzz/fuzz_diff -merge=1 fuzz/corpus/diff_min fuzz/corpus/diff fuzz-work/diff
rm -rf fuzz/corpus/diff && mv fuzz/corpus/diff_min fuzz/corpus/diff
python3 fuzz/make_seeds.py
```

The same works for `fuzz_replay_parse` with `fuzz-work/replay_parse` and `fuzz/corpus/replay_parse`;
skip the `make_seeds.py` step there, since those seeds are not generated by a script (see
`fuzz/corpus/README.md`). Listing the old directory as a merge input keeps its seeds.

## When the fuzzer finds a crash

1. Read it. `lobsim_fuzz_decode <file>` from a normal build prints each request and the reference
   book's events (`--builder` for a `fuzz_diff_builder` input), then runs the differential check and prints the mismatch report, or `books agree`
   if the two books agree in that build (an injected-bug canary crash decodes this way: the bug
   exists only in the canary binary). `fuzz_replay_parse` crashes are plain text: open the file.
2. Shrink it with `-minimize_crash=1` as above.
3. Decide which side is wrong using the rules of `SPEC.md` (not by assuming the fast book is the one
   to change), and fix it. Nothing may be loosened to make a check pass.
4. Copy the minimized file to `fuzz/regressions/diff/<short-description>.bin` (`fuzz/regressions/diff_builder/` for
   `fuzz_diff_builder`; or
   `fuzz/regressions/replay_parse/<short-description>.log`).
5. Add a normal hand-written unit test for the same scenario.
6. Commit with a message that names the bug.

## Seeds and the regression test

The seed corpora `fuzz/corpus/diff/` and `fuzz/corpus/diff_builder/` (11 named scenarios for the
builder operations, ending in a 64-record mixed stream) are generated by `fuzz/make_seeds.py` (Python 3, standard library
only, deterministic): one file per scenario of the original unit tests plus a 64-request mixed stream,
named after the scenario (`fok_one_lot_short.bin`, `stp_cancel_resting.bin`, ...). Regenerate with
`python3 fuzz/make_seeds.py`. The ctest test `fuzz_seeds_up_to_date` runs
`make_seeds.py --check fuzz/corpus/diff --check-builder fuzz/corpus/diff_builder` and fails if a generated seed is missing or differs; extra
files (hash-named inputs from a merge) are ignored. `fuzz/corpus/README.md` also records how the
`replay_parse` seeds were made. `fuzz/corpus/lobster_parse/` is hand-written: 20 valid message lines, 20
valid orderbook lines (L = 10, 5 and 1, with empty levels) and a few malformed and CRLF lines.

`fuzz_regression_tests` runs every regular file of `fuzz/regressions/<target>/` and then of
`fuzz/corpus/<target>/` through the same driver function the fuzzer uses (`run_diff`,
`run_diff_builder`, `run_replay_parse`, `run_lobster_parse`), one test case per file, in any compiler and with no libFuzzer. The files are listed
when the test runs, so a new file needs neither a rebuild nor a reconfigure; for that reason the
tests are registered with plain `add_test` (`fuzz_regressions_diff`, `fuzz_regressions_diff_builder`, `fuzz_regressions_replay_parse`,
`fuzz_regressions_lobster_parse`) and not with `gtest_discover_tests`, whose list is fixed at build time and would silently skip a new
file. A failing file aborts the process; the file's path is printed first, so ctest's output names
it. Empty directories (a `.gitkeep` only) pass. Because a gtest filter that matches nothing also
exits 0, a further ctest entry, `fuzz_regressions_corpus_found`, fails unless the 19 named seeds of
`fuzz/corpus/diff`, the 11 of `fuzz/corpus/diff_builder`, the 4 seeds of `fuzz/corpus/replay_parse` and the 8 files of
`fuzz/corpus/lobster_parse` are found.

## What CI runs

`.github/workflows/ci.yml` runs on every push and pull request, on `ubuntu-24.04`:

- `build-test`: GCC 14 and Clang 18, Debug and Release, with `-DLOBSIM_WERROR=ON`, build and `ctest`
  (this includes the seeded differential test, the corpus regression tests and `fuzz_seeds_up_to_date`).
- `sanitize`: Clang 18 with `-DLOBSIM_SANITIZE=ON` and `ctest`.
- `fuzz-smoke`: the fuzz build with Clang 18, a check that `liblob.a`, `liborderlog.a` and `libreplay.a` contain
  `__sanitizer_cov` symbols (an engine without instrumentation would give the fuzzer no feedback),
  then `fuzz_diff`, `fuzz_diff_builder`, `fuzz_replay_parse` and `fuzz_lobster_parse` for 60 seconds each against the committed corpora. Crash
  files are uploaded on failure.
- `fuzz-canary`: one job per injected bug (below), with the same instrumentation check. The fuzzer
  runs for up to 300 seconds from the committed corpus, and then for up to 120 seconds from an empty
  corpus, so the result does not depend on a seed that triggers the bug by itself. Each run passes
  only if the fuzzer exits non-zero and a `crash-*` file exists: a fuzzer that stopped catching bugs
  fails CI.
- `coverage`: a Clang 18 source-based coverage build running every ctest test (so the regression
  tests and the seeds count), with the line coverage of `src/lob` in the job log and an HTML report as
  an artifact.

`.github/workflows/fuzz-nightly.yml` runs daily and on demand: `fuzz_diff` for 30 minutes and
`fuzz_replay_parse` for 10, from the committed corpora. It always uploads the grown scratch
directory as the artifact `corpus-<date>` and uploads the crash files on failure. It has read-only
permissions and never pushes. Scheduled workflows run only on the default branch.

## The nightly corpus routine

Merging a nightly corpus into the repository is a manual step:

1. Download the `corpus-<date>` artifact; it holds `diff/` and `replay_parse/`.
2. Merge it into the committed corpus through a minimized directory:

   ```sh
   mkdir -p fuzz/corpus/diff_min
   ./build-fuzz/fuzz/fuzz_diff -merge=1 fuzz/corpus/diff_min fuzz/corpus/diff <download>/diff
   rm -rf fuzz/corpus/diff && mv fuzz/corpus/diff_min fuzz/corpus/diff
   python3 fuzz/make_seeds.py
   ```

   and the same for `fuzz_replay_parse` with `fuzz/corpus/replay_parse` and `<download>/replay_parse`.
3. Run `ctest` (the regression test runs every merged file), then commit the corpus.

## Measured runs

Five-minute runs with the exact commands above, started from the committed seeds, on macOS with
Homebrew LLVM clang, Debug fuzz build. None found a crash.

| target | execs | exec/s | coverage (edges) | corpus |
|---|---|---|---|---|
| `fuzz_diff` (D-140) | 507,551 in 301 s | 1,686 (measured while other fuzz runs shared the CPU; a 60 s run alone did about 5,900) | 860 to 1302 | 19 to 1487 inputs |
| `fuzz_diff_builder` (D-157) | 505,298 in 301 s | 1,678 | 1237 to 2042 | 11 to 2009 inputs |
| `fuzz_replay_parse` (D-144) | 10,367,465 in 301 s | 34,443 | 313 to 493 | 3 to 199 inputs |
| `fuzz_lobster_parse` (D-167) | 2,363,409 in 301 s | 7,851 (CPU shared with other builds) | 345 to 423 | 8 to 460 inputs |

## Injected-bug canaries

The fuzzer is only worth running if it can be shown to catch bugs. Three compile-time switches in
`src/lob/order_book.cpp` (the `OrderBook` only; `ReferenceBook` is untouched) each inject one
matching bug. They are off by default, and a `#error` stops any build that defines one without
`LOBSIM_FUZZ_BUILD`. Select one with `-DLOBSIM_FUZZ=ON -DLOBSIM_INJECT_BUG=<macro>`; CMake rejects an
unknown name, and a name without `LOBSIM_FUZZ`, at configure time.

| macro | what it breaks | seeded (corpus `fuzz/corpus/diff`) | unseeded (empty corpus) |
|---|---|---|---|
| `LOBSIM_BUG_TAKER_PRICE` | a limit taker's trades print at its own limit price instead of the maker's | caught, under 1 s (wall clock resolution 1 s), while loading the corpus, 3rd input run: `trade_at_maker_price.bin` | caught, under 1 s, 3018 execs |
| `LOBSIM_BUG_MODIFY_KEEPS_PRIORITY` | a same-price quantity increase keeps time priority (`priority_kept=true`, order not requeued) | caught, under 1 s, while loading the corpus, 17th input run: `modify_increase_loses_priority.bin` | caught, under 1 s, 3028 execs |
| `LOBSIM_BUG_FOK_IGNORES_STP` | the fill-or-kill dry run counts the taker's own orders under `CancelTaking`, so the order is not refused up front | caught, under 1 s, while loading the corpus, 10th input run: `fok_stp_cancel_taking.bin` | caught, under 1 s, 2655 execs |

Times are measured with `date +%s` around a run of `fuzz_diff <scratch dir> <corpus dir>
-max_len=4096 -max_total_time=600 -timeout=10 -artifact_prefix=... -print_final_stats=1` on macOS
(Homebrew LLVM clang++, Debug, arm64), with a limit of 600 s. "Execs" is libFuzzer's
`number_of_executed_units` for the run, including the crashing one. In every seeded run the crash
came before libFuzzer's `INITED` line: a seed that exercises the scenario by itself is enough, no
mutation is involved. The unseeded runs start from nothing and reach the same mismatches by
mutation within a few thousand executions.

Every crashing input decodes with `lobsim_fuzz_decode` from a normal build and ends with
`books agree`: the bug exists only in the canary binary.
