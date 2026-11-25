# Module 0: The big picture

> **In one sentence:** lobsim is a pretend stock market that lives inside your computer, built carefully enough that you can trust what you see in it.
>
> **You will be able to:** say what each part of lobsim does, follow one order from the trader who sends it to the trader who hears about it, build lobsim, run one simulated hour, and find every output file.
>
> **Time:** about 2 hours.

## 0.1 The story

Picture a school playground at lunch where kids trade stickers. There is a noticeboard. If you want to buy a sticker you pin a note: *"I will buy 5 stickers for 10 cents each."* If you want to sell, you pin a different note: *"I will sell 3 stickers for 11 cents each."* When a buyer and a seller agree on a price, a trade happens, and a referee crosses the notes off and announces it to everyone.

A real stock market works the same way, with three differences: it is a computer instead of a noticeboard, the notes are called **orders**, and the referee, called the **matching engine**, handles millions of them a second.

lobsim builds all of that in software:

1. **The noticeboard and the referee.** A matching engine that keeps the list of buy and sell orders (the **order book**) and decides who trades with whom, by exact rules.
2. **Pretend traders.** Little programs called **agents** that decide when to send orders: ten "noise traders" who trade at random, one "informed trader" who secretly knows what the stock is really worth, and one "market maker" who is always willing to buy a bit cheaper and sell a bit dearer, like a shop.
3. **A clock and a post office.** Messages do not arrive instantly. Every order takes a little time to reach the exchange, and every answer takes a little time to come back. lobsim keeps a pretend clock and delivers each message at the right pretend moment.
4. **A scorekeeper.** It watches everything and writes down what happened: every trade, the prices over time, how much money the market maker made, and whether the market behaved like a real one.
5. **A safety net.** Hundreds of tests, a second "slow but obviously correct" engine to compare against, a robot that throws millions of strange orders at the engine looking for mistakes, and checks that run automatically every time the code changes.
6. **Real-world tools.** A reader for real Nasdaq order book files (called LOBSTER files), a way to measure how real traders behave and copy that behaviour into the pretend market (calibration), and a web page that replays a simulated hour like a video.

## 0.2 Why build a pretend market?

Real markets are expensive and risky to experiment in, and you cannot rewind them. A simulator lets you ask questions like *"What happens to the shopkeeper if one trader knows the future?"* and get an answer in a third of a second, then ask again with one thing changed.

But a simulator is only useful if it is **honest**. Many trading simulators make strategies look better than they are. One common trick, done by accident, is to assume that every order you place gets filled. lobsim refuses to do that: an order fills only when the matching engine says another trader traded with it, and it waits in line behind earlier orders at the same price, exactly as on a real exchange. A lot of the care in this project is about honesty: no hidden shortcuts, every number explained, and every claim tested.

## 0.3 The parts and where they live

lobsim is written in C++ (a fast programming language) and split into **libraries**: bundles of code that other code can use. The arrow means "uses".

```
                    apps/  (the programs you run: lobsim_run, lobsim_replay, lobsim_bench, ...)
                      |
     +----------------+------------------+-------------------+--------------+
     |                |                  |                   |              |
  sim (simulator)   replay (LOBSTER)   calib (calibration)  viz (dashboard export)
     |                |                  |                   |
     +-------+--------+---------+--------+-------------------+
             |                  |
         orderlog (orders.log reader/writer)
             |
            lob  (the matching engine: order book, events, the two books)
```

The most important rule of the layout: **`lob` uses nothing else.** The matching engine does not know that a simulator exists. That keeps it small, fast, and easy to test on its own.

