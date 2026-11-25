# Module 6: How we know it is right

> **In one sentence:** lobsim checks its engine five different ways (hand-written examples, self-checks, bookkeeping, a race against the simple book, and a robot that invents millions of inputs), and then it checks that those checks can actually catch bugs.
>
> **You will be able to:** read and write a GoogleTest test, explain typed tests, the conservation checker, differential testing, coverage-guided fuzzing, injected-bug canaries and sanitizers, and use lobsim's tools to find, reproduce and read a bug report.
>
> **Time:** about 4 hours.

## 6.1 Why so much testing?

A matching engine that is wrong by one tick on one trade in a million is wrong. Worse, that kind of bug does not crash: it quietly gives a slightly different answer, and every result built on top of it is quietly wrong too. So lobsim does not rely on one kind of test. Each kind catches mistakes the others miss.

| Kind | What it checks | Where |
|---|---|---|
| Unit tests | Each rule, as a small example worked out by hand | `tests/unit/*_test.cpp` |
| Invariants | The book's structure is consistent after every message | `validate()` in both books |
| Conservation | Every lot of every order is accounted for | `tests/support/conservation.hpp` |
| Differential test | Fast book = simple book, event for event, on millions of random requests | `tests/fuzz_diff.cpp` |
| Coverage-guided fuzzing | The same comparison on inputs a robot evolves to reach new code | `fuzz/` |
| Canaries | The tests themselves can catch known bugs | `LOBSIM_BUG_*` in `src/lob/order_book.cpp` |
| Sanitizers | No memory errors or undefined behaviour while all of the above run | the `sanitize` preset |

The whole release suite has 670 tests (Module 0 ran them). This module covers the engine's tests; determinism and replay tests come in Modules 7 and 12, real-data tests in Module 13.

## 6.2 A unit test, read slowly

`tests/unit/matching_test.cpp`:

```cpp
template <class B>
class MatchingTest : public ::testing::Test {};
TYPED_TEST_SUITE(MatchingTest, BookTypes);

// Asks: A=id1 5@101 (owner 1), B=id2 3@101 (owner 2), C=id3 4@102 (owner 3). Buyer is owner 9.
template <class H>
void worked_example_asks(H& h) {
  h.submit(limit(1, Sell, 101, 5));
  h.submit(limit(2, Sell, 101, 3));
  h.submit(limit(3, Sell, 102, 4));
}

TYPED_TEST(MatchingTest, WorkedExampleLimit101_FillsAThenBThenRestsRemainder) {
  BookHarness<TypeParam> h;
  worked_example_asks(h);
  h.submit(limit(9, Buy, 101, 10));
  EXPECT_EQ(h.last(), (Events{ack(4, 9),
                              trade(1, 101, 5, 1, 4, 1, 9, Buy),
                              trade(2, 101, 3, 2, 4, 2, 9, Buy)}));
  EXPECT_EQ(h.book().best_bid(), 101);
  EXPECT_EQ(h.book().best_level(Buy), (LevelView{101, 2}));
  ...
}
```

- **GoogleTest** is a testing library. `EXPECT_EQ(a, b)` checks that `a == b` and, if not, prints both and marks the test failed (it keeps going; `ASSERT_EQ` would stop the test).
- `limit(1, Sell, 101, 5)`, `ack(4, 9)` and `trade(...)` are small helpers from `tests/support/book_harness.hpp` that build requests and the exact events expected. The test compares the **whole list** of events, every field, thanks to the `operator== = default` you met in Module 3.
- **Typed tests.** `TYPED_TEST_SUITE(MatchingTest, BookTypes)` with `BookTypes = ::testing::Types<lob::ReferenceBook, lob::OrderBook>` (in `tests/unit/book_types.hpp`) makes GoogleTest compile every `TYPED_TEST` twice, once with `TypeParam = ReferenceBook` and once with `TypeParam = OrderBook`. You write each rule once and both books must pass it. In the output, `MatchingTest/0` is the reference book and `MatchingTest/1` the fast book.

### The harness checks more than you asked

`BookHarness::send` (in `book_harness.hpp`) runs after every request:

