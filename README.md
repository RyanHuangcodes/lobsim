# lobsim

A C++20 limit order book, matching engine and agent-based trading simulator.

## Benchmarks

**What these numbers are.** They are in-process service times: the time one thread spends inside the matching engine per message, measured in a closed loop, where the next message is sent only after the previous one has returned. There is no network, no queue and no other traffic. They are not end-to-end latency under load. A closed loop also hides queueing delay, because a slow message delays the ones behind it without that wait being measured (coordinated omission), so tail latency in a real system would be worse than the tails shown here.

Reproduce with:

```
cmake --preset release
cmake --build --preset release -j
./build/release/lobsim_bench
./build/release/micro_bench
```

Machine and build, as printed by the harness: Apple M4; clang 17.0.0 (clang-1700.0.13.5); Release; flags: -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Werror

### Throughput harness (`lobsim_bench`)

The stream is 10,000,000 seeded messages shaped like the simulator's noise-trader flow: 85% limit and 15% market orders among new orders, limit prices 1 to 10 ticks from a slowly drifting reference price, sizes 1 to 10 lots, and every limit order cancelled after an exponentially distributed lifetime. A cancel is sent even when its order has already been filled, so 1,175,093 of the 10,000,000 messages are rejected with `UnknownOrder`. The run ends with 957 resting orders on 36 price levels. Message generation is outside every timed region.

One representative run of three:

| Measurement | Result |
|---|---|
| OrderBook throughput, 10000000 messages (timed per block of 4096) | 47.00 million msgs/s (mean 21.3 ns per message) |
| OrderBook service time, one timer pair per message | p50 0 ns, p90 42 ns, p99 42 ns, p99.9 84 ns, max 6300167 ns |
| OrderBook service time, mean of each batch of 32 messages | p50 20 ns, p90 22 ns, p99 26 ns, p99.9 30 ns, max 123337 ns |
| Timer: back-to-back pair of clock reads | min 0 ns, median 0 ns, smallest step 41 ns |
| Heap allocations inside OrderBook calls | 129865 in 10000000 messages (0.0130 per message) |
| ReferenceBook throughput, first 100000 messages | 1.508 million msgs/s (mean 663 ns per message) |
| ReferenceBook service time, one timer pair per message | p50 625 ns, p90 1083 ns, p99 2917 ns, p99.9 7417 ns, max 69959 ns |
| OrderBook / ReferenceBook throughput on the same 100000 messages | 25x |
| Event hash, OrderBook vs ReferenceBook, first 100000 messages | identical (35f5eadf1b97dd05) |

How to read it:

- **The timer is too coarse for single messages.** On this machine `std::chrono::steady_clock` advances in steps of 41 ns, and one message takes about 21 ns. A per-message sample is therefore 0, 42 or 84 ns and never the true value, which is why the per-message p50 reads 0. One clock read costs about 16 ns.
- **Use the block-timed row for the average.** It times 4,096 messages with one timer pair, so the timer's step and cost are negligible.
- **Use the batch-of-32 row for the body of the distribution.** Each sample is the mean of 32 consecutive messages. It resolves the body (p50 20 ns, p99 26 ns) but averages away single slow messages.
- **Use the per-message row for the far tail and the maximum only.** There a 41 ns step no longer matters.
- **The maximum is the id index growing.** Orders are looked up through a `std::vector` indexed by order id, which doubles when it is full. With 5.4 million order ids it grows to 64 MB during this run, and the message that triggers the copy pays for it. The worst message grows with the largest doubling: 0.6 ms in a 1.8-million-message run, about 2 ms at 3.6 and 7.2 million, and 4 to 6 ms at 10 million. Reserving the index up front would remove it.
- **Run-to-run variation.** Over three runs throughput was 42.4, 47.0 and 47.4 million msgs/s, about 10%, and the speed-up over `ReferenceBook` was 25x to 32x. The percentiles of the batch-of-32 row moved by a few nanoseconds, except that one run showed p99 42 ns and p99.9 77 ns. The maximum varied between 4.1 and 6.3 ms. Other programs were running on the machine.

### Heap allocations

Global `operator new` is replaced in the `lobsim_bench` binary only and counts allocations made while an engine call is running. The caller's event buffer is reserved once and never grows, so every counted allocation is the engine's own.

In the 10,000,000-message run there were 129,865 allocations, 0.0130 per message:

| Allocation size | Count | What it is |
|---|---|---|
| 80 bytes | 129,839 | a `std::map` node, one per newly created price level |
| 16 bytes to 64 MB, doubling | 23 | growth of the id index vector |
| 262,144 bytes | 1 | the one chunk of 4,096 orders the pool needed |
| 32,768 bytes | 1 | the pool's free list for that chunk |
| 8 bytes | 1 | the pool's list of chunks |

This matches what the design predicts: allocations happen only when a price level is created or when the pool or a vector grows. The audit below counts allocations per kind of operation on a warm book, where the pool and the id index have already grown:

| Operation | Operations | Allocations | Deallocations |
|---|---|---|---|
| add, existing price level | 500 | 0 | 0 |
| add, new price level | 500 | 500 | 0 |
| cancel, level stays | 500 | 0 | 0 |
| cancel, level becomes empty | 500 | 0 | 500 |
| match within one level, level stays | 400 | 0 | 0 |
| match that empties a level | 200 | 0 | 200 |
| modify, same price, smaller qty | 500 | 0 | 0 |
| modify to a new price level | 200 | 200 | 4 |

### Microbenchmarks (`micro_bench`, Google Benchmark)

Time per operation with the book pre-filled to 1,000 resting orders (50 price levels per side) and to 100,000 (500 levels per side). Each benchmark times a batch of 128 operations and undoes the batch outside the timed region, so the book stays near its nominal size.

| Operation | 1,000 orders | 100,000 orders |
|---|---|---|
| Add a non-crossing order to an existing level | 18.1 ns | 18.3 ns |
| Add a non-crossing order that creates a level | 32.6 ns | 38.0 ns |
| Cancel a random resting order | 11.0 ns | 18.8 ns |
| Aggressive order that fully fills one maker in one level | 13.6 ns | 13.8 ns |
| Modify to a smaller quantity at the same price | 10.7 ns | 16.2 ns |
| Read best bid and best ask | 1.30 ns | 1.24 ns |

These are from one run. Over three runs most rows moved by 1 to 5 ns. The two rows that touch a random order in the 100,000-order book varied more: cancel between 18 and 28 ns, modify between 16 and 30 ns. Those operations miss the cache on the order they touch, so they are the most sensitive to what else the machine is doing.