| Folder | What is in it | Module |
|---|---|---|
| `include/lob/`, `src/lob/` | The matching engine: types, events, the fast `OrderBook`, the simple `ReferenceBook` | 1 to 5 |
| `tests/`, `fuzz/` | Tests, the differential fuzzer, test helpers | 6 |
| `include/sim/`, `src/sim/` | The simulator: random numbers, the clock, latency, the exchange, the agents, the scorekeeper | 7 to 11 |
| `apps/` | The command-line programs | throughout |
| `bench/`, `apps/bench_main.cpp` | Speed measurements | 12 |
| `include/replay/`, `src/replay/` | The LOBSTER real-data replayer | 13 |
| `include/calib/`, `src/calib/`, `calibration/` | Calibration: measuring real order flow and testing the simulator against it | 14 |
| `viz/`, `web/` | The dashboard exporter and the web page | 15 |
| `configs/` | Settings files for runs (`default.json` is the standard hour) | 9, 10 |
| `tools/`, `scripts/` | Python plotting and analysis, shell scripts | 11, 13, 14 |
| `.github/workflows/` | The automatic checks that run on every change | 15 |
| `SPEC.md`, `DECISIONS.md` | The rulebook lobsim was built to, and every decision the rulebook left open | all |

## 0.4 One order's journey

Follow one order through the system. This is the single most useful picture in the course; come back to it whenever you get lost.

```
  NoiseTrader #5 wakes up (its alarm clock went off)
        |
        |  decides: "market buy, 3 lots"         [agent code, Module 9]
        v
  ctx.send(request)  ----- entry latency ~200 microseconds ----->  Exchange
                                                                     |
                                     writes the request to orders.log [Module 12]
                                     gives it to the OrderBook        [Modules 2-5]
                                     OrderBook answers with EVENTS:
                                        Ack (accepted, here is your order id)
                                        Trade (3 lots at 9998, with the market maker's order)
                                     |
        +----------------------------+-------------------------------+
        | private events, response latency                           | public data, feed latency
        v                                                            v
  to NoiseTrader #5: Ack, Trade                    to EVERY agent: TradePrint, new best bid/ask (BBO)
  to the market maker: Trade (its order was hit)
        |
        v
  each agent updates what it believes about the market, and may send new orders
```

Three things to notice already:

- **Nobody looks at the book directly.** Agents only know what messages tell them, and messages are late. An agent can make a decision based on old news. That is realistic, and it causes interesting problems you will meet in Module 10.
- **The engine answers with events, not with "true" or "false".** Every effect of an order is an event: accepted (`Ack`), refused (`Reject`), traded (`Trade`), removed (`Cancelled`), changed (`Modified`).
- **Every request is written down.** The file `orders.log` holds every request in arrival order. Feeding that file into a fresh engine reproduces every event exactly. That is how lobsim proves a run can be repeated (Module 12).

## 0.5 The numbers you will see

lobsim measures prices in **ticks** and quantities in **lots**, always as whole numbers. A tick is the smallest step a price can move (think "one cent"), and a lot is a fixed bundle of shares (think "one pack of stickers"). The default market starts at a price of 10,000 ticks. Time is counted in **nanoseconds** (billionths of a second) since the start of the run. Module 1 explains why whole numbers matter so much.

## Lab 0.1: build, test, run, replay

Do this in a terminal, in the lobsim folder.

**Step 1. Build.** (Skip if you did it in the course README.)

```sh
cmake --preset release
cmake --build --preset release -j
```

The first command is like reading the recipe and laying out the ingredients: it checks your compiler, downloads three libraries lobsim depends on (GoogleTest for tests, Google Benchmark for speed tests, nlohmann/json for reading settings files) and writes build instructions into `build/release/`. The second command cooks: it turns the C++ source files into programs.

**Step 2. Test.**

```sh
ctest --preset release
```

Expected: `100% tests passed, 0 tests failed out of 670` (the exact count may grow as the project grows).

**Step 3. Predict, then run one simulated hour.** Before running, write down guesses: how many trades do you think happen in one hour with 12 traders? Will the market maker make or lose money?

```sh
./build/release/lobsim_run --config configs/default.json --out out/run1
```

It finishes in under a second of real time. The first lines it printed when this course was written:

```
seed                 42
simulated            3600.000 s, 12 agents
requests             331640
engine events        387289
trades               51653
engine event hash    5aa4b082b3f2168c
resting orders       1135
best bid             9963 x 27
best ask             9965 x 64
fundamental value    9967.9402 after 35999 steps
mm final inventory   -34 lots (the agent believes -34)
...
mm pnl after warm-up 12263.3400 ticks, max |inventory| 51 lots
```

