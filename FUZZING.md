# Fuzzing

lobsim is fuzzed with libFuzzer, which feeds a program huge numbers of generated inputs and keeps
the ones that reach new code (coverage-guided), then mutates those to go deeper. A rare path such
as a fill-or-kill order meeting self-trade prevention during a modify is found far faster this way
than by blind random generation. This file says what the two fuzzers check, how to build and run
them, what to do with a crash, and what CI runs.

## Contents

- [What the fuzzers check](#what-the-fuzzers-check)
- [The input format of `fuzz_diff`](#the-input-format-of-fuzz_diff)
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

**`fuzz_replay_parse`: the `orders.log` parser.** The input is treated as the text of an
`orders.log`. The real parser (`sim::parse_order_log`) either accepts it or rejects it with
`sim::ReplayError`; that error is the only clean rejection. Any other exception (`bad_alloc`,
`length_error`, `logic_error`, ...), a sanitizer report or a hang is a crash. If the text parses, it is
formatted again with `sim::format_order_log` and parsed a second time: the second parse must not
throw, must equal the first, and must format to the same text. This is the double round trip: the
codec's own output must always be accepted and must not change.

Both fuzz builds are compiled with AddressSanitizer and UndefinedBehaviorSanitizer and
`-fno-sanitize-recover=all`, so a memory error or undefined behaviour in the engine or the parser
is a crash as well. The engine and the parser are instrumented, not only the harness, otherwise
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

## Building

The fuzz targets need Clang with libFuzzer (GCC does not support `-fsanitize=fuzzer`). They are built
in a build directory of their own, `build-fuzz/`, which configures only the `lob` and `orderlog`
libraries, the drivers and the two fuzz executables: no tests, apps or benchmarks, so no instrumented
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

Plain Homebrew `clang++` linked both fuzzers without errors: no `lld` and no libc++ library-path flag
were needed (D-131).

The executables are `build-fuzz/fuzz/fuzz_diff` and `build-fuzz/fuzz/fuzz_replay_parse`. The fuzz
build defaults to Debug, so `NDEBUG` is off and the engine's own `validate()` calls run on every
request. `lobsim_fuzz_decode` is built by the normal build (for example `build/debug/fuzz/lobsim_fuzz_decode`).

## Running, replaying, minimizing and merging

libFuzzer writes new inputs to the first corpus directory, so fuzz into a scratch directory and give
the committed seeds as the second one:

```sh
mkdir -p fuzz-work/diff fuzz-work/replay_parse fuzz-artifacts
./build-fuzz/fuzz/fuzz_diff fuzz-work/diff fuzz/corpus/diff \
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
   book's events, then runs the differential check and prints the mismatch report, or `books agree`
   if the two books agree in that build (an injected-bug canary crash decodes this way: the bug
   exists only in the canary binary). `fuzz_replay_parse` crashes are plain text: open the file.
2. Shrink it with `-minimize_crash=1` as above.
3. Decide which side is wrong using the rules of `SPEC.md` (not by assuming the fast book is the one
   to change), and fix it. Nothing may be loosened to make a check pass.
4. Copy the minimized file to `fuzz/regressions/diff/<short-description>.bin` (or
   `fuzz/regressions/replay_parse/<short-description>.log`).
5. Add a normal hand-written unit test for the same scenario.
6. Commit with a message that names the bug.

## Seeds and the regression test

The seed corpus `fuzz/corpus/diff/` is generated by `fuzz/make_seeds.py` (Python 3, standard library
only, deterministic): one file per scenario of the original unit tests plus a 64-request mixed stream,
named after the scenario (`fok_one_lot_short.bin`, `stp_cancel_resting.bin`, ...). Regenerate with
`python3 fuzz/make_seeds.py`. The ctest test `fuzz_seeds_up_to_date` runs
`make_seeds.py --check fuzz/corpus/diff` and fails if a generated seed is missing or differs; extra
files (hash-named inputs from a merge) are ignored. `fuzz/corpus/README.md` also records how the
`replay_parse` seeds were made.

`fuzz_regression_tests` runs every regular file of `fuzz/regressions/<target>/` and then of
`fuzz/corpus/<target>/` through the same driver function the fuzzer uses (`run_diff`,
`run_replay_parse`), one test case per file, in any compiler and with no libFuzzer. The files are listed
when the test runs, so a new file needs neither a rebuild nor a reconfigure; for that reason the
tests are registered with plain `add_test` (`fuzz_regressions_diff`, `fuzz_regressions_replay_parse`)
and not with `gtest_discover_tests`, whose list is fixed at build time and would silently skip a new
file. A failing file aborts the process; the file's path is printed first, so ctest's output names
it. Empty directories (a `.gitkeep` only) pass.

## What CI runs

`.github/workflows/ci.yml` runs on every push and pull request, on `ubuntu-24.04`:

- `build-test`: GCC 14 and Clang 18, Debug and Release, with `-DLOBSIM_WERROR=ON`, build and `ctest`
  (this includes the seeded differential test, the corpus regression tests and `fuzz_seeds_up_to_date`).
- `sanitize`: Clang 18 with `-DLOBSIM_SANITIZE=ON` and `ctest`.
- `fuzz-smoke`: the fuzz build with Clang 18, then `fuzz_diff` and `fuzz_replay_parse` for 60 seconds
  each against the committed corpora. Crash files are uploaded on failure.
- `fuzz-canary`: one job per injected bug (below). The fuzzer runs for up to 300 seconds, and the job
  passes only if it exits non-zero and a `fuzz-artifacts/crash-*` file exists: a fuzzer that stopped
  catching bugs fails CI.
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
Homebrew LLVM clang, Debug fuzz build. Neither found a crash.

| target | execs | exec/s | coverage (edges) | corpus |
|---|---|---|---|---|
| `fuzz_diff` (D-140) | 507,551 in 301 s | 1,686 (measured while other fuzz runs shared the CPU; a 60 s run alone did about 5,900) | 860 to 1302 | 19 to 1487 inputs |
| `fuzz_replay_parse` (D-144) | 10,367,465 in 301 s | 34,443 | 313 to 493 | 3 to 199 inputs |

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
