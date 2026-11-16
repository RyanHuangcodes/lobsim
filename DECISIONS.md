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