```cpp
    try {
      book_.validate();
    } catch (const std::logic_error& e) {
      ADD_FAILURE() << "validate() threw: " << e.what();
    }
    auto violations = checker_.check(req, out, [this](OrderId id) { return book_.order(id); });
    for (const std::string& v : violations) ADD_FAILURE() << "conservation: " << v;
```

So every unit test also checks every invariant (Module 5.9) and quantity conservation after every single message, even though the test only mentions the events.

## 6.3 Conservation: follow every lot

SPEC.md 3.8: for every order, *filled + cancelled + resting = accepted quantity*, adjusted at each modify. The `ConservationChecker` keeps its **own** record of each order's remaining quantity, using only the requests and the events:

| Event | What the checker does |
|---|---|
| `Ack` | remaining = the request's quantity |
| `Trade` | remaining −= qty, for the maker and for the taker |
| `Cancelled` | `qty_cancelled` must equal remaining; remaining = 0 |
| `Modified` | remaining = `new_qty` |

Then it asks the book about every order it knows: remaining 0 means the book must not hold it, remaining > 0 means the book must report exactly that. A book that "forgets" one lot, or double-counts a fill, is caught on the message where it happens.

## 6.4 Differential testing: a race between two engines

`tests/fuzz_diff.cpp` generates a stream of random requests from a seed: valid and invalid, every order type, few owners (so self-trades happen often), cancels and modifies of random ids (many of them unknown), prices in a small range (so orders cross often). It feeds the stream to both books and, after every message, compares:

- the two event lists, field for field;
- `validate()` on both;
- conservation;
- every few messages, the query results (best prices, depth, orders).

The default run is 200 seeds × 10,000 messages (CTest splits it into 10 entries so they run in parallel); `--long` runs 4 × 1,000,000.

### Testing the test

A random generator might, by bad luck, never produce some tricky case. So `fuzz_diff` counts what happened and **fails if any event type, reject reason, cancel reason or self-trade path never occurred**. Run one seed and look at the end of its output:

```sh
build/release/fuzz_diff --seed 1 --messages 10000 | tail -25
```

You will see a table of counts such as `STP CancelResting, FOK execution`, `book emptied (resting -> 0)` and `max resting orders`. A test that silently stopped reaching a case would otherwise keep passing for the wrong reason.

The generator draws its random numbers with SplitMix64 and never draws two random values inside one function call's argument list (DECISIONS.md D-047). Module 7 explains why that matters.

## 6.5 Coverage-guided fuzzing

A random generator throws darts blindfolded. A **coverage-guided fuzzer** (lobsim uses libFuzzer, part of LLVM Clang) watches which lines and branches of the code each input reaches. When an input reaches something new, it keeps that input and makes small changes to it (flip a byte, copy a piece, join two inputs) to try to go deeper. Rare paths, such as a fill-or-kill order meeting self-trade prevention during a modify, are found far faster this way.

The fuzzer only produces bytes. `fuzz/diff_driver.cpp` turns every 8 bytes into one request (FUZZING.md has the table):

```cpp
Fields decode_fields(const std::uint8_t* rec, const std::vector<OrderId>& known_ids) {
  Fields f;
  f.owner = 1 + rec[1] % 3;                       // only 3 owners: self-trades are common
  f.flags = rec[2];                                // side, post-only, STP mode, order type, ...
  const auto p16 = static_cast<std::int16_t>(static_cast<std::uint16_t>(rec[3] | (rec[4] << 8)));
  const bool raw_price = (f.flags & 0x20) != 0;
  f.price = raw_price ? static_cast<Price>(p16) : 95 + static_cast<Price>(static_cast<std::uint16_t>(p16) % 11);
  ...
```

Most prices land in the band 95 to 105 so that orders meet; a flag bit lets raw values through (0, negatives, above `max_price`) so the rejection paths get tested too. Cancel and modify targets are mostly picked from ids the book has acknowledged, so they often hit real orders.

The check itself, `run_diff`, applies each request to a fresh `OrderBook` and a fresh `ReferenceBook` and compares exactly what `tests/fuzz_diff.cpp` compares, plus depth and `order()` of the target. Any difference prints a report and calls `std::abort()`, which libFuzzer records as a crash and saves the input to a file.

`fuzz/fuzz_diff.cpp`, the whole fuzz target:

```cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lob::fuzz::run_diff(data, size);
  return 0;
}
```

