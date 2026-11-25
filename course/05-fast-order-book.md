# Module 5: The fast book

> **In one sentence:** `OrderBook` follows exactly the same rules as `ReferenceBook`, but it arranges memory so that every common job touches only the few orders it needs, which makes it hundreds to thousands of times faster on a big book.
>
> **You will be able to:** explain Big-O with examples, explain pointers and why a pointer can "dangle", draw lobsim's book as maps, levels, linked lists, a pool and an index, read `match_in`, `unlink` and `rest` line by line, list the invariants `validate()` checks, and catch a memory bug with a sanitizer.
>
> **Time:** about 5 hours, in two sittings. This is the hardest code module.

## 5.1 Big-O: how the work grows

Looking for "Smith" in a phone book:

- Read every name from the first page until you find it. A book twice as thick takes twice as long. That is **O(n)**, "linear".
- Open it in the middle, decide which half Smith is in, open the middle of that half, and so on. Doubling the book adds just **one** more step. That is **O(log n)**, "logarithmic". A million names take about 20 steps.
- Someone tells you "page 412". One step, however thick the book. That is **O(1)**, "constant".

Big-O ignores fixed costs and only describes how the work **grows** with the size of the job. `ReferenceBook` is O(N) for almost everything (N = resting orders): it reads the whole list. `OrderBook`'s targets, from SPEC.md 3.5: cancel O(1) (plus removing a price level if it empties), add O(log L) for a new price level and O(1) to the back of an existing one once you have it (L = number of price levels), best price O(1).

