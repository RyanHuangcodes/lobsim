# The lobsim course

This course teaches lobsim from zero. You do not need to know anything about markets, trading or C++ before you start. Each module explains one idea with everyday examples, then opens the real code that does it, then gives you something to build, run or break with your own hands.

If you finish it, you will be able to explain how lobsim works to someone else, change it safely, and read its results honestly.

## How to use this course

Do the modules in order. Each one is built on the ones before it.

Every module has the same parts:

| Part | What it is |
|---|---|
| **Lecture** | The idea, explained with a story first and the precise version second. |
| **Read the real code** | The important parts of lobsim's code, with the file and line where they live. Code that repeats a pattern you have already seen is shown once and the rest is listed under **Skipped on purpose**, so you know it exists. |
| **Predict, then run** | Before you run something, you write down what you think will happen. Being wrong is the useful part: it shows you exactly what you had not understood yet. |
| **Labs** | Hands-on work. The commands and programs were run while this course was written, and the expected output is shown so you can check yourself. The few steps that could not be run are marked as such (see [labs/README.md](labs/README.md)). |
| **Common questions** | The questions people usually ask at that point, with answers. |
| **Quiz** | Short questions. The answers are folded away: click **Answer** to open one, but only after you have written yours down. |

Keep a notebook (paper or a text file). Write your predictions, your answers and your questions in it. A good habit from the start: when something surprises you, write down what you expected and what happened.

## Before you start: build lobsim once

You need a computer with Linux or macOS, a C++20 compiler (GCC 11+ or Clang 14+), CMake 3.21+, Python 3 and an internet connection the first time (the build downloads three libraries). From the folder where lobsim lives:

```sh
cmake --preset release
cmake --build --preset release -j
ctest --preset release
```

The last command runs more than 650 tests. When this course was written it printed `100% tests passed, 0 tests failed out of 670` in about 20 seconds. Module 0 explains what you just did.

The C++ labs of the course are small programs that use lobsim's libraries. Build any one of them with:

```sh
course/labs/build_lab.sh course/labs/lab02_book_repl.cpp     # writes build/release/labs/lab02_book_repl
```

The Python labs need only Python 3 itself (no extra packages). They are in `course/labs/py/`. See [labs/README.md](labs/README.md) for the full list.

## The modules

| # | Module | You will learn | Main code |
|---|---|---|---|
| 0 | [The big picture](00-big-picture.md) | What lobsim is, its parts, and how one order travels through it | the whole repo |
| 1 | [Markets 101](01-markets-101.md) | Buyers, sellers, the order book, bid, ask, spread, mid, ticks and lots | `include/lob/types.hpp` |
| 2 | [The rules of matching](02-matching-rules.md) | Order types, price-time priority, maker and taker, modify, self-trade prevention, events | `include/lob/events.hpp`, `SPEC.md` 3 |
| 3 | [Your C++ toolkit](03-cpp-toolkit.md) | Compiling, types, structs, enums, variant, optional, headers, CMake | `include/lob/*.hpp`, `CMakeLists.txt` |
| 4 | [The simple book](04-reference-book.md) | The slow, obviously correct matching engine, line by line | `src/lob/reference_book.cpp` |
| 5 | [The fast book](05-fast-order-book.md) | Pointers, linked lists, sorted maps, memory pools, Big-O, invariants | `src/lob/order_book.cpp`, `include/lob/pool.hpp` |
| 6 | [How we know it is right](06-testing.md) | Unit tests, typed tests, conservation, differential testing, fuzzing, canaries, sanitizers | `tests/`, `fuzz/` |
| 7 | [Randomness you can repeat](07-randomness.md) | Random number generators, seeds, distributions, Poisson processes, determinism | `include/sim/rng.hpp` |
| 8 | [The simulation clock](08-simulation-kernel.md) | Discrete-event simulation, the event queue, latency, the exchange, the kernel loop | `include/sim/*.hpp`, `src/sim/simulator.cpp` |
| 9 | [The traders](09-traders.md) | Agents, zero-intelligence noise traders, the hidden "true value", the informed trader | `src/sim/noise_trader.cpp`, `informed_trader.cpp` |
| 10 | [The market maker](10-market-maker.md) | Inventory risk, the Avellaneda–Stoikov formulas, and how the code survives races | `src/sim/as_market_maker.cpp` |
| 11 | [Keeping score](11-measuring-results.md) | PnL, markouts, adverse selection, confidence intervals, returns, fat tails, autocorrelation | `src/sim/metrics.cpp` |
| 12 | [Replay and speed](12-replay-and-speed.md) | The order log, replaying a run, benchmarks, measuring time honestly | `src/sim/order_log.cpp`, `apps/bench_main.cpp` |
| 13 | [Real market data](13-real-data-lobster.md) | The LOBSTER files, rebuilding a real book, checking the matching rule against data | `src/replay/` |
| 14 | [Calibration](14-calibration.md) | Measuring order flow from data, survival curves, a calibrated trader, testing on unseen data | `src/calib/`, `calibrated_flow.cpp` |
| 15 | [Dashboard, CI and good habits](15-dashboard-ci-and-habits.md) | The replay dashboard, GitHub Actions, and the project's working rules | `viz/`, `web/`, `.github/` |
| 16 | [Capstone projects](16-capstone.md) | Projects that use everything | your own code |

[glossary.md](glossary.md) explains every special word used in the course, in one or two sentences each.

## How long it takes

About 40 to 60 hours for someone starting from zero, spread over several weeks. Modules 1, 2, 9 and 10 are mostly about markets; modules 3, 4, 5, 6, 7, 8 and 12 are mostly about programming; modules 11, 13 and 14 mix statistics with both. Modules 13 and 14 cover the newest and most advanced parts of lobsim and can wait until the rest feels comfortable.

## A note for whoever is guiding the learner

- One module per sitting is a good pace for modules 0 to 2; the code modules (4, 5, 8, 10) are worth two sittings each.
- Ask the learner to explain the lab results back to you in their own words. If they can predict an output before running it, they understand it.
- The quiz answers and the "expected output" blocks are there for checking, not for copying. Cover them first.
- The project's own documents are the next step after this course: `README.md` (results), `SPEC.md` (the rules lobsim was built to), `DECISIONS.md` (every choice the spec left open, with its reason), `FUZZING.md`, `REAL_DATA.md` and `CALIBRATION.md`.