libFuzzer calls this function millions of times with different bytes. **Skipped on purpose:** the other three targets follow the same pattern: `fuzz_diff_builder.cpp` (the book-builder operations of Module 13), `fuzz_replay_parse.cpp` (the `orders.log` reader of Module 12: any bytes must either be rejected cleanly or read, written back and read again identically) and `fuzz_lobster_parse.cpp` (the LOBSTER line readers).

### When the fuzzer finds a crash (FUZZING.md)

1. Read it: `lobsim_fuzz_decode <file>` prints each request and the events.
2. Shrink it with libFuzzer's `-minimize_crash=1`.
3. Decide which book is wrong **using the rules in SPEC.md**, not by assuming the fast one is.
4. Save the small input to `fuzz/regressions/diff/`, where the regression test runs it forever after.
5. Add a hand-written unit test for the same case.

## 6.6 Canaries: does the alarm actually ring?

Miners used to take a canary underground: if gas built up, the bird showed it first. lobsim plants three **deliberate bugs** in `src/lob/order_book.cpp`, each behind a switch:

| Switch | The bug |
|---|---|
| `LOBSIM_BUG_TAKER_PRICE` | trades print at the taker's limit instead of the maker's price |
| `LOBSIM_BUG_MODIFY_KEEPS_PRIORITY` | a same-price quantity increase keeps its place in line |
| `LOBSIM_BUG_FOK_IGNORES_STP` | the fill-or-kill dry run counts the taker's own orders |

You saw the first one in Module 5:

```cpp
        Price trade_price = level.price;
#ifdef LOBSIM_BUG_TAKER_PRICE
        if (!t.is_market) trade_price = t.limit;  // injected bug: print at the taker's limit
#endif
```

CI builds the fuzzer once per bug and **fails unless the fuzzer finds it**, both from the committed seed inputs and from no seeds at all. FUZZING.md records each one being caught in under a second (about 3,000 attempts without seeds). If someone later weakens the checks so they can no longer see these bugs, CI goes red. A guard stops anyone building a bug in by accident:

```cpp
#if (defined(LOBSIM_BUG_TAKER_PRICE) || defined(LOBSIM_BUG_MODIFY_KEEPS_PRIORITY) || \
     defined(LOBSIM_BUG_FOK_IGNORES_STP)) && !defined(LOBSIM_FUZZ_BUILD)
#error "LOBSIM_BUG_* injects a matching bug and is only allowed in a LOBSIM_FUZZ build"
#endif
```

`#error` makes compilation fail with that message. This idea, deliberately breaking code to check that tests notice, is called **mutation testing**. The README notes that "several tests were strengthened after injected bugs showed they could not see the failure they were written for".

## 6.7 Sanitizers and coverage

**AddressSanitizer** (ASan) adds checks around every memory access: reading freed or poisoned memory, writing past the end of an array, leaks. **UndefinedBehaviorSanitizer** (UBSan) catches undefined behaviour such as signed overflow. The `sanitize` preset builds everything with both, and with `-fno-sanitize-recover=all` so that the first finding stops the program and fails the test. You used it in Lab 5.3.

**Line coverage** measures which lines the tests ran. The README reports 99.4% for `src/lob`: 3 of 536 lines never run, all of them failure branches of `validate()` that a healthy book cannot reach. 100% coverage does not mean "no bugs", but an unrun line is a line nobody has tested.

## Lab 6.1: run and read tests

```sh
build/release/lob_tests --gtest_list_tests | head -20
build/release/lob_tests --gtest_filter='MatchingTest*WorkedExample*'
```

The second command runs four tests: two examples × two books. Then open `tests/unit/stp_test.cpp`, pick one test, and before reading its `EXPECT_EQ` lines, write down the events you expect from its requests. Check yourself against the test.

## Lab 6.2: write your own test

Add this at the end of `tests/unit/matching_test.cpp`:

