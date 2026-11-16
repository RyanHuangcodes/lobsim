# DECISIONS

One line per decision, with the reason. Section numbers refer to `SPEC.md`.

## Tooling and build
- D-001 CMake comes from a project-local `.venv` (`uv pip install cmake`), not a system install: the machine had no CMake, and a local venv changes nothing outside the repo. The same venv holds pandas + matplotlib for `tools/plot.py`.
- D-002 `CMakePresets.json` defines `debug`, `release` and `sanitize` (build dirs `build/<name>`): one short, identical command per configuration for every contributor and helper.
- D-005 The presets file uses `"version": 3`, which needs CMake 3.21+: version 2 makes `generator` mandatory and we pin none. On exactly CMake 3.20 the project still builds with plain `cmake -S . -B <dir> -DCMAKE_BUILD_TYPE=...`.
- D-003 Sanitize = Debug + `-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer`, applied globally so the dependencies are instrumented too: UBSan findings must fail the test run, not just print, and stack traces must be readable.
- D-004 `BENCHMARK_ENABLE_WERROR OFF` and third-party targets treated as SYSTEM: "zero warnings" applies to our code; a newer compiler must not break a pinned dependency.
- D-006 `LOBSIM_WERROR` (default ON) adds `-Werror` to our own targets: "zero warnings" (Section 8) is then enforced by every build instead of by reading logs.
- D-007 With no `CMAKE_BUILD_TYPE` the build defaults to Release: a bare `cmake -S . -B build` should give the fast binaries the benchmarks describe.

## Matching engine API (Section 3 is silent on exact signatures)
- D-010 Both books expose the same non-virtual member API (`submit`, `cancel`, `modify`, `best_bid`, `best_ask`, `best_level`, `depth`, `order`, `resting_orders`, `validate`) and tests are written once as typed tests over both: a virtual base would add an indirect call to the hot path for no benefit.
- D-011 `Event` is a `std::variant<Ack, Reject, Trade, Cancelled, Modified>` appended to a caller-owned `std::vector<Event>`: trivially copyable alternatives, no allocation once the buffer has grown (Section 3.7).
- D-012 `Request` is a `std::variant<NewOrder, Cancel, Modify>` with a free `apply(book, request, out)`: fuzz, replay, bench and the exchange all feed the same stream type.
- D-013 `best_level(side)` returns the best `(price, total_qty)` without allocating: the exchange needs BBO quantities after every message and `depth()` returns a vector.
- D-014 `validate()` throws `std::logic_error` naming the first broken invariant; Debug builds call it inside the book after every message: a thrown error is testable and gives a message, an `assert` does neither.
- D-015 Event hash encoding: one tag byte (Ack 0, Reject 1, Trade 2, Cancelled 3, Modified 4), then every field in declaration order, 64-bit fields as 8 bytes LE, 32-bit as 4 bytes LE, enums and bools as 1 byte: fixed and platform-independent (Section 3.7).

## Matching semantics (cases Section 3 leaves open)
- D-020 `UnknownOrder` means "not resting right now": never-assigned, fully filled and already-cancelled ids are indistinguishable to the caller, and the id index only knows resting orders.
- D-021 Cancel reason for a market order: `SelfTrade` if STP `CancelTaking` stopped it (even with zero fills), else `NoLiquidity` if nothing filled, else `IOCRemainder`. A limit IOC remainder is always `IOCRemainder`, also with zero fills: Section 3.3.3 reserves `NoLiquidity` for market orders.
- D-022 A market order whose only opposite liquidity was its own orders cancelled under `CancelResting` ends with `NoLiquidity`: nothing filled.
- D-023 Post-only "would cross" is a pure price test against the opposite best, whoever owns it: Section 3.3.5 defines it by price only.
- D-024 A modify that loses priority re-enters as a GTC taker using the order's stored `stp` and `post_only`; under `CancelTaking` a self-match on re-entry cancels the re-entering order's remainder with `Cancelled(SelfTrade)`: Section 3.3.6 says it re-enters "as a new arrival".
- D-025 Same price with a larger qty moves the order to the tail of its own level (`priority_kept = false`); it cannot match because the book is never crossed.
- D-026 `Modified.new_qty` is the requested new remaining quantity, before any matching the re-entry causes: the event is emitted first (Section 3.3.6).
- D-027 The modify `PostOnlyWouldCross` check applies only to post-only orders; a normal order modified to a crossing price trades as a taker.
- D-028 `Cancelled.qty_cancelled` is the order's remaining quantity at that moment; `Trade.aggressor_side` is the taker's side.
- D-029 FOK is valid only on limit orders (a market order must be IOC, Section 3.2), and a FOK that passes the dry run always fills completely, because execution walks the same orders the dry run counted.
- D-030 A market, limit IOC or limit GTC taker stopped by `CancelTaking` gets `Cancelled(SelfTrade)` for its remainder: Section 3.4 states it without exception, so D-021's "limit IOC remainder is always IOCRemainder" applies only when STP did not stop it. (A FOK can never end this way: it either fails the dry run or fills before reaching its own order, D-029.)