How close were your guesses? 331,640 requests in 3,600 simulated seconds is about 92 a second.

**Step 4. Look at what it wrote.**

```sh
ls out/run1
head -5 out/run1/orders.log
head -5 out/run1/trades.csv
```

| File | What it holds | Module |
|---|---|---|
| `orders.log` | Every request that reached the exchange, one per line | 12 |
| `trades.csv` | Every trade: time, price, quantity, who was the maker and who was the taker | 2, 11 |
| `bbo.csv` | Once a second: best bid, best ask, mid, the hidden true value, the market maker's inventory and PnL | 11 |
| `mm_fills.csv` | Every market maker trade with "markouts" (did the price move against it afterwards?) | 11 |
| `summary.json` | Every measured number of the run, the settings used and the engine event hash | 11 |

**Step 5. Replay.**

```sh
./build/release/lobsim_replay --log out/run1/orders.log
grep engine_event_hash out/run1/summary.json
```

The replay feeds `orders.log` into a fresh matching engine and prints a 16-character **event hash**, a fingerprint of every event the engine produced. It must equal the hash the live run wrote into `summary.json`. When this course was written both were `5aa4b082b3f2168c`. If they ever differ, something in lobsim is broken.

**Step 6. Run it again.** Run step 3 a second time into `out/run1b` and compare: `cmp out/run1/orders.log out/run1b/orders.log` prints nothing when the files are identical. Same settings and same seed give the same run, byte for byte. Now try `--seed 7` and compare again.

## Lab 0.2: look at a run

Open the live dashboard: [ryanhuangcodes.github.io/lobsim/?run=demo](https://ryanhuangcodes.github.io/lobsim/?run=demo). It replays the same hour you just simulated (default settings, seed 42). Press play. Find:

1. The blue heatmap: dark cells are prices where many orders are waiting.
2. The two lines on top of it: the best bid and the best ask.
3. The price chart: the "mid" price and the hidden "fundamental" value. Does the mid follow the fundamental closely, or loosely?
4. The market maker's inventory: does it stay near zero?

Write down one thing that surprised you. You will be able to explain it by Module 11.

## Common questions

**Is this real money or a real market?** No. Every trader is a small program, the stock does not exist, and no real data goes into the default run. It is a laboratory.

**Can lobsim predict real stock prices?** No, and it does not try to. It is for studying how a market *works*: how orders become trades, why a market maker earns or loses, what happens when someone knows more than everyone else.

**Why C++ and not Python?** Speed and control. The matching engine handles about 30 to 50 million messages a second on one core, and C++ lets the code decide exactly how memory is used. The analysis tools that do not need speed are in Python.

**Why are there two matching engines?** One is fast and complicated, one is slow and simple. They must always give exactly the same answers. If they ever disagree, at least one has a bug. Module 6 shows how a robot checks this millions of times.

**What does "deterministic" mean?** Same inputs, same outputs, every time. lobsim's random numbers come from a recipe started by a "seed" number, so seed 42 always tells the same story. Module 7 explains how.

**What is a "hash"?** A short fingerprint computed from a lot of data. Change one detail anywhere in the data and the fingerprint changes completely. lobsim fingerprints every event of a run, so two runs can be compared by comparing 16 characters.

## Quiz

1. What is the matching engine's job, in one sentence?
2. Name the three kinds of agent in the default run.
3. Can an agent read the order book directly?
4. What does `orders.log` contain, and what is it used for?
5. Which library does not depend on any other lobsim library, and why is that useful?

<details><summary>Answers</summary>

1. It keeps the list of resting buy and sell orders and decides, by fixed rules, which orders trade with each other and at what price.
2. Noise traders (10 of them), one informed trader, one market maker.
3. No. Agents see the market only through delayed messages: public data (best prices, trades) and private replies about their own orders.
4. Every request that reached the exchange, with its arrival time. Replaying it through a fresh engine reproduces every event, which proves the run is repeatable (the event hashes match).
5. `lob`, the matching engine. It can be tested, measured and reused on its own, without the simulator.

</details>

## What's next

Module 1 slows down and builds the vocabulary of markets: what a bid and an ask are, what the spread is, and why prices are counted in whole ticks.