```cpp
// Lab 6.2: my first test. A sell that crosses two bid levels and rests what is left.
TYPED_TEST(MatchingTest, MyFirstTest_SellSweepsTwoBidLevelsThenRestsTheRest) {
  BookHarness<TypeParam> h;
  h.submit(limit(1, Buy, 100, 2));  // id1
  h.submit(limit(2, Buy, 99, 3));   // id2
  h.submit(limit(9, Sell, 99, 7));  // id3: 2 @ 100, then 3 @ 99, then 2 rest at 99
  EXPECT_EQ(h.last(), (Events{ack(3, 9),
                              trade(1, 100, 2, 1, 3, 1, 9, Sell),
                              trade(2, 99, 3, 2, 3, 2, 9, Sell)}));
  EXPECT_EQ(h.book().best_bid(), std::nullopt);
  EXPECT_EQ(h.book().best_level(Sell), (LevelView{99, 2}));
  EXPECT_EQ(h.book().order(3), (OrderView{3, 9, Sell, 99, 2, STP::CancelTaking, false}));
}
```

Build and run only your test:

```sh
cmake --build --preset release -j --target lob_tests
build/release/lob_tests --gtest_filter='*MyFirstTest*'
```

Expected: `[  PASSED  ] 2 tests.` (one per book). Now make the test wrong on purpose (change the second trade's price to 100) and run it again: read how GoogleTest reports the difference. Then write a second test of your own for a rule from Module 2 that you find tricky. When you are done, remove your tests with `git checkout -- tests/unit/matching_test.cpp`.

## Lab 6.3: break the engine and watch the alarms

```sh
git apply course/labs/patches/lab06_taker_price_bug.patch     # trades print at the taker's limit
git diff
cmake --build --preset release -j --target lob_tests fuzz_diff
build/release/lob_tests --gtest_brief=1 | tail -3
build/release/fuzz_diff --seed 1 --messages 10000 | head -20
```

Predict before running: how many unit tests fail, and for which book? When this course was written, 15 tests failed, every one of them with `TypeParam = lob::OrderBook` (the patch only touches the fast book), and `fuzz_diff` stopped at the 46th message of seed 1:

```
MISMATCH seed=1 message=45: event lists differ
  request: NewOrder{owner=4, client_order_id=46, side=Sell, type=Limit, tif=IOC, price=1, qty=83, post_only=false, stp=CancelResting}
  OrderBook events:
    Ack{order_id=28, owner=4, client_order_id=46}
    Trade{trade_id=2, price=1, qty=1, maker_order_id=24, ...}
    ...
  ReferenceBook events:
    Ack{order_id=28, owner=4, client_order_id=46}
    Trade{trade_id=2, price=12, qty=1, maker_order_id=24, ...}
    ...
  reproduce: fuzz_diff --seed 1 --messages 46
```

Read the report like a detective: a sell IOC with limit 1 traded against a bid. Which price is right according to Module 2, 1 or 12? Which book is wrong? Then run the `reproduce` command it suggests and check you get the same report from just 46 messages.

Undo the bug and rebuild:

```sh
git apply -R course/labs/patches/lab06_taker_price_bug.patch
git diff --stat                                      # prints nothing when the file is restored
cmake --build --preset release -j --target lob_tests fuzz_diff
```

<details><summary>Answer</summary>

12. The trade must print at the maker's price (the resting bid at 12), not at the seller's limit of 1. The `OrderBook` is wrong, as expected, since the patch put the bug there.

</details>

## Lab 6.4: speak the fuzzer's language

Make the two 8-byte records of FUZZING.md's worked example by hand and decode them:

```sh
python3 -c "open('out/two.bin','wb').write(bytes.fromhex('0000006400050000' '0001016400030000'))"
build/release/fuzz/lobsim_fuzz_decode out/two.bin
```

Expected:

```
#0 New owner=1 side=Buy type=Limit tif=GTC price=96 qty=6 post_only=0 stp=CancelTaking
    Ack{order_id=1, owner=1, client_order_id=0}
#1 New owner=2 side=Sell type=Limit tif=GTC price=96 qty=4 post_only=0 stp=CancelTaking
    Ack{order_id=2, owner=2, client_order_id=1}
    Trade{trade_id=1, price=96, qty=4, maker_order_id=1, taker_order_id=2, maker_owner=1, taker_owner=2, aggressor_side=Sell}
books agree: 2 requests
```

Using the table in FUZZING.md, work out by hand why the first record means "owner 1, buy, price 96, quantity 6" (hint: price = 95 + (100 % 11), quantity = 1 + (5 % 10)). After these two records, 2 lots of the buy are still resting at 96. Write a third record that makes owner 3 send a **market sell** for 2 lots, which should trade with them; add it to the file and decode again. Finally, decode a committed seed: `build/release/fuzz/lobsim_fuzz_decode fuzz/corpus/diff/fok_stp_cancel_taking.bin`.

<details><summary>One possible third record</summary>

`00 02 19 00 00 01 00 00`: kind `0 % 3 = 0` (New), owner `1 + 2 % 3 = 3`, flags `0x19` = bits 0, 3 and 4 set = side Sell (bit 0) and order type 3, Market/IOC (bits 3 and 4), quantity `1 + 1 % 10 = 2`. Write all three with `bytes.fromhex('0000006400050000' '0001016400030000' '0002190000010000')`. The decoder shows `Trade{trade_id=2, price=96, qty=2, maker_order_id=1, taker_order_id=3, ...}`. With flags `0x18` instead (bit 0 clear: a market **buy**), you get `Cancelled(NoLiquidity)`: there are no sellers left.

</details>

## Lab 6.5 (optional, not run while writing this course): the real fuzzer

This needs LLVM Clang with libFuzzer (on Ubuntu `clang-18` and `libclang-rt-18-dev`; on macOS Homebrew `llvm`; Apple's Xcode Clang does not include libFuzzer). The sandbox used to write this course had no libFuzzer runtime, so these steps come from FUZZING.md and were not run here.

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DLOBSIM_FUZZ=ON
cmake --build build-fuzz -j
mkdir -p fuzz-work/diff fuzz-artifacts
./build-fuzz/fuzz/fuzz_diff fuzz-work/diff fuzz/corpus/diff -max_len=4096 -max_total_time=60 \
  -artifact_prefix=fuzz-artifacts/ -print_final_stats=1
```

Then build a canary (`-DLOBSIM_INJECT_BUG=LOBSIM_BUG_MODIFY_KEEPS_PRIORITY` in a separate build folder) and fuzz it from an **empty** folder. FUZZING.md reports each canary found in under a second from nothing. Decode the crash file with `lobsim_fuzz_decode` from the normal build: it says `books agree`, because the bug exists only in the canary build.

## Common questions

**If the two books are compared all the time, why also write unit tests?** Both books could share a misunderstanding of the rules; comparing them would never notice. Unit tests are written from the spec by a person, so they check the *rules*, not just agreement.

**Why does the fuzzer only use 3 owners and prices from 95 to 105?** To make interesting things likely: with few owners, self-trades are frequent; with a narrow price band, orders cross and queue at the same prices.

**What does it mean that a fuzz crash "decodes to books agree"?** The input made the canary build fail, but in a normal build both books agree on it. That is exactly what should happen for an injected bug.

**Can tests prove there are no bugs?** No. They can only show bugs that exist. That is why lobsim layers many kinds of tests and checks that each kind can catch something.

**Why `-fno-sanitize-recover=all`?** By default some sanitizer checks print a warning and continue. Recovering would let a test "pass" with an error in its log. Making every finding fatal makes it impossible to ignore.

## Quiz

1. What does `TYPED_TEST` give you that `TEST` does not, here?
2. Name three things `BookHarness` checks after every request, apart from what the test itself checks.
3. In the differential test, what decides whether the fast book is right?
4. Why does `fuzz_diff` fail if some reject reason never occurred?
5. What is a canary, and what does CI do with the three in lobsim?
6. Which kind of bug can AddressSanitizer find that a differential test can miss? Give the example from Module 5.

<details><summary>Answers</summary>

1. The same test compiled and run for both `ReferenceBook` and `OrderBook`.
2. `validate()` (the invariants), quantity conservation, and (implicitly) that no exception escapes.
3. Agreement with `ReferenceBook` on every event and query, and when they disagree, the rules of SPEC.md decide which one is wrong.
4. Because then the random stream did not test that path at all; a pass would say nothing about it.
5. A deliberately injected bug, compiled only in fuzz builds. CI requires the fuzzer to find each of them (from the seeds and from an empty start), so a weakened check makes CI fail.
6. A memory error that happens to give the right answer: reading `m->next` after the order was released (Lab 5.3). Both books produced the same events, but the fast book read freed memory.

</details>

## What's next

The engine is done. Modules 7 to 11 build the simulated market around it, starting with the most surprising ingredient: random numbers that are exactly the same every time.
