# Module 12: Replay and speed

> **In one sentence:** every run writes down every request it sent to the engine, so the engine's answers can be recomputed and fingerprinted, and lobsim measures its own speed carefully enough to say exactly what its numbers mean.
>
> **You will be able to:** read and write `orders.log` lines, explain why replaying the log reproduces the run, compute the FNV-1a event hash by hand, explain why the hash uses a fixed byte encoding, read the benchmark report (throughput, percentiles, timer resolution, allocations), and explain coordinated omission.
>
> **Time:** about 3 hours.

## 12.1 The order log

The exchange appends every arriving request to `orders.log` **before** applying it (Module 8.5). The format, from `include/sim/order_log.hpp`:

```
# lobsim orders.log v1 max_price=<max_price>
<time_ns> N <owner> <client_order_id> <side> <type> <tif> <price> <qty> <post_only> <stp>
<time_ns> C <owner> <order_id>
<time_ns> M <owner> <order_id> <new_price> <new_qty>
```

Plain text, one request per line, whole decimal numbers only; enums and bools are written as their numbers (side 0 = Buy, 1 = Sell; type 0 = Limit, 1 = Market; tif 0 = GTC, 1 = IOC, 2 = FOK; stp 0 = CancelTaking, 1 = CancelResting). Text is slower and bigger than a binary format, but you can read it, `grep` it, and diff two of them.

There is **one codec**: the exchange writes through `append_request` and every reader goes through `OrderLogReader` (D-074, D-137). If the writer and reader were separate pieces of code, they could drift apart.

```cpp
std::optional<std::pair<TimeNs, lob::Request>> parse_request(std::string_view line) {
  const Tokens t = split(line);
  if (t.size() < 2 || t[1].size() != 1) return std::nullopt;
  TimeNs time = 0;
  if (!parse(t[0], time) || time < 0) return std::nullopt;

  switch (t[1][0]) {
    case 'N': {
      lob::NewOrder o;
      if (t.size() != 11) return std::nullopt;
      if (!parse(t[2], o.owner) || !parse(t[3], o.client_order_id) || !parse_small(t[4], 1, o.side) ||
          !parse_small(t[5], 1, o.type) || !parse_small(t[6], 2, o.tif) || !parse(t[7], o.price) ||
          !parse(t[8], o.qty) || !parse_small(t[9], 1, o.post_only) || !parse_small(t[10], 1, o.stp))
        return std::nullopt;
      return std::pair<TimeNs, lob::Request>{time, o};
    }
    ...
```

Parsing is strict on purpose: exactly 11 fields for an `N` line, each field must be a whole number with nothing left over (`std::from_chars` must consume the whole token), enums must be in range (`parse_small(t[6], 2, ...)` accepts only 0, 1, 2). Anything else is "malformed" and the reader throws a `ReplayError` that names the line. The reader also refuses a time earlier than the previous line's (equal times are fine).

This parser is also fuzzed (`fuzz_replay_parse`, Module 6): any bytes must either be rejected with a `ReplayError` or be read, written back out, and read again to exactly the same requests and the same text. That "double round trip" checks that the codec's own output is always accepted and never changes.

## 12.2 Replay: running the engine again

`src/sim/replay.cpp` is the whole replay, about 20 lines:

```cpp
ReplayResult replay_log(std::istream& in) {
  OrderLogReader reader(in);
  lob::OrderBook book(lob::BookConfig{reader.max_price()});
  lob::EventHasher hasher;
  lob::EventBuffer events;
  ReplayResult result;
  result.max_price = reader.max_price();
  while (const auto logged = reader.next()) {
    events.clear();
    lob::apply(book, logged->request, events);
    hasher.add(events);
    ...
  }
  result.event_hash = hasher.value();
  return result;
}
```

A fresh book, the logged requests in order, every event into a hasher. Why does this reproduce the live run? Because the engine is **deterministic**: its answer depends only on the requests it is given and their order, never on the time of day, random numbers or anything else. The log holds exactly those requests in exactly that order. Everything random in the simulation (who sends what, when) is already baked into the log.

