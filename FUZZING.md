# Fuzzing

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
