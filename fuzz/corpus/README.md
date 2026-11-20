# Fuzz corpus

`diff/` holds the seed inputs of the differential fuzzer (format: `fuzz/diff_driver.hpp`, one 8-byte record per request).

The named seeds (`worked_example.bin`, `fok_one_lot_short.bin`, ...) are generated, not written by hand:

    python3 fuzz/make_seeds.py

There is one file per scenario of the original unit tests, plus a 64-request mixed stream. The script is deterministic, so the output is byte-for-byte the same on every run; the ctest test `fuzz_seeds_up_to_date` fails if a committed seed differs from it or is missing. To read a seed as requests and events: `lobsim_fuzz_decode fuzz/corpus/diff/<name>.bin`.

Fuzzing writes the inputs it finds into a scratch directory, not here (see FUZZING.md). Corpus files with hash names, from merging a scratch directory back in, may also live in `diff/`; the regression test runs every file in the directory.

## replay_parse/

Seeds for `fuzz_replay_parse`, whose input is the text of an `orders.log`. `empty.log` is an empty file; `run_seed1.log` to `run_seed3.log` are the first lines of the `orders.log` of a short simulator run (a prefix cut at a line boundary is a valid log), each under 4 KB and together containing N, C and M lines. Made from the repository root, after building `lobsim_run`, with `$OUT` a scratch directory:

    for s in 1 2 3; do
      build/debug/lobsim_run --config configs/default.json --seed $s --duration-s 10 --out $OUT/run$s
      head -c 4000 $OUT/run$s/orders.log | sed '$d' > fuzz/corpus/replay_parse/run_seed$s.log
    done
    : > fuzz/corpus/replay_parse/empty.log

`sed '$d'` drops the last line, which `head -c` may have cut in the middle.

## lobster_parse/

Hand-written seeds for `fuzz_lobster_parse`, whose input is lines of LOBSTER text, each tried as a message line and as an orderbook line. `message_a.csv` and `message_b.csv` hold 20 valid message lines (types 1 to 7, fractions of 0, 1, 3 and 9 digits); `book_l10.csv`, `book_l10_empty_levels.csv`, `book_l5.csv` and `book_l1.csv` hold 20 valid orderbook lines (L = 10, 5 and 1; empty levels use 9999999999 / -9999999999 with size 0). `malformed.csv` has wrong column counts, a 12-digit fraction, direction 0 on type 7 and type 4, a negative size, a non-monotone book row and an overflowing time; `crlf.csv` has CRLF line ends.
