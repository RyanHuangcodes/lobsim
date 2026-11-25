# Module 16: Capstone projects

> **In one sentence:** pick one or more projects that make you use the engine, the simulator, the statistics and the testing habits together, and finish each one to the standard lobsim holds itself to.
>
> **Time:** each project takes from an afternoon to a couple of weeks. Projects A and E are the best first choices.

Every project has the same finishing rules, borrowed from the project itself:

1. **It is repeatable.** Someone else can run your command with your seed and get your numbers.
2. **It does not break anything.** `ctest --preset release` still passes. If you changed engine code, the differential test (`fuzz_diff`) still passes.
3. **It is measured, not guessed.** Every claim in your write-up comes with a number and says where the number came from, and results that rest on one seed say so.
4. **It is honest.** Say what your result does not show.

## Project A: measure something new (starter provided)

Write an `Observer` (Module 8.7) that measures something about the market that `summary.json` does not report, and prove it does not change the run.

The starter `course/labs/capstone_observer.cpp` runs ten minutes twice, without and with an example observer, and compares the event hashes. When this course was written:

```
event hash without the observer bca29f3b54619193, with it bca29f3b54619193: identical, good
top of book changed 10243 times
average time between changes 58.573 ms, longest 0.608 s
```

Ideas for your own measurement, from easier to harder:

- How long the market maker's orders rest before their first fill (use `on_engine_event`: an `Ack` for owner 12 starts a clock, its first `Trade` stops it).
- The distribution of trade sizes, split by the taker's role.
- How often the informed trader's market orders walk through more than one price level.
- "Queue position": when a noise trader's limit order joins a level, how many lots are ahead of it? (You will need to track the true book's levels from the events, since observers only receive the top of book.)

**Done when:** the hashes are identical, your numbers are explained in a paragraph, and you have checked at least one of them by hand on a small case (for example with the playground of Module 2).

## Project B: an informed trader that reacts to the size of the mispricing

The README lists this as lobsim's next milestone. Today's informed trader sends 5 lots whenever the gap between the true value and the price is at least 2 ticks, whatever the gap's size. Write one whose size grows with the gap, for example `size = round(k × (gap − θ))`, capped.

Hints:

- Start from `src/sim/informed_trader.cpp` and copy it into a lab file as a new agent class, like Copycat in `course/labs/lab09_my_agent.cpp`. Its constructor needs the fundamental value: `simulator.fundamental()` returns it, and it must outlive the agent (it does, since the simulator owns it).
- Remove the configured informed trader (use `course/labs/configs/no_informed.json`) and add yours with `Simulator::add_agent(..., sim::StreamRole::Test, 0)`.
- Draw nothing from `ctx.rng()` outside `on_start` and `on_wakeup` (Module 7.6).
- To get the usual output files, attach a `sim::MetricsObserver` and write them as `apps/run_main.cpp` does (`write_trades_csv`, `write_bbo_csv`, `write_mm_fills_csv`, `summary_json`), and pass an `std::ofstream` for `orders.log` in `SimulatorOptions`. Your agent's fills count as role "other" in `summary.json`; split them out with its owner id.

Measure: the market maker's PnL per lot, its 30 s markout against your trader's fills (adapt `course/labs/py/markouts.py`: your trader's owner id is printed by the program), and how closely the mid now follows the fundamental. Compare with the default informed trader over several seeds.

**Done when:** you have a table over at least 5 seeds, with the range, and a paragraph explaining what changed and why.

## Project C: your own market maker

Write a simple market maker as a new agent: always quote the mid ± 1 tick, post-only, 5 lots, no inventory lean. Then compare it with the Avellaneda–Stoikov market maker on PnL, PnL per lot, largest inventory and quoting time.

This is harder than it sounds, because your agent must follow the same discipline as the real one (Module 10.4): at most one request in flight per side, every reply matched to its request, fills that are not replies, `UnknownOrder` rejects after unseen fills. Count anything unexpected in an "anomalies" counter like the real one does, and make it 0.

**Done when:** your agent runs a full hour with 0 anomalies, its inventory never exceeds a limit you set and can justify, and your comparison table covers several seeds.

## Project D (advanced): an array-indexed price ladder

SPEC.md and the README list this stretch goal: replace `std::map` price levels with an array indexed by price (a "ladder"), so that finding a level is O(1) instead of O(log L).

- Write a third book class with exactly the same public functions as `OrderBook`.
- Add it to `BookTypes` in `tests/unit/book_types.hpp`, so every typed unit test runs against it too.
- Write a differential driver like `fuzz/diff_driver.cpp`'s `run_diff`, comparing your book with `ReferenceBook`, and run it on many seeds. (If you have LLVM Clang with libFuzzer, fuzz it.)
- Benchmark it with a copy of `bench/micro_bench.cpp` and compare with the README's numbers, measured on your own machine. Explain how you find the best price quickly when the array is mostly empty.

**Done when:** all typed tests pass for your book, your differential run agrees on at least 200 seeds × 10,000 messages, the sanitize build is clean, and your benchmark table states the machine, the compiler and what was measured.

## Project E: an honest report of one run

Pick a seed of your own and a config change you find interesting (for example the noise traders' `D` or the market maker's `gamma`). Produce a two-page report in the style of the README's "Results of a sample run":

- the market maker's results, with markouts and their decomposition (Lab 11.2);
- the spread, the stylized facts (Lab 11.3), and a plot or two (Lab 11.4);
- a comparison with the default config over at least 5 seeds;
- a section "an honest reading" and a section "what this does not show".

**Done when:** someone else can reproduce every number from the commands in your report.

## Project F: teach it back

Choose one module and teach it to someone who has not seen lobsim, in 15 minutes, using one lab as the hands-on part. Afterwards, write down the two questions they asked that you could not answer at once, and find the answers in the code or in `DECISIONS.md`.

**Done when:** your listener can do the module's quiz without help.

## Where to go next

- The project's own documents, in this order: `README.md`, `SPEC.md`, `DECISIONS.md`, `FUZZING.md`, `REAL_DATA.md`, `CALIBRATION.md`, `DASHBOARD.md`.
- The papers SPEC.md and CALIBRATION.md cite: Avellaneda and Stoikov (2008) on market making; Gode and Sunder (1993) and Smith, Farmer et al. on zero-intelligence markets; Farmer, Patelli and Zovko on the predictive power of zero intelligence; Cont (2001) on stylized facts; Platt and Gebbie on calibrating agent-based models; Kaplan and Meier (1958) on survival curves.
- Real data: REAL_DATA.md explains how to obtain LOBSTER files and the one command that replays and calibrates on them.