So `lobsim_replay` prints the same hash as `summary.json`. That one comparison checks a lot at once: the log is complete, the codec loses nothing, and the engine did not depend on anything outside its input. The ctest `cli_run_then_replay` runs `lobsim_run` and `lobsim_replay` as real programs, compares the two hashes and checks that a corrupted log is refused (D-110); `tests/replay.cpp` checks it in process, going through the text and not through saved C++ objects.

## 12.3 The event hash

A **hash function** turns any amount of data into a short fixed-size number, its fingerprint. A good one changes completely when any input bit changes. lobsim uses **FNV-1a 64-bit** (`EventHasher` in `include/lob/events.hpp`):

```cpp
  void add_bytes(const void* data, std::size_t n) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
      h_ ^= p[i];
      h_ *= kPrime;
    }
  }
  ...
  static constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
  static constexpr std::uint64_t kPrime = 0x100000001b3ULL;
```

Start from a fixed number, then for each byte: mix the byte in with XOR, stir with a multiplication (wrapping at 64 bits). That is the whole algorithm. It is fast and simple, and good enough for detecting differences (it is **not** a cryptographic hash: someone could deliberately construct two inputs with the same hash, but nobody is attacking lobsim's tests).

**Which bytes?** Not the raw memory of the event structs. A `Trade` in memory has padding bytes between fields whose values are undefined, and the layout could differ between compilers. Instead, each event is written in a **fixed little-endian encoding** (D-015): one tag byte for the kind (Ack 0, Reject 1, Trade 2, Cancelled 3, Modified 4), then every field in declaration order, 64-bit fields as 8 bytes, 32-bit as 4, enums and bools as 1, least significant byte first:

```cpp
  void encode(const Trade& e) noexcept {
    u8(2); u64(e.trade_id); i64(e.price); i64(e.qty); u64(e.maker_order_id);
    u64(e.taker_order_id); u32(e.maker_owner); u32(e.taker_owner); en(e.aggressor_side);
  }
```

So the same events give the same hash on any machine and with any compiler. **Skipped on purpose:** the `encode` functions for the other four events follow the same pattern.

## 12.4 Measuring speed honestly

The README's speed numbers come from `lobsim_bench` (`apps/bench_main.cpp`) and `micro_bench` (`bench/micro_bench.cpp`, using Google Benchmark). The interesting part is less the numbers than how carefully they are labelled.

**What is measured.** "In-process service time": the time one thread spends inside the engine per message, in a **closed loop** (the next message is sent only after the previous one returned). No network, no queue, no other traffic. Not end-to-end latency.

**Coordinated omission.** In a closed loop, a slow message delays all the messages behind it, but those messages are never *measured* as waiting, because they had not been sent yet. In a real system they would have been sitting in a queue. So closed-loop tail latencies look better than the real thing. SPEC.md requires the README to say this, and it does.

**The timer is too coarse.** On the README's machine `std::chrono::steady_clock` ticks in steps of 41 ns, and a message takes about 21 ns. Timing one message gives 0, 42 or 84 ns, never the truth. So the harness measures three ways (D-061):

| Pass | What it is good for |
|---|---|
| one timer pair per block of 4,096 messages | the average (throughput): the timer's cost and step vanish |
| the mean of each batch of 32 messages | the body of the distribution (p50 to p99.9) |
| one timer pair per message | the far tail and the maximum, where a 41 ns step no longer matters |

**Percentiles, never only the mean.** p50 is the median (half the messages were faster), p99 means 99% were faster, and so on. The mean hides rare slow messages, and in trading systems the rare slow ones are often what matters.

**The maximum explained.** The slowest message of a 10-million-message run took several milliseconds. The README traced it: the id-index vector (Module 5) doubled, and copying 64 MB of pointers landed on one unlucky message. A test build that sized the index in advance brought the maximum down to tens of microseconds.

**Counting allocations.** The harness replaces the global `operator new` (the function behind every heap allocation in C++) in the benchmark program only, and counts while an engine call is running:

```cpp
void* allocate(std::size_t n) {
  void* p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr) throw std::bad_alloc();
  if (counting) record(n);
  return p;
}
...
void* operator new(std::size_t n) { return alloc_count::allocate(n); }
```

The design (Module 5) predicts allocations only when a price level is created, or when the pool or a vector grows. The report confirms it (Lab 5.4).

**Benchmarks refuse to measure broken code.** `lobsim_bench` exits with an error if its passes disagree on the number of events or if the two books' hashes differ on the shared prefix: "a benchmark of a broken run is worse than none" (D-064).

## Lab 12.1: read and break an order log

Decode these lines of `out/run1/orders.log` by hand (every field), then check your reading against `build/release/labs/lab08_trace_log out/run1/orders.log 12`:

```
14113737 N 2 1 0 0 0 9990 2 0 0
21168497 N 5 1 0 1 1 0 3 0 0
21235007 M 12 3 9998 5
```

Now break a copy of the log three ways and replay each:

```sh
head -1000 out/run1/orders.log > out/small.log
build/release/lobsim_replay --log out/small.log | tail -1

# 1. an impossible value: post_only = 7 on line 5
sed '5s/ 9998 5 1 0$/ 9998 5 7 0/' out/small.log > out/bad1.log
build/release/lobsim_replay --log out/bad1.log

# 2. time going backwards: swap lines 8 and 9
awk 'NR==8{l=$0; next} NR==9{print; print l; next} {print}' out/small.log > out/bad2.log
build/release/lobsim_replay --log out/bad2.log

# 3. a legal change: line 7 asks for 9997 instead of 9998
sed '7s/^21235007 M 12 3 9998 5$/21235007 M 12 3 9997 5/' out/small.log > out/changed.log
build/release/lobsim_replay --log out/changed.log | tail -1
```

When this course was written:

```
engine event hash    5d9e61a0944bbe88
lobsim_replay: out/bad1.log: line 5: malformed request: 21120731 N 12 2 1 0 0 9998 5 7 0
lobsim_replay: out/bad2.log: line 9: time goes backwards: 21235939 after 21345475
engine event hash    4c962a3b3f7326f3
```

The first two are refused with the exact line (and exit code 1). The third is a valid log, but a different one: the modify that was rejected in the real run now succeeds, and the hash is completely different.

## Lab 12.2: the hash by hand

```sh
python3 course/labs/py/fnv1a.py
(cat course/labs/scripts/worked_example.txt; echo hash) | build/release/labs/lab02_book_repl | grep "event hash"
```

The Python script encodes the six events of the worked example with the D-015 layout and hashes them. When this course was written:

```
FNV-1a of the bytes of 'a': 0xaf63dc4c8601ec8c (the published test value is 0xaf63dc4c8601ec8c)
hash of the worked example's six events: 6624cbaf5d769c4f
same events, one price changed:         7dcedb2b6865f902
  event hash so far: 6624cbaf5d769c4f
```

Python and C++ agree. One price changed and the whole fingerprint changed. Open the script and find where `u64`, `u32` and `u8` write little-endian bytes.

## Lab 12.3: read the benchmark

```sh
build/release/lobsim_bench --messages 2000000 --ref-messages 50000
```

On the machine used for this course (an Intel Xeon, GCC 11):

```
OrderBook, 2000000 messages
  throughput (timed per block of 4096)          28.67 million msgs/s   (mean 34.9 ns per message)
  service time, one timer pair per message     p50       53   p90       75   p99      116   p99.9      213   max    6203528  ns
  service time, mean of each batch of 32       p50       29   p90       34   p99       40   p99.9       66   max     193757  ns
  ...
timer (std::chrono::steady_clock)
  back-to-back pair of reads: min 20 ns, median 22 ns; one read costs about 22.4 ns; smallest
  non-zero step 20 ns
  This is SIGNIFICANT next to the block-timed mean of 34.9 ns per message.
  ...
ReferenceBook, first 50000 messages of the same stream
  throughput (timed per block of 4096)          0.296 million msgs/s   (mean 3380.1 ns per message)
  ...
  event hash fe0c7d68e6c30b07; OrderBook on the same 50000 messages: fe0c7d68e6c30b07
```

1. The per-message p50 (53 ns) is higher than the block mean (34.9 ns). Why?
2. Which line would you quote as "how long does one message usually take"?
3. The max is over 6 ms. What is the most likely cause, according to this module?
4. How many times faster is the fast book than the reference book here?
5. Why does the harness print the two books' hashes?

<details><summary>Answers</summary>

1. On this machine one clock read costs about 22 ns, and a per-message sample includes part of the timer's own cost. The block-timed mean shares one timer pair among 4,096 messages.
2. The batch-of-32 p50: 29 ns here. It resolves the body of the distribution without being dominated by the timer.
3. The id-index vector doubling and copying its contents during one message.
4. About 28.67 / 0.296 ≈ 97 times on this stream. The README reports 31 times on its Apple M4 for the first 100,000 messages; the ratio depends on the machine and on how big the book gets. Lab 5.1 showed it growing with the book's size.
5. To prove both books did the same work on the same messages: a speed comparison between two programs that disagree would mean nothing.

</details>

Then run two microbenchmarks with Google Benchmark:

```sh
build/release/micro_bench --benchmark_filter='BM_BestPrices|BM_Cancel'
```

Compare `BM_Cancel/1000` with `BM_Cancel/100000` (1,000 against 100,000 resting orders). Cancel is O(1) in theory; why might it still be slower on a bigger book? (On the machine used for this course: 17.6 ns against 57.0 ns.)

<details><summary>Answer</summary>

Caches again. With 100,000 orders, the orders, levels and index entries touched by a random cancel are less likely to be in the processor's fast cache, so each pointer followed costs more. Big-O counts steps, not the price of each step.

</details>

## Common questions

**Why not log the events too?** They can always be recomputed from the requests, and recomputing them is the check. Logging only the input keeps the file smaller and the replay meaningful.

**Could two different runs have the same hash by accident?** In principle yes (there are only 2^64 possible hashes), but for a non-malicious check the chance is about 1 in 18 quintillion. In practice a different hash means a different event stream.

**Why text instead of binary for `orders.log`?** Readability and safety: you can read, search and diff it, and a parser for whole decimal numbers is simple to make strict. The 11 MB of a one-hour log is not a problem.

**Is 30 to 50 million messages a second fast?** For a single-threaded in-process engine on one core, it is in the range people quote for well-built engines; it is not a claim about a real exchange, which also handles networking, persistence, risk checks and many instruments.

**What is Google Benchmark doing that a simple timer does not?** It repeats each operation until the timing is stable, reports CPU and wall time, and handles warm-up. lobsim adds two "calibration" benchmarks that measure the cost of its own timer pauses, so the README can say how much of each number is measurement overhead (D-065).

## Quiz

1. Write the `orders.log` line for: at 5,000 ns, owner 3 sends a post-only GTC limit sell of 4 lots at 10,001 with client id 9 and STP CancelTaking.
2. Why does replaying `orders.log` reproduce exactly the same events?
3. Why does the event hash encode fields one by one instead of hashing the structs' memory?
4. What is coordinated omission?
5. Why does `lobsim_bench` time blocks, batches and single messages separately?

<details><summary>Answers</summary>

1. `5000 N 3 9 1 0 0 10001 4 1 0`.
2. The engine is deterministic: its output depends only on its input requests and their order, and the log records exactly those, in arrival order.
3. Struct memory contains padding bytes with undefined values and a layout that can differ between compilers; a fixed encoding gives the same bytes everywhere.
4. In a closed-loop test, a slow request delays the following requests, but their waiting time is never measured, so the measured tail latency is better than in a real system with a queue.
5. The timer's resolution and cost are similar to one message's service time. Blocks give an accurate average, batches the body of the distribution, single messages the far tail and maximum.

</details>

## What's next

Everything so far happened inside lobsim's own world. Module 13 brings in the outside world: real Nasdaq order book files, and the question of whether lobsim's matching rule agrees with a real exchange.
