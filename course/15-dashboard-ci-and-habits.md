# Module 15: Dashboard, CI and good habits

> **In one sentence:** a project becomes trustworthy when its results can be seen, its checks run automatically on every change, and every decision is written down with its reason.
>
> **You will be able to:** export a run for the replay dashboard and view it, explain why the exporter re-verifies a run before drawing it, describe the bundle format, read lobsim's GitHub Actions workflows, and name the working habits that run through the whole project, with an example of each.
>
> **Time:** about 2 hours.

## 15.1 The replay dashboard

Numbers in a JSON file are hard to feel. The dashboard ([live demo](https://ryanhuangcodes.github.io/lobsim/?run=demo), DASHBOARD.md) replays a simulated hour like a video, with one shared timeline for every panel:

- a **heatmap** of the order book: time left to right, price top to bottom, colour = resting size (darker blue, more lots), with the best bid, best ask and trades drawn on top;
- a **depth ladder** at the cursor: each price level's bid and ask size, with the market maker's own quotes labelled `MM bid` / `MM ask` in text (colour is never the only cue);
- a **price chart**: mid, fundamental, the MM's quotes and the trades;
- the MM's **inventory** and **PnL** over time.

Play, pause, scrub, change speed (1× to 600×), zoom by dragging, and switch any panel to a table of the numbers around the cursor.

### Two programs, one contract

1. `lobsim_viz_export` (`viz/viz_core.cpp`, C++) turns a run folder into a **bundle**: `meta.json` (a small description of the run and of the data layout) and `data.bin` (the numbers).
2. The web page (`web/`, plain HTML, CSS and JavaScript modules, no build step, one vendored chart library, uPlot) reads the bundle and draws it.

`viz/BUNDLE.md` documents the format, and says: "If `viz_core.cpp` and this document ever disagree, `viz_core.cpp` wins and this document is the bug."

**Verify before you draw.** The exporter does not trust the run's files. It rebuilds the book from `orders.log` with the real engine, recomputes the engine event hash and the market maker's PnL, and compares them with `summary.json`. If either differs, it writes **nothing** and exits with code 2. A picture of a run that cannot be reproduced would be worse than no picture.

**The binary format, briefly.** `data.bin` is a list of typed columns (arrays of 32-bit integers, 64-bit floats, bytes), little-endian and 8-byte aligned, each described in `meta.json`'s `series` table. Prices are stored as 32-bit integers; a value that does not fit is an error, never a silent wrap. Missing values use a sentinel (`INT32_MIN`). Each heatmap cell is one byte on a **log scale** (`encode_log_u8`: 0 stays 0, the largest size maps to 255), because order sizes range from 1 lot to hundreds and a straight scale would show only the biggest levels. A heatmap column covers a time bucket and shows the **time-weighted** mean size over that bucket (the same idea as Module 11.5), so a level that was big only for an instant does not look like a wall.

**A size budget.** The exporter aims at about 8 MB per bundle (`--budget-mb`). If the run would be larger, it widens the sample periods to fit, working the size out by arithmetic before sampling anything (a security and performance review found that one far-off timestamp could otherwise make it loop without bound: DECISIONS.md D-909).

**Skipped on purpose:** the JavaScript (`web/js/bundle.js` decodes the bundle, `heatmap.js` draws the heatmap once per zoom level into an off-screen canvas, `app.js` runs the cursor, playback and keyboard), and the dashboard's own tests (`viz/viz_test.cpp` and the node tests in `web/test/`, run by ctest).

## 15.2 Automatic checks: CI

**Continuous integration** (CI) means that every change pushed to the shared repository is built and tested automatically by machines, so nobody depends on remembering to run the tests. lobsim uses GitHub Actions; the instructions are YAML files in `.github/workflows/`.

`ci.yml` runs on every push and pull request (README, FUZZING.md):

| Job | What it does |
|---|---|
| `build-test` | GCC 14 and Clang 18, Debug and Release, with `-Werror`; the whole test suite |
| `sanitize` | the whole suite under AddressSanitizer and UBSan |
| `fuzz-smoke` | each libFuzzer target for 60 s from the committed seeds, after checking the libraries really are instrumented |
| `fuzz-canary` | one fuzz build per injected matching bug; the fuzzer **must** find it, from the seeds and from nothing |
| `replay-canary` | one build per injected replayer bug; the LOBSTER replay **must** detect it, and a no-bug control must stay exact |
| `coverage` | line coverage of the matching engine |

Look at how a canary passes (`ci.yml`, job `fuzz-canary`):

```yaml
          if [ "$rc" -ne 0 ] && [ -n "$crash" ]; then
            echo "PASS: ${{ matrix.bug }} found after ${elapsed}s, crash file: $(basename "$crash")"
          else
            echo "FAIL: ${{ matrix.bug }} not detected (exit code $rc, ${elapsed}s, no crash file)"
            exit 1
          fi
```

It is inverted: the job **succeeds only if the fuzzer fails** (non-zero exit code and a crash file). The same job first checks that the compiled libraries contain the fuzzer's coverage hooks (`__sanitizer_cov`), because a fuzzer without them would get no feedback and could pass by luck (D-150).

`fuzz-nightly.yml` fuzzes for longer every night and keeps the grown corpus as a download. `pages.yml` rebuilds the dashboard demo (default config, seed 42) and publishes it to GitHub Pages on every push to `main`; as the only workflow with write permissions, it pins its actions to exact commit hashes instead of version tags (D-910), and `dependabot.yml` proposes updates to them.

## 15.3 The habits behind the code

Looking back over the course, the same few habits appear in every module. They are worth more than any single piece of code.

**Write the rules first, and write down every choice they leave open.** `SPEC.md` is the rulebook; `DECISIONS.md` has one line per decision the spec did not make, with its reason: 295 of them when this course was written. When lobsim departs from the spec, the departure is recorded with its evidence: the symmetric reference price (D-088 to D-090, Module 9) and the market maker's cancel-near-the-limit rule (D-092, Module 10).

**Measure, then decide.** A review suggested a faster event-queue layout; it was built, timed (476 to 558 ms against 481 to 508 ms before), and reverted because it was not faster (D-905). An alternative trimming rule for the LOBSTER replay was measured, shown to hide most of a problem, and *not* adopted because it departed from the spec and still did not fix it (D-212, D-213).

**Label every number.** Everything computed from simulated data says "synthetic", and real-data sections say "pending" instead of showing anything. The exporter writes a `README_SYNTHETIC.txt` next to its files, the estimator reads it, and `run_real_data.sh` refuses a ticker named `SYNTH...` without one.

**Count everything; drop nothing silently.** Every LOBSTER message lands in exactly one counter; every excluded order in the estimator is counted by reason; every fallback of the calibrated agent is counted; every skipped markout is counted.

**Make runs repeatable.** One tie-break rule, own random generators and distributions, one stream per purpose, no wall-clock time in any output, deterministic number formatting, an event hash that replays.

**Test the tests.** Canaries the fuzzer must find; a differential test that fails if some case never occurred; `validate()` tests that corrupt a book on purpose; a check that fails if the committed seed files are not found (a test filter that matches nothing would otherwise "pass").

**Fail loudly.** An unknown key in a config is an error naming the field (try `"lamda"` instead of `"lambda"` in Lab 15.3); warnings are errors (`-Werror`); sanitizer findings are fatal (`-fno-sanitize-recover=all`); a crossed book throws even in release builds; scheduling an event in the past throws.

**Keep the core small and pure.** `lob` depends on nothing; `compute_quotes` is a pure function; the statistics helpers each have a worked example in a test. Small pure pieces are easy to test by hand.

**Treat inputs as hostile.** The security review (D-900 to D-913) treated every file a tool reads and every command-line argument as possibly malicious: numbers are range-checked before they are multiplied, a fingerprint window is capped, the dashboard checks a bundle's shape before drawing and sets a Content-Security-Policy, and a shell script that once let `--window-s 'a[$(cmd)]'` run a command now accepts only digits.

**Be honest about what the results mean.** The README's sample-run section ends with "An honest reading": the market maker makes money because its counterparties are zero-intelligence traders, and "it is not evidence that this strategy earns money in a real market". "What is not modelled" is a section of its own.

## Lab 15.1: export and view a run

```sh
build/release/viz/lobsim_viz_export --run out/run1 --out out/viz1
```

When this course was written:

```
viz: wrote out/viz1/meta.json and data.bin (6749872 bytes); hash 5aa4b082b3f2168c verified, MM PnL cross-check passed
```

On your own computer (with a browser), view it either way:

- open `web/index.html` directly in the browser, click **Open run…** and pick `out/viz1/meta.json` and `out/viz1/data.bin`; or
- copy the bundle to `web/runs/run1/`, run `python3 -m http.server --bind 127.0.0.1` inside `web/`, and open `http://localhost:8000/?run=run1`.

Find a moment in the last five minutes where the market maker's inventory is large. What does the depth ladder show around its quotes at that moment? (Module 10.3 predicts it.)

## Lab 15.2: make the exporter refuse

```sh
mkdir -p out/tampered && cp out/run1/*.* out/tampered/
sed -i 's/"engine_event_hash": "5aa4b082b3f2168c"/"engine_event_hash": "5aa4b082b3f2168d"/' out/tampered/summary.json
build/release/viz/lobsim_viz_export --run out/tampered --out out/viz_tampered; echo "exit code $?"
```

(On macOS, write `sed -i ''` instead of `sed -i`.) If your run's hash is different, use yours. When this course was written:

```
viz: engine hash mismatch: rebuilt 5aa4b082b3f2168c but summary.json says 5aa4b082b3f2168d. No bundle written.
exit code 2
```

One changed character, and the exporter refuses. Now try instead changing one line in the middle of `out/tampered/orders.log` (restore `summary.json` first). What happens, and why?

<details><summary>Answer</summary>

The rebuilt hash no longer equals the one in `summary.json`, so the exporter refuses again (or, if the line is malformed, the log reader rejects it with its line number). The hash ties the log and the summary together: neither can be changed without the other noticing.

</details>

## Lab 15.3: fail loudly

```sh
sed 's/"lambda": 5.0/"lamda": 5.0/' configs/default.json > out/typo.json
build/release/lobsim_run --config out/typo.json --out out/typo
```

Expected: `lobsim_run: config: noise_traders.lamda: unknown key`, exit code 1. Many programs would ignore the unknown key and silently use the default rate. Why is that dangerous for a research tool? Then try a negative value (`"lambda": -5.0`) and a missing key (delete `"D": 10,`) and read the messages.

<details><summary>Answer</summary>

You would believe you had run an experiment with a changed setting while actually running the default, and your conclusions would be about the wrong market. An error that names the field turns a silent wrong result into a two-second fix.

</details>

## Lab 15.4: read the project's memory

Open `DECISIONS.md`. For each of these, find the decision, then explain in two sentences what was decided and why: D-020 (what `UnknownOrder` means), D-047 (random draws in argument lists), D-072 (which stream a message-triggered draw uses), D-092 (market maker repricing near its limit), D-104 (time-weighted statistics). Then find one decision this course did **not** mention, and explain it to someone else.

## Lab 15.5: read the workflows

Open `.github/workflows/ci.yml`.

1. Which compilers and build types does `build-test` use?
2. In `replay-canary`, what has to happen for the job to pass?
3. Why does `fuzz-canary` run the fuzzer twice, once with `fuzz/corpus/diff` and once with an empty folder?

<details><summary>Answers</summary>

1. GCC 14 and Clang 18, each in Debug and Release.
2. For each injected replayer bug, the strict replays of the hand-written fixtures that can catch it must exit with code 2 (a detected failure). For `DIRECTION_IS_AGGRESSOR` and `NO_SEED` there are two more steps: a normal (bug-free) build must replay a 60 s closed-loop export exactly (exit code 0, the control, so that a broken export cannot look like a caught bug), and the canary build must fail on that same export with code 2. `APPLY_HIDDEN` is checked on the fixtures only, because the simulator produces no hidden executions.
3. A committed seed may trigger the bug by itself, which proves the seeds are good but says nothing about the fuzzer's ability to search. Starting from nothing shows libFuzzer reaches the bug by mutation alone (in about 3,000 attempts, per FUZZING.md).

</details>

## Common questions

**Why no web framework or build step for the dashboard?** Fewer moving parts: plain files work from `file://` and on GitHub Pages, and there are no dependencies to update except one pinned chart library.

**Why not commit the dashboard bundle or the simulation outputs?** They are generated and large; the workflow rebuilds them from the code, so the published demo always matches the code on `main`. `.gitignore` excludes `out/`, `build/`, `site/`, `data/` and generated calibration files.

**Do I need to read all of DECISIONS.md?** No. Use it like an encyclopedia: when you wonder "why is it done this way?", search it for the function or file name.

**What does "pinned to a commit SHA" protect against?** A version tag such as `v7` can be moved to point at different code later; a commit hash cannot. For the one workflow that can publish, that removes a way for someone else's change to run with write access.

## Quiz

1. What two things does `lobsim_viz_export` verify before writing a bundle, and what does it do if they fail?
2. Why are heatmap sizes stored on a log scale?
3. What makes the `fuzz-canary` job pass?
4. Give an example from lobsim of "measure, then decide".
5. Give an example of "count everything, drop nothing silently".

<details><summary>Answers</summary>

1. The engine event hash and the market maker's PnL, both recomputed from `orders.log`, against `summary.json`. If either differs it writes nothing and exits with code 2.
2. Sizes range from one lot to hundreds; a log scale keeps small levels visible next to large ones.
3. The fuzzer must exit with an error and leave a crash file, i.e. it must find the injected bug, both from the committed seeds and from an empty corpus; and the libraries must contain fuzzer instrumentation.
4. Any of: the symmetric reference price (the floor rule's drift was measured over 12 seeds before changing the default); the reverted event-queue layout (D-905); the replay trimming alternative that was measured and not adopted.
5. Any of: every LOBSTER message in exactly one counter; the estimator's excluded orders counted by reason; the calibrated agent's fallbacks and redraws; skipped markouts.

</details>

## What's next

Module 16 has capstone projects that use everything in the course.
