# Fuzz corpus

`diff/` holds the seed inputs of the differential fuzzer (format: `fuzz/diff_driver.hpp`, one 8-byte record per request).

The named seeds (`worked_example.bin`, `fok_one_lot_short.bin`, ...) are generated, not written by hand:

    python3 fuzz/make_seeds.py

There is one file per scenario of the original unit tests, plus a 64-request mixed stream. The script is deterministic, so the output is byte-for-byte the same on every run; the ctest test `fuzz_seeds_up_to_date` fails if a committed seed differs from it or is missing. To read a seed as requests and events: `lobsim_fuzz_decode fuzz/corpus/diff/<name>.bin`.

Fuzzing writes the inputs it finds into a scratch directory, not here (see FUZZING.md). Corpus files with hash names, from merging a scratch directory back in, may also live in `diff/`; the regression test runs every file in the directory.
