# The labs

Hands-on material for the course. Every program here uses lobsim's own libraries, so what you see is what the real engine and simulator do.

## Before the labs

Build lobsim once (from the repository root):

```sh
cmake --preset release
cmake --build --preset release -j
```

Run every command below from the **repository root** (the folder that holds `CMakeLists.txt`). Several labs read `configs/default.json` by that path, and most read the outputs of the default run, made in Module 0:

```sh
build/release/lobsim_run --config configs/default.json --out out/run1
```

`out/` and `build/` are ignored by git, so nothing you generate there is committed.

## C++ labs

Build one with `course/labs/build_lab.sh <file>`; the program is written to `build/release/labs/<name>`. The script compiles the lab with the same include folders and libraries as lobsim itself. Set `CXX` to choose the compiler (for example `CXX=clang++`).

| File | Module | What it does |
|---|---|---|
| `lab02_book_repl.cpp` | 1, 2, 4, 12 | The order book playground: type orders, see the engine's events and the book. Reads commands from the keyboard or from a script (`< scripts/...`). `using Book = ...` switches the engine (Lab 4.3). |
| `lab03_cpp_tour.cpp` | 3 | Eight short demonstrations of the C++ that lobsim uses, with lobsim's own types. |
| `lab05_big_o.cpp` | 5 | Times both books as the book grows from 100 to 10,000 resting orders. |
| `lab07_rng.cpp` | 7 | Prints lobsim's random numbers for seed 42 (compare with `py/xoshiro.py`), plus the argument-order puzzle. |
| `lab08_event_queue.cpp` | 8 | The event queue's tie-break rule and a FIFO latency channel. |
| `lab08_trace_log.cpp` | 8, 10, 12 | Replays the first N requests of an `orders.log` and prints each one's events. |
| `lab09_my_agent.cpp` | 9 | "Copycat", a complete custom agent added to a ten-minute run. Change it. |
| `capstone_observer.cpp` | 16 | Starter for capstone A: your own observer, with a check that it does not change the run. |

## Scripts for the playground

| File | Module | What it shows |
|---|---|---|
| `scripts/worked_example.txt` | 2 | The worked example of SPEC.md section 1 |
| `scripts/order_types.txt` | 2 | IOC, FOK, market, post-only and invalid orders |
| `scripts/modify_and_stp.txt` | 2 | Modify keeping and losing priority, cancel by the wrong owner, both self-trade-prevention modes |

Run one with `build/release/labs/lab02_book_repl < course/labs/scripts/order_types.txt`.

## Python labs (standard library only)

| File | Module | What it does |
|---|---|---|
| `py/xoshiro.py` | 7 | lobsim's random number generator rewritten in Python; prints the same numbers as `lab07_rng`. |
| `py/modulo_bias.py` | 7 | Counts every outcome of `r % n` with and without rejection sampling. |
| `py/poisson.py` | 7 | Simulates a noise trader's wakeups and compares the counts with the Poisson formula. |
| `py/same_decisions.py` | 7 | Shows that removing the informed trader leaves every noise trader's decisions unchanged. |
| `py/who_traded.py` | 9 | Checks the noise traders' rules against `orders.log`, and gives every role's PnL. |
| `py/as_quotes.py` | 10 | The Avellaneda–Stoikov quotes by hand; reproduces the market maker's first quotes. |
| `py/mm_inventory.py` | 10 | The market maker's inventory over the hour, window by window. |
| `py/mm_pnl.py` | 11 | Recomputes the market maker's cash, inventory, fees and PnL from `trades.csv`. |
| `py/markouts.py` | 11 | Recomputes markouts and their decomposition, and splits them by counterparty. |
| `py/returns_stats.py` | 11 | Recomputes log returns, excess kurtosis and autocorrelations from `bbo.csv`. |
| `py/fnv1a.py` | 12 | The engine's event hash by hand. |
| `py/kaplan_meier.py` | 14 | The Kaplan–Meier survival curve, with and without censoring. |
| `py/distances.py` | 14 | Total variation and Wasserstein-1 distances between distributions. |

## Configs

| File | What it changes from `configs/default.json` |
|---|---|
| `configs/noise_only_floor.json` | no informed trader, no market maker, `reference_price` "floor" |
| `configs/noise_only_symmetric.json` | no informed trader, no market maker, `reference_price` "symmetric" |
| `configs/no_informed.json` | no informed trader |
| `configs/strong_informed.json` | informed trader `lambda` 10 and `size` 20 |

## Patches (deliberate bugs, for Modules 5 and 6)

| File | The bug | Who catches it |
|---|---|---|
| `patches/lab05_use_after_release.patch` | `match_in` reads a maker's `next` after releasing it | Only AddressSanitizer: the normal tests still pass |
| `patches/lab06_taker_price_bug.patch` | trades print at the taker's limit | 15 unit tests and the differential test |

Apply with `git apply <patch>`, undo with `git apply -R <patch>`, and rebuild after each. `git diff --stat` printing nothing means the source is clean again.

## What was checked when the course was written

Every command above was run on Linux (GCC 11.5, Clang 15, CMake 3.22, Python 3.11), and the expected outputs quoted in the modules are copies of what it printed. Timings and benchmark numbers depend on the machine. Three exceptions: the libFuzzer steps of Lab 6.5 were not run (no libFuzzer runtime was available); the sanitize build of Lab 5.3 was run, but that machine lacked GCC's sanitizer runtime (`libasan`, `libubsan`), which had to be added separately; and the dashboard of Lab 15.1 needs a browser, so only its exporter was run.