Real computers add a twist: memory that was used recently is much faster to read (it sits in the processor's **cache**). That is why the fast book also avoids asking the operating system for new memory on every order. Lab 5.1 shows both effects.

## 5.2 Memory and pointers

A computer's memory is a very long street of numbered houses, one byte per house. Every value lives at some **address**. A **pointer** is a value that holds an address: "the order you want lives at house 0x7f0e...830".

```cpp
Order* o = index_[id];   // o holds the address of an Order (or nullptr: "no address")
o->remaining -= qty;     // -> means "go to that address and use this field"
```

- `Order*` is the type "pointer to an `Order`".
- `nullptr` is the pointer that points nowhere. Always check for it before following a pointer.
- `->` follows the pointer and reads a field: `o->remaining`.
- `&x` gives the address of `x`: `o->level = &level;`.

Pointers are fast because following one is a single step: no searching. They are dangerous because nothing stops a pointer from holding the address of a house that has since been demolished, or given to someone else. Following such a **dangling pointer** reads garbage, or, worse, someone else's data that looks fine. Most of the care in this module is about never doing that.

## 5.3 The design in one picture

After the worked example (asks A 5 @ 101 and B 3 @ 101, C 4 @ 102, then a buy of 10 @ 101 that left 2 resting), the fast book looks like this:

```
 asks_ : std::map<Price, Level>                    bids_ : std::map<Price, Level, std::greater<Price>>
   begin() -> [102] Level{total 4, count 1}          begin() -> [101] Level{total 2, count 1}
                head --> Order#3 <-- tail                         head --> Order#4 <-- tail
                         (C, 4 left)                                       (buyer, 2 left)

 Orders live in the pool (chunks of 4096 slots that never move):
   [ Order#3 | Order#4 | free | free | ... ]

 index_ : std::vector<Order*>, by order id
   [0]=null [1]=null [2]=null [3]=&Order#3 [4]=&Order#4
```

Four pieces, each chosen for one job.

### Piece 1: a sorted map of price levels per side

```cpp
using Bids = std::map<Price, Level, std::greater<Price>>;
using Asks = std::map<Price, Level>;
```

A `std::map` keeps its entries sorted by key at all times (inside, it is a balanced tree). Finding, adding or removing a key is O(log L). `begin()` is the first entry: for asks sorted from low to high, that is the **lowest** ask; for bids, `std::greater<Price>` reverses the order, so `begin()` is the **highest** bid. Either way, `begin()` is the best price, in one step.

One more property is essential: a map's entries **never move in memory** while they exist, even when other entries are added or removed. So an order may safely keep a pointer to its level.

### Piece 2: a level is a queue of orders, linked by hand

```cpp
struct Level {
  Price price;
  Qty total_qty;
  std::uint32_t count;
  Order* head;   // first in line (oldest)
  Order* tail;   // last in line (newest)
};
struct Order {
  OrderId id; OwnerId owner; Side side; Price price; Qty remaining; STP stp; bool post_only;
  Order* prev;   // the order in front of me in the queue
  Order* next;   // the order behind me
  Level* level;  // the level I am in
};
```

Each level is a **doubly linked list**: a conga line where every person holds the shoulder of the person in front (`prev`) and is held by the person behind (`next`). The level remembers the first (`head`) and last (`tail`) person. Joining the back is O(1). Leaving from anywhere is also O(1): the people on either side of you just hold each other instead. A plain array would have to shuffle everyone behind you.

"Intrusive" means the links live inside the `Order` itself, so no extra memory is needed for list nodes.

The level also keeps `total_qty` and `count` up to date at every change, so "how much is at this price?" never needs a walk.

### Piece 3: a pool of order slots

Orders are not created with the system's general-purpose memory allocator (`new`) one by one. They come from an `ObjectPool<Order>` (`include/lob/pool.hpp`): memory is taken in **chunks of 4,096 order slots**, and a slot that is freed goes onto a **free list** to be handed out again. Two reasons:

- **Speed.** Asking the operating system for memory is slow compared with taking a slot from a list. After the pool has grown, adding an order allocates nothing.
- **Addresses never move.** If orders were stored in a `std::vector<Order>`, the vector would sometimes grow by copying everything to a bigger block, and every `Order*` in the book (in lists, in the index) would dangle at once. SPEC.md 3.5 forbids exactly that. A chunk is never moved or freed until the book is destroyed.

### Piece 4: an index from order id to order

```cpp
std::vector<Order*> index_;  // by OrderId; nullptr unless that order is resting
```

Order ids are handed out 1, 2, 3, ..., with no gaps, so a plain vector indexed by id works as a lookup table: `index_[17]` is order 17, or `nullptr` if it is not resting. Cancel and modify find their order in O(1). SPEC.md chose this over a hash table (`std::unordered_map`) because a hash table allocates a node for every insert. The price: the vector is as long as the highest id ever rested, and when it is full it doubles by copying (Module 12 shows that copy as the slowest message of a 10-million-message benchmark).

## 5.4 Read the real code: putting an order in

`src/lob/order_book.cpp`, line 72:

```cpp
template <class SideMap>
void OrderBook::append(SideMap& side, Order* o) {
  Level& level = side.try_emplace(o->price, Level{o->price, 0, 0, nullptr, nullptr}).first->second;
  o->level = &level;  // map nodes never move, so this stays valid until the level is erased
  o->prev = level.tail;
  o->next = nullptr;
  (level.tail != nullptr ? level.tail->next : level.head) = o;
  level.tail = o;
  level.total_qty += o->remaining;
  ++level.count;
}
```

- `try_emplace(price, Level{...})` finds the level at this price, or creates an empty one if there is none. It returns a pair whose `.first` points at the map entry; `->second` is the `Level`. This is the O(log L) step.
- The new order goes to the **back** of the line: its `prev` is the old tail, its `next` is nobody.
- `(level.tail != nullptr ? level.tail->next : level.head) = o;` is a compact trick. It picks *which variable to assign*: if the line already had a tail, that tail's `next` now points to the newcomer; if the line was empty, the newcomer becomes the `head`. Then it becomes the new `tail`.
- The level's totals are updated.

`rest` (line 85) wraps it:

```cpp
void OrderBook::rest(const Taker& t, bool post_only) {
  if (t.id >= index_.size()) index_.resize(std::max<std::size_t>(t.id + 1, index_.size() * 2));
  Order* o = pool_.create(
      Order{t.id, t.owner, t.side, t.limit, t.remaining, t.stp, post_only, nullptr, nullptr, nullptr});
  if (t.side == Side::Buy) append(bids_, o);
  else append(asks_, o);
  set_index(t.id, o);
}
```

Grow the index if this id does not fit (at least doubling, so growth is rare), take a slot from the pool, put the order at the back of its level, and record it in the index.

## 5.5 Read the real code: taking an order out

Line 42:

```cpp
void OrderBook::unlink(Order* o) noexcept {
  Level* level = o->level;
  (o->prev != nullptr ? o->prev->next : level->head) = o->next;
  (o->next != nullptr ? o->next->prev : level->tail) = o->prev;
  level->total_qty -= o->remaining;
  --level->count;
  o->prev = nullptr;
  o->next = nullptr;
  o->level = nullptr;
}
```

The same trick twice: whoever pointed forward to `o` (the order in front, or the level's `head` if `o` was first) now points to the order behind `o`; whoever pointed back to `o` (the order behind, or the `tail`) now points to the order in front. Four cases (first, last, middle, only) handled by two lines. Draw them on paper with three boxes; it is worth it.

Line 53 and 58:

```cpp
void OrderBook::release(Order* o) noexcept {
  set_index(o->id, nullptr);
  pool_.destroy(o);  // `o` dangles from here on
}

void OrderBook::remove_resting(Order* o) {
  Level* level = o->level;
  const Side side = o->side;
  const Price price = level->price;
  unlink(o);
  release(o);
  if (level->count == 0) {
    // No order points at the level any more, so erasing its map node is safe.
    if (side == Side::Buy) bids_.erase(price);
    else asks_.erase(price);
  }
}
```

Read the comments: the author marks the exact moment a pointer starts to dangle. `remove_resting` copies `side` and `price` **before** `release`, because after it, reading `o->side` would read a slot that belongs to the pool again. An empty level is erased from the map (SPEC.md: "Delete a level when it becomes empty"), and only when no order points at it.

`do_cancel` (line 233) is then four lines: find the order through the index, check the owner, emit `Cancelled(User)`, `remove_resting`.

## 5.6 Read the real code: matching

Line 128. This is the busiest function in all of lobsim.

```cpp
template <class Makers>
OrderBook::MatchResult OrderBook::match_in(Makers& makers, Taker& t, EventBuffer& out) {
  MatchResult result;
  while (t.remaining > 0 && !makers.empty()) {
    auto it = makers.begin();
    Level& level = it->second;
    if (!taker_crosses(t, level.price)) break;

    Order* m = level.head;
    while (m != nullptr && t.remaining > 0) {
      Order* const next = m->next;
      if (m->owner == t.owner) {
        if (t.stp == STP::CancelTaking) {
          result.stopped_by_stp = true;
          break;
        }
        out.push_back(Cancelled{m->id, m->owner, m->remaining, CancelReason::SelfTrade});
        unlink(m);
        release(m);
      } else {
        const Qty qty = std::min(t.remaining, m->remaining);
        Price trade_price = level.price;
        ...
        out.push_back(Trade{next_trade_id_++, trade_price, qty, m->id, t.id, m->owner, t.owner, t.side});
        t.remaining -= qty;
        m->remaining -= qty;
        level.total_qty -= qty;
        result.filled += qty;
        if (m->remaining == 0) {
          unlink(m);
          release(m);
        }
      }
      m = next;
    }

    if (level.count == 0) makers.erase(it);
    if (result.stopped_by_stp) break;
  }
  return result;
}
```

Compare it with `ReferenceBook::match` from Module 4. The decisions are identical: best price first, earliest first, maker's price (`level.price`), STP by the taker's mode. What changed is *how the next maker is found*:

- **Outer loop:** the best level is always `makers.begin()`. If its price does not cross, stop: every other level is worse.
- **Inner loop:** walk the level's line from `head` to the back. No searching at all.
- `Order* const next = m->next;` is read **at the top**, before `m` might be released. After `release(m)`, `m`'s slot belongs to the pool and must not be read. Lab 5.3 removes this line and watches a sanitizer catch it.
- A level is erased only **after** its walk is over, when nothing points into it.

`template <class Makers>` lets the same code run on `asks_` (for a buy) and `bids_` (for a sell), even though those are different types because of their different sort orders. `match` (line 172) just picks one: `t.side == Side::Buy ? match_in(asks_, t, out) : match_in(bids_, t, out)`. The fill-or-kill dry run `fok_fillable_in` (line 99) walks the same way with `const` (read-only) access and stops as soon as enough is found.

The `...` hides three lines starting with `#ifdef LOBSIM_BUG_TAKER_PRICE`. That is a **deliberately injected bug** that only exists in special fuzzing builds, used to prove the tests can catch it. Module 6 explains it. The `#error` at the top of the file makes sure it can never be switched on in a normal build.

## 5.7 Read the real code: modify

`do_modify` (line 254) runs the same checks in the same order as the reference book, then:

```cpp
  if (req.new_price == o->price && req.new_qty < o->remaining) {
    o->level->total_qty -= o->remaining - req.new_qty;
    o->remaining = req.new_qty;
    out.push_back(Modified{o->id, o->owner, o->price, req.new_qty, true});
    return;
  }

  // Anything else loses priority: the order leaves the book (its node goes back to the
  // pool and its level may be erased) and re-enters as a new arrival with the same
  // OrderId, keeping stp and post_only. Everything needed afterwards is copied first.
  const OrderId id = o->id;
  const OwnerId owner = o->owner;
  const Side side = o->side;
  const STP stp = o->stp;
  const bool post_only = o->post_only;
  remove_resting(o);  // `o` dangles from here on
  out.push_back(Modified{id, owner, req.new_price, req.new_qty, false});
  ... match as a Taker, then rest or cancel, exactly like the reference book
```

Shrinking in place keeps the order where it is in the list (that *is* keeping priority) and fixes the level's total. Everything else removes the order and re-adds it at the back, which is what "losing priority" means in this data structure. Same lesson as `ReferenceBook::do_modify`: copy the fields first, because the order is about to disappear.

## 5.8 The pool, up close

`include/lob/pool.hpp` is short and worth reading in full. The essentials:

```cpp
template <class T, std::size_t ChunkSize = 4096>
class ObjectPool {
  static_assert(std::is_trivially_destructible_v<T>, "ObjectPool never runs destructors at pool death");
 public:
  template <class... Args>
  T* create(Args&&... args) {
    if (free_.empty()) grow();
    void* mem = free_.back();
    free_.pop_back();
    LOB_POOL_UNPOISON(mem, sizeof(Slot));
    T* obj = ::new (mem) T(std::forward<Args>(args)...);
    ++live_;
    return obj;
  }

  void destroy(T* obj) noexcept {
    obj->~T();
    void* mem = static_cast<void*>(obj);
    free_.push_back(mem);  // capacity reserved in grow(): never reallocates
    LOB_POOL_POISON(mem, sizeof(Slot));
    --live_;
  }
  ...
  std::vector<std::unique_ptr<Slot[]>> chunks_;
  std::vector<void*> free_;
```

- `::new (mem) T(...)` is **placement new**: "build a `T` in this memory I already have", instead of asking for new memory.
- The free list hands back the **most recently freed** slot first. That slot was just used, so it is probably still in the processor's cache (DECISIONS.md D-041).
- `LOB_POOL_POISON` marks a freed slot as "do not touch" for **AddressSanitizer** (Module 6). In a sanitizer build, any read of a freed order is reported at once, with the line that did it. In normal builds the macro does nothing.
- The free list is a separate vector, not links stored inside the free slots. Storing links inside would mean reading poisoned memory (D-040).
- `static_assert` is a check done by the compiler. Here it insists that `T` needs no cleanup when destroyed, because the pool does not track which slots are in use when it dies.

## 5.9 Self-checks: `validate()`

`validate()` (line 472) checks every invariant of SPEC.md 3.8 and throws `std::logic_error` naming the first broken one. For each side and each level:

- the level's price equals its map key, and is in `[1, max_price]`;
- the level is not empty, its head has no `prev` and its tail has no `next`;
- walking the list: every `next`/`prev` pair agrees, every order points back at this level, is on this side, has this price, has `remaining > 0`, and the index slot for its id points exactly at it;
- the walk length equals `count` and the quantities add up to `total_qty`.

For the whole book: not crossed; the number of non-null index slots equals the number of resting orders (so the index holds nothing stale); the pool's live count equals the number of resting orders (so no slot leaked).

Debug builds run this after **every** request. `tests/unit/order_book_validate_test.cpp` proves the checks are not empty promises: through a special test-only friend (`OrderBookTestPeer`) it breaks a healthy book on purpose in each way (wrong total, wrong count, a cleared index slot, a stale index slot, a zero-quantity order, a crossed book, broken links, an order on the wrong side, a wrong price) and expects `validate()` to name the problem.

## 5.10 What each operation costs

| Operation | ReferenceBook | OrderBook |
|---|---|---|
| Best bid or ask | O(N) | O(1): `begin()` |
| Add an order that rests at an existing price | O(N) (search for the best opposite price) | O(log L) to find the level, then O(1) |
| Add an order at a new price | O(N) | O(log L), plus one map node allocation |
| Cancel | O(N) to find, O(N) to erase | O(1) through the index, plus erasing a level if it empties |
| A trade with the first order in line | O(N) per fill | O(1) per fill |

**Why not O(1) for adding to an existing level, as SPEC.md 3.5 targets?** Finding the level by price in a `std::map` is O(log L), and the spec also requires `std::map`. DECISIONS.md D-050 records this honestly: L is tens of levels in the simulator, so it is a handful of comparisons, and an array indexed by price (a "price ladder") is listed as a stretch goal.

**Skipped on purpose:** the query functions (`best_bid`, `best_level`, `depth`, `order`, `level_orders`, lines 367 to 421) read the maps and lists without changing anything; the book-builder operations (`insert_passive`, `reduce`, `remove`, `preview_match`, lines 302 to 365) mirror the reference book's and are used by the replayer in Module 13. The copy and move constructors are `= delete`d in `order_book.hpp`: a copied book would hold pointers into the original's levels and pool (D-045).

## Lab 5.1: feel the Big-O

```sh
course/labs/build_lab.sh course/labs/lab05_big_o.cpp
build/release/labs/lab05_big_o
```

Both books get the same job: hold N resting orders, then do 20,000 rounds of "add one order, cancel the oldest". Predict first: when N grows 10 times, how much slower does each book get? On the machine used to write this course:

```
 resting N   ReferenceBook ns/step       OrderBook ns/step
       100                      75                      81
      1000                   14543                      61
     10000                  147191                      57
```

The reference column grows about 10 times from 1,000 to 10,000 orders: O(N). The fast column stays flat. Your numbers will differ.

Two puzzles: why are the two books about equally fast at N = 100? And why did the reference book get almost 200 times slower from 100 to 1,000, not 10 times?

<details><summary>Answers</summary>

At N = 100 the whole vector is tiny (about 6 KB) and stays in the processor's fastest cache, so a scan of 100 orders costs about as much as the fast book's map and pool bookkeeping. Between 100 and 1,000 orders the vector outgrows that cache, and every scan and every `erase` (which shifts the orders behind the erased one) starts paying for slower memory too. Big-O describes the growth for large N; caches change the constants.

</details>

## Lab 5.2: draw it

On paper, start from an empty `OrderBook` and apply: sell 5 @ 101 (trader 1), sell 3 @ 101 (trader 2), sell 4 @ 102 (trader 3). Draw `asks_` (which keys, in which order), each `Level` with `total_qty`, `count`, `head`, `tail`, each `Order` with `prev`, `next`, `level`, and `index_`. Then apply "cancel order 1" and redraw: which pointers changed, and which lines of `unlink` changed them?

<details><summary>Answer for the cancel</summary>

Order 1 was the head of level 101. `unlink`: `o->prev` is null, so `level->head = o->next` (order 2). `o->next` is order 2, so `order2->prev = o->prev` (null). `total_qty` 8 → 3, `count` 2 → 1. Then `release` clears `index_[1]` and returns the slot to the pool. The level still has one order, so it stays in the map.

</details>

## Lab 5.3: a bug the tests cannot see

This lab needs the sanitize build. It takes a few minutes the first time:

```sh
cmake --preset sanitize
cmake --build --preset sanitize -j --target lob_tests
```

Now inject a bug: in `match_in`, read `m->next` **after** the maker may have been released, instead of before.

```sh
git apply course/labs/patches/lab05_use_after_release.patch
git diff                                     # read exactly what changed
```

Predict: will the normal tests notice? Rebuild and run the release tests and the differential test:

```sh
cmake --build --preset release -j --target lob_tests fuzz_diff
build/release/lob_tests --gtest_brief=1 | tail -2
build/release/fuzz_diff --seed 1 --messages 10000 > /dev/null; echo "exit code $?"
```

When this course was written: `[  PASSED  ] 225 tests.` and `exit code 0`. The bug is invisible. (Why? After `unlink`, the released order's `next` field was set to `nullptr`, and nothing overwrote it before it was read, so the inner loop just stops early and the outer loop starts again at the level's new head. The answer is right by luck.)

Now the sanitize build:

```sh
cmake --build --preset sanitize -j --target lob_tests
build/sanitize/lob_tests --gtest_filter='MatchingTest/1.WorkedExample*' 2>&1 | grep -E "ERROR|READ|#0 "
```

As printed when this course was written (shortened):

```
==108==ERROR: AddressSanitizer: use-after-poison on address 0x7f0ec91bf830 ...
READ of size 8 at 0x7f0ec91bf830 thread T0
    #0 ... in lob::OrderBook::match_in<...> ... src/lob/order_book.cpp:162
```

AddressSanitizer stopped the program at the exact line that read a slot the pool had poisoned. "Size 8" is the 8-byte pointer `next`. Undo the bug and rebuild:

```sh
git apply -R course/labs/patches/lab05_use_after_release.patch
git diff --stat                              # should print nothing
cmake --build --preset release -j && cmake --build --preset sanitize -j --target lob_tests
```

The lesson: a test only checks the answers it looks at. A program can give the right answers while doing something illegal, and the next compiler, machine or change can turn that into a crash or a silent wrong answer. Sanitizers check the *behaviour*, not the answers. That is why lobsim runs its whole test suite under them in CI.

If your system's compiler has no sanitizer runtime installed, the sanitize build stops with a message such as `libasan.so.6: cannot open shared object file`. Installing your compiler's sanitizer package (for GCC, `libasan` and `libubsan`) fixes it.

## Lab 5.4: where does the fast book allocate memory?

```sh
build/release/lobsim_bench --messages 2000000 --ref-messages 50000
```

Find the section `heap allocations inside OrderBook calls`. When this course was written: `25972 allocations (0.0130 per message)` in 2,000,000 messages, almost all of them `80 bytes x 25948`. The rest form a doubling series 16, 32, 64, ..., 16,777,216 bytes (one of each), plus three left over: one more of 262,144 bytes, one more of 32,768 bytes, and one of 8 bytes. Using this module, explain:

1. The 80-byte allocations.
2. The doubling series.
3. The three left over.

Then read the `allocation audit on a warm book` table just below it. Which operations allocate nothing?

<details><summary>Answers</summary>

1. A `std::map` node: one new price level (a `Level` plus the map's own bookkeeping).
2. The id index `std::vector<Order*>` doubling as order ids grow (8 bytes per id).
3. The pool: one chunk of 4,096 orders × 64 bytes = 262,144 bytes, that chunk's free list of 4,096 addresses × 8 bytes = 32,768 bytes, and the pool's list of chunks (one 8-byte pointer). The book never held more than 4,096 orders, so one chunk was enough. The README's table for a 10-million-message run shows the same pattern.

Adding to an existing level, cancelling while the level stays, matching that leaves the level, and a same-price smaller modify all allocate nothing.

</details>

## Common questions

**Why not use a hash map for the id lookup?** It allocates a node per insert and hashes on every lookup. Ids are dense (1, 2, 3, ...), so a plain vector is simpler and faster. The cost is memory proportional to the highest id.

**What happens to the index entries of finished orders?** They are set back to `nullptr`, but the vector never shrinks. That is why `validate()` keeps a counter `indexed_` of non-null slots instead of scanning the whole vector after every message (D-044).

**Is `std::map` slow?** Each lookup is a few pointer hops in a tree. For tens of levels that is very fast; the README's microbenchmark adds an order to an existing level in 16.5 ns on a book of 1,000 orders and 18.1 ns on one of 100,000. An array indexed by price would be faster still and is a listed stretch goal.

**Why does releasing an order not erase its level immediately inside `match_in`?** The loop is still walking that level and holds a reference to it. Erasing the map node would make `level` dangle. So the code erases the level after the walk (`if (level.count == 0) makers.erase(it);`).

**Why is the book not copyable?** Orders hold pointers to their `Level`s inside *this* book's maps. A copy would point into the original. Deleting the copy constructor turns that mistake into a compile error.

## Quiz

1. Rank O(1), O(N), O(log N) from fastest growing to slowest growing.
2. Why does `begin()` of `bids_` give the highest bid?
3. What does `(cond ? a : b) = x;` do in `unlink` and `append`?
4. Why can't the orders be kept in a `std::vector<Order>`?
5. In `match_in`, why is `next` read before the maker is released?
6. Name four invariants `validate()` checks.
7. Which costs O(log L) in the fast book, and why not O(1)?

<details><summary>Answers</summary>

1. O(N) grows fastest, then O(log N), then O(1), which does not grow.
2. The bids map is declared with `std::greater<Price>`, so it is sorted from high to low.
3. It chooses which variable to assign: `a` if `cond` is true, otherwise `b`, then stores `x` in it. It handles "neighbour exists" and "I am at the end of the list" in one line.
4. A vector moves its elements when it grows, so every pointer to an order (in the lists, the index) would dangle.
5. After `release`, the slot belongs to the pool; reading it is a use-after-free bug (Lab 5.3).
6. Any four of: not crossed; level price equals its key; no empty level; head has no prev and tail has no next; next/prev links agree; each order points at its level, has its side and price, and remaining > 0; the index points exactly at each resting order; count and total_qty match the list; index non-null count and pool live count equal the number of resting orders.
7. Finding a price level in the `std::map` (adding an order, creating or erasing a level). The spec fixes the container as `std::map`, which has no O(1) lookup by price.

</details>

## What's next

Two engines, one set of rules. Module 6 shows how lobsim makes sure they really agree, and how it tests the tests.