## OrderBook internals and the differential fuzz test (Phase 3; Section 3.5 is silent on these)
- D-040 `ObjectPool` keeps its free list in a separate `std::vector<void*>`, not as links inside free slots, and reserves that vector to the pool's full capacity whenever a chunk is added: a released slot is ASan-poisoned and must never be read, and `destroy()` then never allocates.
- D-041 The pool hands out the most recently released slot first (LIFO) and uses chunks of 4096 slots: the reused slot is still in cache, and one chunk of 64-byte orders (256 KiB) covers a typical book without growing.
- D-042 `ObjectPool<T>` requires a trivially destructible `T` (static_assert): the pool does not track which slots are live, so it cannot run destructors when it dies; `Order` is trivially destructible.
- D-043 A modify that loses priority returns the old `Order` node to the pool and takes a fresh one if the remainder rests (same `OrderId`, same index slot): one removal path for cancel, fill and modify, and no node that is both resting and the taker during a match.
- D-044 The id index grows (geometrically) only when an order rests; lookups bounds-check, so any id beyond it is `UnknownOrder`. `validate()` checks every resting order's own slot and compares `indexed_`, the number of non-null slots kept by the index's single writer `set_index()`, with the number of resting orders, instead of scanning the whole index: the index grows with every id ever rested, which would make a per-message `validate()` O(total ids). It also checks pool live count == resting orders (catches a leaked node).
- D-045 `OrderBook` is neither copyable nor movable: orders point at map nodes and pool slots, so a copied book would alias the original's orders.
- D-046 `ReferenceBook::validate()` finds duplicate ids by sorting a copy (O(n log n)) instead of comparing every pair: same check, fast enough to run twice per fuzz message.
- D-047 The fuzz generator uses SplitMix64 (`tests/support/splitmix64.hpp`) with rejection-sampled ranges, and never draws two random values inside one function-call argument list (unspecified evaluation order): the same seed gives the same stream on any compiler.
- D-048 `ConservationChecker::check_fast()` gives `check()`'s verdict in O(open + touched) per message by comparing the book's `resting_orders()` with the model's live count instead of re-querying every finished id. The fuzz test runs it on every message; on every 20th seed `check()` runs beside it and the verdicts must agree; `--long` uses `check_fast()` only, since `check()` is quadratic over 10^6 messages.
- D-049 The default fuzz mode is registered as 10 ctest entries of 20 seeds each, and the test presets run 10 jobs: the sanitize build takes several minutes serially.
- D-050 Adding an order to an existing price level costs one `std::map` lookup, O(log L), not the O(1) of Section 3.5's target: 3.5 also fixes the level container as `std::map`, which has no O(1) lookup by price, and an incoming order carries no pointer to its level. Only the best level is O(1) (`begin()`). L is tens of levels in the simulator, so this is a handful of comparisons; an O(1) price index is the array-ladder stretch goal of Section 10.
- D-051 `tests/unit/order_book_validate_test.cpp` also holds a deep-book differential test (8,500 resting orders, three pool chunks, scattered cancels, slot reuse, full drain) with an independent scan of the whole id index: the fuzz test keeps the book under about 500 orders, so it never leaves the first pool chunk.

## Benchmarks (Phase 4)
- D-060 The benchmark stream measures order lifetimes in new-order arrivals (exponential, mean 1,500), not in time: the bench has no clock, and this holds the book near 1,000 resting orders like the simulator's noise flow.
- D-061 `lobsim_bench` makes three passes over the same stream: timed per block of 4,096 messages (throughput), per batch of 32 (body of the distribution) and per message (far tail and maximum). On the benchmark machine the clock advances in 41 ns steps and a message takes about 21 ns, so single-message samples cannot resolve the body (Section 7: "time batches if it's significant").
- D-062 The allocation counter is on only while engine calls run, and the caller's event buffer is reserved once: every counted allocation is then the engine's own.
- D-063 Each microbenchmark times a batch of 128 operations and undoes it outside the timed region: pausing the timer per operation costs more than the operation, and the book must stay near its nominal size.
- D-064 The harness fails (non-zero exit) if its passes disagree on the event count, if an order id differs from the predicted one, or if `OrderBook` and `ReferenceBook` hashes differ on the shared prefix: a benchmark of a broken run is worse than none.
