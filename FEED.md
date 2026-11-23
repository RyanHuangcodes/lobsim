# Binary market-data feed (ITCH 5.0)

`lobsim_feed` turns every change to the visible book of a lobsim run into a binary market-data feed in the format of Nasdaq TotalView-ITCH 5.0. It then proves the feed is complete: it rebuilds the order book from the bytes alone and checks, after every request, that the rebuilt book equals the engine's.

The feed can also be cut into MoldUDP64 packets, the framing Nasdaq uses to send ITCH over UDP, and read back with gap detection ([below](#moldudp64-packets)).

The feed is a subset of ITCH 5.0. It uses five message types with ITCH's own fixed body layouts, every integer big-endian, and each body preceded by a 2-byte big-endian length. There is no custom header, so any ITCH reader can walk the file. lobsim's parser walks any ITCH 5.0 file framed this way and skips the types it does not use. The decisions behind every rule below are D-300 to D-331 in [DECISIONS.md](DECISIONS.md).

## Contents

- [The wire format](#the-wire-format)
- [How engine events map to messages](#how-engine-events-map-to-messages)
- [Hand check](#hand-check)
- [Running it](#running-it)
- [How the feed is verified](#how-the-feed-is-verified)
- [MoldUDP64 packets](#moldudp64-packets)
- [Measured numbers](#measured-numbers)
- [Code layout](#code-layout)
- [Not included](#not-included)

## The wire format

Every body starts with the same 11 bytes:

| Offset | Length | Field | Value in lobsim |
|---|---|---|---|
| 0 | 1 | message type | ASCII `S`, `A`, `E`, `X` or `D` |
| 1 | 2 | stock locate | 1 |
| 3 | 2 | tracking number | 0 |
| 5 | 6 | timestamp | ns after midnight: 34,200,000,000,000 (9:30) + simulated ns |

| Type | Body bytes | On disk | Fields after the prefix (offset: name, length) | Emitted when |
|---|---|---|---|---|
| S System Event | 12 | 14 | 11: event code (1): `O` start of messages, `C` end of messages | once before the first message, once after the last |
| A Add Order | 36 | 38 | 11: order ref (8), 19: side `B`/`S` (1), 20: shares (4), 24: stock `LOBSIM` + 2 spaces (8), 32: price (4) | a limit remainder comes to rest (a new order, or re-entry after a priority-losing modify) |
| E Order Executed | 31 | 33 | 11: order ref (8), 19: executed shares (4), 23: match number (8) | every trade, on the maker's ref; match number = `trade_id` |
| X Order Cancel | 23 | 25 | 11: order ref (8), 19: cancelled shares (4) | a same-price size reduction (`Modified` with priority kept) |
| D Order Delete | 19 | 21 | 11: order ref (8) | a resting order is cancelled (user, or self-trade `CancelResting`), or loses priority on a modify |

Field values:

- **Order ref** = the engine's `OrderId`. Ids are dense and start at 1, so the decoder indexes a vector by reference.
- **Price(4)** = ticks x 100, because 1 tick is $0.01 and ITCH has 4 implied decimals. A price above 4,294,967,295 (42,949,672 ticks) is refused, and so is a quantity above 4,294,967,295. So is a timestamp of 2^48 ns or more, which leaves 68.7 hours of simulated time after 9:30.

## How engine events map to messages

The encoder makes one pass over each request's engine events, in order, with a mirror of the resting orders:

1. An `Ack` of a GTC limit order marks it as the *subject*: the order that may come to rest.
2. A `Modified` that keeps priority gives an X for the reduction. One that loses priority gives a D for the old order and makes the order the subject at its new price and quantity.
3. A `Trade` gives an E on the maker. If the subject was the taker, its size counts against the subject.
4. A `Cancelled` of the subject means it never rested, so nothing is published. A `Cancelled` of a resting order gives a D.
5. After the events, a subject that still has shares left gives an A with the remainder.

This yields these rules:

- An execution that takes a resting order to zero gives only an E. In ITCH an order executed in full is gone, so a following D would name an unknown reference.
- A priority-losing modify is D, then its executions as a taker, then A, with the **same** reference. ITCH's Replace (U) is not used: it issues a new reference, and it cannot express that the re-entered order trades before it rests.
- The taker's A comes **after** its executions, which is the order an exchange publishes. Applied the other way round, the A would cross the book.
- Orders that never rest give nothing for themselves: market, IOC and FOK orders, rejects, fully filled GTC orders, and takers stopped by self-trade `CancelTaking`. Only the E messages on the makers they hit appear.
- Types U, C, P, F, R and the rest are never emitted. lobsim has no hidden orders (P), always trades at the maker's price (C), has no attribution (F) and has one instrument (R).

## Hand check

An A for order 7, buy 5 lots at 10,000 ticks, at simulated time 1.5 s, is these 38 bytes:

```
00 24 | 41 | 00 01 | 00 00 | 1f 1b 28 42 1f 00 | 00 00 00 00 00 00 00 07 | 42 | 00 00 00 05 | 4c 4f 42 53 49 4d 20 20 | 00 0f 42 40
len36 | A  | locate| track | 34,201,500,000,000 ns | ref 7              | B  | 5 shares    | "LOBSIM  "              | 1,000,000 = $100.0000
```

34,200 s + 1.5 s = 34,201.5 s = 34,201,500,000,000 ns = `0x1F1B28421F00`, and 10,000 x 100 = 1,000,000 = `0x000F4240`. The test `FeedItch.GoldenAddOrder` checks exactly this array. S, E, X and D have their own hand-written arrays in `tests/unit/feed_itch_test.cpp`.

Because every message has a fixed size, the size of a feed file can be checked by hand: bytes = 38 A + 33 E + 25 X + 21 D + 14 S. The closed-loop test checks this equation on every run.

## Running it

```sh
build/release/lobsim_run --config configs/default.json --seed 42 --out out/run1
build/release/lobsim_feed check  --run out/run1                         # every check below, no file written
build/release/lobsim_feed encode --run out/run1 --out out/run1/feed.itch  # orders.log -> engine -> ITCH file
build/release/lobsim_feed decode --feed out/run1/feed.itch --depth 5      # parse, rebuild, print counts + book
```

Each subcommand ends with one line in the same format, for example on the default 1-hour run with seed 42:

```
feed: S 2 A 171237 E 51653 X 0 D 141489 bytes 11182852 digest 06adcb6b1c2a0518
```

The digest is a hash of the final book: the engine's book for `check` and `encode`, the rebuilt book for `decode`. All three agree. The exit codes are the same as for the LOBSTER tools:

| Code | Meaning |
|---|---|
| 0 | done; for `check`, every check passed |
| 1 | bad arguments, an unreadable or malformed file, or a value the feed cannot hold |
| 2 | the first mismatch, named on stderr (the request number and what differed) |

`decode` also accepts ITCH files that `lobsim_feed` did not write. It skips and counts the types it does not use. One limit applies: references must stay below 2^26, because lobsim's dense reference index cannot hold larger ones.

## How the feed is verified

`lobsim_feed check --run <dir>` replays `orders.log` through the engine's `OrderBook` with `lob::apply`. It encodes each request's events in memory, parses them straight back into a `FeedBook` and compares:

1. **After every request:** best bid, best ask and the top 10 levels of both sides.
2. **Every 10,000 requests and at the end:** a full book digest, FNV-1a 64 over every level's price, total quantity and order count, then each order's remaining quantity in FIFO order. Order ids are left out because the two books number their orders differently. The FIFO order still captures time priority.
3. **Trade tape:** one E per trade, in order, whose match number and shares equal the trade's id and quantity. The totals must match at the end.
4. **Round trip:** re-encoding every parsed message gives exactly the bytes that were parsed.

The `FeedBook` applies messages through the book-builder operations (`insert_passive`, `reduce`, `remove`), with no matching. It stops at the first message that does not fit the book: an unknown reference, an A that would cross, an execution or cancel larger than the order, or anything before S O or after S C. The crossing check is the most useful one. If the encoder ever published a taker's A before its executions, the rebuilt book would cross and the run would stop at that message.

Other checks, all in CI:

| Check | Where | What it shows |
|---|---|---|
| Unit tests | `tests/unit/feed_itch_test.cpp`, `feed_encoder_test.cpp`, `feed_decoder_test.cpp` (in `lob_tests`) | golden bytes for every type; the byte-order helpers; one engine scenario per mapping rule; the overflow checks; every parser and `FeedBook` error; the round trip |
| Closed loop | ctest `feed_closed_loop_60s`, `feed_closed_loop_600s` (seed 7) | `check`, `encode` and `decode` all exit 0, print the same summary line, and the file size equals the fixed message sizes |
| Seeded differential test | `tests/fuzz_diff.cpp`, 200 seeds x 10,000 random requests | every request is encoded and rebuilt; the rebuilt book's digest equals the engine's at the end of every seed |
| Fuzzing | `fuzz_feed_parse` in `fuzz-smoke` (60 s) and nightly (10 min); seeds and regressions in ctest | arbitrary bytes either parse and re-encode to exactly the same bytes, or are refused cleanly at an offset; the `FeedBook` never crashes ([FUZZING.md](FUZZING.md)) |
| Injected-bug canary | CI job `feed-canary` | an encoder built with `LOBSIM_FEED_BUG=add_before_exec` publishes the taker's A before its executions. `check` must exit 2 on the hand-written exporter fixture, the differential stream must stop on the feed's crossing check, and the normal build must pass both |

In the default simulator, no order ever trades as a taker and then rests: its noise traders price from their own view of the book, and its market maker is post-only. So a closed-loop simulator run cannot show the canary bug. The canary is checked on the fixture and on the random differential stream instead, which both contain that case (D-327).

## MoldUDP64 packets

```sh
build/release/lobsim_feed packetize   --feed out/run1/feed.itch --out out/run1/feed.mold [--session LOBSIM] [--max-packet 1400]
build/release/lobsim_feed depacketize --packets out/run1/feed.mold --out out/run1/feed.back.itch
```

A MoldUDP64 packet is a 20-byte header followed by its messages:

| Offset | Length | Field | Value in lobsim |
|---|---|---|---|
| 0 | 10 | session | `--session`, default `LOBSIM`, padded with spaces |
| 10 | 8 | sequence number | of the packet's first message; the first message of the session is 1 |
| 18 | 2 | message count | 1 to 65,534; 0 is a heartbeat; 0xFFFF ends the session |
| 20 | | message blocks | each a 2-byte length and the message: exactly the framing of the feed file |

`packetize` fills each packet with as many whole messages as fit in `--max-packet` bytes (default 1,400, header included, which keeps a packet inside a 1,500-byte Ethernet frame with room for the IP and UDP headers). It ends with an end-of-session packet whose sequence number is the next one. A file does not keep packet boundaries, so the packet file stores each packet behind its own 2-byte big-endian length. Only `packetize` and `depacketize` use that extra length; there are no sockets.

`depacketize` reads packets in file order and follows the sequence numbers. A number already delivered is a duplicate, such as a retransmission, and is skipped. A jump forward is a gap: the missing range is printed and the run exits with code 2. A jump can show up in a message packet, a heartbeat or the end-of-session packet. A missing end-of-session packet also exits 2. A malformed packet file exits 1 with the byte offset: a truncated packet, a packet shorter than its header, a message that runs past its packet, bytes left after the message count, a change of session, or anything after the end of the session. `packetize --drop-packet K` leaves out the K-th message packet to simulate a loss.

On the default 1-hour run with seed 42, the 364,381 messages go into 8,196 packets, 44.5 messages per packet, plus the end-of-session packet. The 20-byte packet headers add 1.47% to the feed (163,940 bytes); the packet file, with its 2-byte length per packet, is 11,363,186 bytes, 1.61% more than the feed. `depacketize` gives back the feed byte for byte, and `--drop-packet 1000` is reported as the gap 44,151 to 44,196 (46 messages).

The closed-loop tests check the round trip and the reported gap on every run. The unit tests (`tests/unit/feed_mold_test.cpp`) cover a hand-checked packet, the size limit and greedy filling, a lost packet, a loss shown only by the end-of-session packet, a retransmitted packet, heartbeats, the session and first sequence number, and every malformed case. The `fuzz_feed_parse` target also puts the part of each input that parses into packets of a size chosen from the input. It reads them back, requires the same bytes with no gap, and reads the raw input as a packet file.

## Measured numbers

All on one machine: Intel Xeon Platinum 8488C, 8 vCPU, Linux x86-64, GCC 11.5, Release (`-O3`). Each number is the median of 3 runs, with the load average below 1.1 throughout. The engine's own throughput on the same machine is given for comparison. Apple M4 numbers (where the README's engine benchmarks were measured) are **pending**.

| Measurement | How | Result |
|---|---|---|
| Encode throughput | `micro_bench` `BM_FeedEncode`: the engine events of 1,000,000 pre-generated requests, encoded in memory (no engine time inside the timer) | **47.2 million requests/s** (44.4 million ITCH messages/s, 1.28 GiB/s) |
| Engine, same machine | `lobsim_bench` throughput harness (10,000,000 messages); `BM_FeedEngineSame` on the feed benchmark's stream | 26.4 million msgs/s; 29.3 million requests/s |
| Parse only | `BM_FeedParse`: the whole feed with a handler that does no work | **115.8 million messages/s, 3.34 GiB/s** |
| Parse + rebuild | `BM_FeedParseRebuild`: the whole feed into a `FeedBook` | **25.8 million messages/s** (27.5 million requests/s), bounded by the book, not the parser |
| Size | `lobsim_feed encode` on the default 1-hour run, seed 42 (331,640 requests) | **11,182,852 bytes, 33.72 bytes per request**; `orders.log` is 11,100,312 bytes, 33.47 per request |
| MoldUDP64 packetize | `BM_MoldPacketize`: the whole feed into packets of at most 1,400 bytes | **107.3 million messages/s** (3.09 GiB/s of feed) |
| MoldUDP64 read | `BM_MoldRead`: those packets back into a feed, with the sequence checks | **94.1 million messages/s** (2.71 GiB/s of feed) |
| Allocations | `lobsim_bench --feed` (counting global `operator new`), 10,000,000 messages, the first 1,000,000 not counted | **0 per message** in encode and in parse; parse + rebuild 0.013 per message (116,801 `std::map` nodes for new price levels, against 116,800 in the engine on the same messages) |

Encode is 1.8 times the engine's throughput on the same machine, so the feed is never the bottleneck. The size row checks by hand: 38 x 171,237 + 33 x 51,653 + 25 x 0 + 21 x 141,489 + 14 x 2 = 11,182,852. The default run has no X: none of its agents ever reduces an order at the same price.

To reproduce:

```sh
build/release/micro_bench --benchmark_filter=Feed --benchmark_min_time=2s
build/release/lobsim_bench --feed
build/release/lobsim_feed encode --run out/run1 --out out/run1/feed.itch
```

## Code layout

| Path | Contents |
|---|---|
| `include/feed/itch.hpp` | constants, `kBodyLen`, the message structs, the `feed::be` byte-order helpers, `write` and `read_*` (header-only) |
| `include/feed/encoder.hpp`, `src/feed/encoder.cpp` | `feed::Encoder`: the mirror of resting orders and the per-request mapping |
| `include/feed/decoder.hpp` | `feed::parse` (a template over the handler, so every call inlines) and `feed::Reencoder` |
| `include/feed/feed_book.hpp`, `src/feed/feed_book.cpp` | `feed::FeedBook` and `feed::book_digest` |
| `include/feed/moldudp64.hpp` | MoldUDP64: `packetize`, `read_packets` (a template over the handler) and `Reassembler` (header-only) |
| `apps/feed_main.cpp` | `lobsim_feed` |
| `tests/unit/feed_*.cpp`, `tests/feed_*.cmake` | unit tests, the closed loop and the canary checks |
| `fuzz/fuzz_feed_parse.cpp`, `fuzz/feed_parse_driver.*`, `fuzz/make_feed_seeds.py`, `fuzz/corpus/feed_parse/` | the fuzz target, its shared check and its seeds |
| `bench/feed_bench.cpp` | the feed microbenchmarks |

The `feed` library depends on `lob` only, never on `sim`, the same rule `lob` follows. It adds no third-party dependency.

## Not included

These are deliberately left out: networking (MoldUDP64 packets exist, but only in files and memory, with no sockets and no retransmission requests), a live feed from inside the simulator, message types other than S, A, E, X and D, a custom file header, and `mmap` or SIMD (a run's feed is a few MB, and a plain read is already memory-bound). No real Nasdaq ITCH file is in the repository, and nothing here claims results on real data.
