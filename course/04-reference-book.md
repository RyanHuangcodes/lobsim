# Module 4: The simple book

> **In one sentence:** `ReferenceBook` is a complete matching engine written to be read, not to be fast: one list of orders, and a search through the whole list every time it needs the best one.
>
> **You will be able to:** explain why lobsim has a deliberately slow engine, follow every line of `src/lob/reference_book.cpp` that handles new orders, cancels and modifies, and trace a request through it by hand.
>
> **Time:** about 3 hours.

## 4.1 Why write a slow engine on purpose?

Imagine checking a pocket calculator. You could do every sum again on paper. Paper is slow, but it is so simple that you trust it. If the calculator and the paper ever disagree, the calculator is suspect.

lobsim does the same with its matching engine. The fast `OrderBook` (Module 5) uses clever data structures, and clever code hides bugs. So there is a second engine, `ReferenceBook`, with the **same functions and the same rules**, written as plainly as possible. SPEC.md section 3.6 says it "exists to catch bugs in `OrderBook`", with "readability over speed". The two are fed the same requests millions of times and must produce exactly the same events (Module 6). When they disagree, you decide which one is wrong by going back to the rules of Module 2, not by assuming the slow one is right.

## 4.2 The data: one list

```cpp
// include/lob/reference_book.hpp
struct Resting {
  OrderId id;
  OwnerId owner;
  Side side;
  Price price;
  Qty remaining;
  STP stp;
  bool post_only;
  std::uint64_t priority_seq;  // lower = earlier arrival
};

std::vector<Resting> orders_;
OrderId next_order_id_{1};
std::uint64_t next_trade_id_{1};
std::uint64_t next_priority_seq_{1};
```

Every resting order, buys and sells mixed together, sits in one vector `orders_` in no particular order. There are no price levels at all. Time priority is a number: `priority_seq` is handed out from a counter when an order starts resting, so a lower number means "arrived earlier". A modify that loses priority simply gets a new, higher number.

Three counters remember what to hand out next: order ids (from 1), trade ids (from 1), priority numbers.

The trailing underscore in `orders_` is a naming habit: it marks a member of the class, so you can tell it apart from local variables.

## 4.3 Who comes first: `has_priority_over`

```cpp
bool ReferenceBook::has_priority_over(const Resting& a, const Resting& b) const {
  if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
  return a.priority_seq < b.priority_seq;
}
```

This one function is price-time priority (Module 2.1), for two orders on the same side:

- Different prices: for buys, the higher price wins; for sells, the lower price wins. `cond ? x : y` means "if `cond` then `x` else `y`".
- Same price: the earlier arrival (smaller `priority_seq`) wins.

## 4.4 Finding things by looking at everything

```cpp
// Both return an index into orders_, or orders_.size() when there is none.
std::size_t ReferenceBook::find_order(OrderId id) const {
  for (std::size_t i = 0; i < orders_.size(); ++i)
    if (orders_[i].id == id) return i;
  return orders_.size();
}

std::size_t ReferenceBook::find_best(Side side) const {
  std::size_t best = orders_.size();
  for (std::size_t i = 0; i < orders_.size(); ++i) {
    if (orders_[i].side != side) continue;
    if (best == orders_.size() || has_priority_over(orders_[i], orders_[best])) best = i;
  }
  return best;
}
```

`find_best` walks the entire list, keeping the best order of one side seen so far. "Not found" is signalled by returning `orders_.size()`, a position just past the end that can never be a real order. With N resting orders it looks at all N, every time. Keep that in mind: Module 5 is about not doing that.

## 4.5 The two price tests

```cpp
// Post-only "would cross": buy price >= best ask, or sell price <= best bid (3.3.5, D-023).
bool ReferenceBook::would_cross(Side side, Price price) const {
  std::size_t best = find_best(opposite(side));
  if (best == orders_.size()) return false;
  return side == Side::Buy ? price >= orders_[best].price : price <= orders_[best].price;
}

bool ReferenceBook::crosses(const Taker& t, Price maker_price) const {
  if (t.is_market) return true;
  return t.side == Side::Buy ? maker_price <= t.limit : maker_price >= t.limit;
}
```

`would_cross` answers "would a post-only order at this price trade on arrival?". `crosses` answers "may this taker trade at this maker's price?": a market order may trade at any price; a limit buy only at or below its limit; a limit sell only at or above.

A **taker** here is a small struct holding the arriving order (a new order, or a modified order re-entering): its id, owner, side, whether it is a market order, its limit, its remaining quantity and its STP mode.

## 4.6 The heart: `match`

```cpp
ReferenceBook::MatchResult ReferenceBook::match(Taker& t, EventBuffer& out) {
  MatchResult result;
  while (t.remaining > 0) {
    std::size_t i = find_best(opposite(t.side));
    if (i == orders_.size() || !crosses(t, orders_[i].price)) break;
    Resting& maker = orders_[i];

    if (maker.owner == t.owner) {
      if (t.stp == STP::CancelTaking) {
        result.stopped_by_stp = true;
        break;
      }
      out.push_back(Cancelled{maker.id, maker.owner, maker.remaining, CancelReason::SelfTrade});
      orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }

    Qty qty = std::min(t.remaining, maker.remaining);
    out.push_back(Trade{next_trade_id_++, maker.price, qty, maker.id, t.id, maker.owner, t.owner,
                        t.side});
    t.remaining -= qty;
    maker.remaining -= qty;
    result.filled += qty;
    if (maker.remaining == 0) orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
  }
  return result;
}
```

Read it as a loop of questions:

1. *Is anything left to fill?* (`while (t.remaining > 0)`)
2. *Who is the best maker on the other side, and may I trade at its price?* If there is none, or its price is beyond my limit, stop.
3. *Is it my own order?* Under `CancelTaking`, stop and remember why (`stopped_by_stp`). Under `CancelResting`, cancel the maker and go round again (`continue`).
4. *Otherwise trade* `min(what I want, what it has)` **at the maker's price** (`maker.price`), with the next trade id. Subtract the quantity from both. If the maker is used up, remove it.

`orders_.erase(orders_.begin() + i)` removes the i-th element and shifts everything after it one place to the left, another cost that grows with N. The `static_cast<std::ptrdiff_t>` only converts the index to the signed number type that iterator arithmetic expects (`-Wall -Wextra` would warn otherwise).

`match` never decides what happens to the leftover. It reports how much filled and whether STP stopped it, and the caller decides.

## 4.7 A new order, start to finish: `do_submit`

```cpp
void ReferenceBook::do_submit(const NewOrder& req, EventBuffer& out) {
  auto reject = [&](RejectReason reason) {
    out.push_back(Reject{req.owner, RequestKind::New, req.client_order_id, 0, reason});
  };
  const bool is_market = req.type == OrderType::Market;

  // 3.2 validation, first failing check wins.
  if (req.qty <= 0) return reject(RejectReason::InvalidQty);
  if ((req.post_only && (is_market || req.tif != TIF::GTC)) || (is_market && req.tif != TIF::IOC))
    return reject(RejectReason::InvalidFlags);
  if (!is_market && !price_in_range(req.price)) return reject(RejectReason::InvalidPrice);
  if (req.post_only && would_cross(req.side, req.price))
    return reject(RejectReason::PostOnlyWouldCross);

  const OrderId id = next_order_id_++;
  out.push_back(Ack{id, req.owner, req.client_order_id});

  Taker t{id, req.owner, req.side, is_market, req.price, req.qty, req.stp};

  if (req.tif == TIF::FOK && !fok_fillable(t)) {
    out.push_back(Cancelled{id, req.owner, t.remaining, CancelReason::FOKUnfillable});
    return;
  }

  MatchResult result = match(t, out);
  if (t.remaining == 0) return;

  if (req.tif == TIF::GTC && !result.stopped_by_stp) {
    rest(t, req.post_only);
    return;
  }

  // Remainder is cancelled; the reason follows D-021 and 3.4.
  CancelReason reason = CancelReason::IOCRemainder;
  if (result.stopped_by_stp) reason = CancelReason::SelfTrade;
  else if (is_market && result.filled == 0) reason = CancelReason::NoLiquidity;
  out.push_back(Cancelled{id, req.owner, t.remaining, reason});
}
```

Five stages:

1. **Validate** (you saw this in Module 2). Any failure returns before anything changes.
2. **Accept**: take the next id, emit `Ack`. This is why `Ack` is always first.
3. **Fill-or-kill check**: if it cannot fill completely, emit `Cancelled(FOKUnfillable)` and stop. The book was not touched, because `fok_fillable` only reads.
4. **Match**.
5. **Decide about the leftover**: nothing left, done. A GTC order that was not stopped by STP rests. Everything else is cancelled, and the reason is chosen by the rules of DECISIONS.md D-021 and D-030: `SelfTrade` if STP stopped it, `NoLiquidity` if it was a market order that filled nothing, otherwise `IOCRemainder`.

`rest` is three lines: push a new `Resting` with the next `priority_seq` onto the end of the vector.

## 4.8 Fill-or-kill's dry run

```cpp
bool ReferenceBook::fok_fillable(const Taker& t) const {
  std::vector<Resting> makers;
  for (const Resting& o : orders_)
    if (o.side == opposite(t.side) && crosses(t, o.price)) makers.push_back(o);
  std::sort(makers.begin(), makers.end(),
            [this](const Resting& a, const Resting& b) { return has_priority_over(a, b); });

  Qty found = 0;
  for (const Resting& m : makers) {
    if (found >= t.remaining) break;
    if (m.owner == t.owner) {
      if (t.stp == STP::CancelResting) continue;  // would be cancelled, adds nothing
      break;                                      // CancelTaking: the walk ends here
    }
    found += m.remaining;
  }
  return found >= t.remaining;
}
```

It copies every maker the taker could reach, sorts the copies into priority order with `std::sort` (the lambda says how to compare two orders), and adds up quantities with the self-trade rules of Module 2.6. Because it works on copies and the function is `const`, it cannot change the book. Simple, and obviously right, which is the point.

## 4.9 Cancel and modify

```cpp
void ReferenceBook::do_cancel(const Cancel& req, EventBuffer& out) {
  ...
  std::size_t i = find_order(req.order_id);
  if (i == orders_.size()) return reject(RejectReason::UnknownOrder);
  if (orders_[i].owner != req.owner) return reject(RejectReason::NotOwner);

  out.push_back(Cancelled{orders_[i].id, orders_[i].owner, orders_[i].remaining, CancelReason::User});
  orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
}
```

Find it, check it is yours, report the quantity that was resting, remove it.

The modify (line 181) runs the six checks of Module 2.4, then:

```cpp
  // Same price, smaller qty: reduce in place, keep priority.
  if (req.new_price == o.price && req.new_qty < o.remaining) {
    o.remaining = req.new_qty;
    out.push_back(Modified{o.id, o.owner, o.price, req.new_qty, true});
    return;
  }

  // Anything else loses priority: remove and re-enter as a new arrival (same OrderId),
  // keeping stp and post_only. It may trade on re-entry as a taker.
  const Resting old = o;
  orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
  out.push_back(Modified{old.id, old.owner, req.new_price, req.new_qty, false});

  Taker t{old.id, old.owner, old.side, false, req.new_price, req.new_qty, old.stp};
  MatchResult result = match(t, out);
  if (t.remaining == 0) return;
  if (result.stopped_by_stp) {
    out.push_back(Cancelled{t.id, t.owner, t.remaining, CancelReason::SelfTrade});
    return;
  }
  rest(t, old.post_only);
```

Notice `const Resting old = o;` before `erase`. `o` is a reference into the vector; after `erase` the vector has shifted and `o` would refer to a different order (or past the end). Copying first keeps the needed values safe. In the fast book (Module 5) the same danger exists with pointers, and its code has the same "copy what you need first" comment.

## 4.10 Questions about the book

`best_bid`, `best_ask` and `best_level` use `find_best`; `depth(side, n)` copies one side, sorts it, and merges equal prices into levels; `order(id)` uses `find_order`. Every one is a scan of the whole list.

## 4.11 The self-check: `validate`

```cpp
void ReferenceBook::validate() const {
  ...
  for (const Resting& o : orders_) {
    if (o.remaining <= 0) fail("order " + std::to_string(o.id) + " has remaining <= 0");
    if (!price_in_range(o.price)) fail(... "has price outside [1, max_price]");
    ids.push_back(o.id);
  }
  // Duplicate ids, found by sorting a copy: O(n log n) instead of comparing every pair.
  std::sort(ids.begin(), ids.end());
  auto dup = std::adjacent_find(ids.begin(), ids.end());
  if (dup != ids.end()) fail("duplicate order id " + std::to_string(*dup));
  ... // and the book must not be crossed
}
```

An **invariant** is a fact that must be true whenever nobody is in the middle of changing the book. Here: every resting order has something left, a legal price and a unique id, and the best bid is below the best ask. `validate()` checks them all and throws an error naming the first one that is broken. In debug builds both books call it after every request.

**Skipped on purpose:** `insert_passive`, `reduce`, `remove` and `preview_match` (lines 220 to 278) and `level_orders` (line 327). They are the "book builder" operations added later for replaying real data (Module 13): they put orders in and take them out without matching. They are written in the same style as what you just read: search the vector, change it, done.

## Lab 4.1: trace the worked example through the code

Use the worked example again (asks A 5 @ 101, B 3 @ 101, C 4 @ 102; then a buy of 10 @ 101). On paper, write down `orders_` after each request as rows of `(id, owner, side, price, remaining, priority_seq)`. Then follow `do_submit` for the buy step by step: each pass of the `match` loop, the value of `i`, `qty`, `t.remaining`, and what is pushed to `out`.

<details><summary>Answer</summary>

After the three sells: `(1, 1, Sell, 101, 5, 1)`, `(2, 2, Sell, 101, 3, 2)`, `(3, 3, Sell, 102, 4, 3)`.

The buy passes validation and gets id 4: `Ack{4, 9, ...}`. It is GTC, not FOK. In `match`:

- Pass 1: `find_best(Sell)` compares the three sells. 101 beats 102; between the two at 101, seq 1 beats seq 2. So `i = 0` (order 1). 101 ≤ 101 crosses. Owners differ. `qty = min(10, 5) = 5`. Push `Trade{1, 101, 5, maker 1, taker 4, ...}`. `t.remaining = 5`. Order 1 is now 0, erased. `orders_` is now `[(2 ...), (3 ...)]`.
- Pass 2: best is order 2 at index 0. `qty = 3`. Push `Trade{2, 101, 3, ...}`. `t.remaining = 2`. Order 2 erased.
- Pass 3: best is order 3 at 102. 102 ≤ 101 is false: `break`.

Back in `do_submit`: 2 left, GTC, not stopped by STP: `rest` pushes `(4, 9, Buy, 101, 2, 4)`. Final `orders_`: `[(3, 3, Sell, 102, 4, 3), (4, 9, Buy, 101, 2, 4)]`.

</details>

## Lab 4.2: questions about the lines

Answer from the code shown in this module.

1. In `match`, why does the `CancelResting` branch use `continue` while the trade branch has no `continue`?
2. Why must `do_modify` copy the order into `old` before erasing it?
3. A market buy arrives on an empty book. Walk through `do_submit`: which events come out, and which line chooses the reason?
4. In `fok_fillable`, why is it fine that the function sorts a *copy*?
5. Why does `do_submit` call `fok_fillable` before `match` and not after?

<details><summary>Answers</summary>

1. Both go round the loop again. The trade branch reaches the end of the loop body anyway; the STP branch has to skip the trade code below it, so it jumps back to the top with `continue`.
2. `o` is a reference to an element of the vector. `erase` moves the elements after it, so `o` would afterwards refer to whatever moved into that slot. The values needed later (id, owner, side, stp, post_only) must be copied before.
3. `Ack`, then `match` finds no maker (`i == orders_.size()`) and returns with 0 filled. `t.remaining > 0`, the TIF is IOC, so the order is cancelled. `else if (is_market && result.filled == 0) reason = CancelReason::NoLiquidity;` picks the reason: `Cancelled(NoLiquidity)`.
4. The dry run must not change the book, and the real `orders_` is in no particular order anyway. Sorting copies gives the priority order without touching anything.
5. A fill-or-kill must not trade at all unless it can fill completely. After `match` it would be too late: the trades would already have happened.

</details>

## Lab 4.3: swap the engine under the playground

Open `course/labs/lab02_book_repl.cpp` and find the line `using Book = lob::OrderBook;`. Change it to `using Book = lob::ReferenceBook;`, rebuild, and run all three scripts plus a `hash` command:

```sh
course/labs/build_lab.sh course/labs/lab02_book_repl.cpp
(cat course/labs/scripts/worked_example.txt course/labs/scripts/order_types.txt \
     course/labs/scripts/modify_and_stp.txt; echo hash) | build/release/labs/lab02_book_repl | grep "event hash"
```

Do it once with each book. When this course was written both printed `event hash so far: 94929a8f72f52f9a`. Same events, same fingerprint: the two engines agree on these scripts. Change the line back afterwards.

This only works because both classes have exactly the same functions with the same arguments. DECISIONS.md D-010 records that choice: the tests are written once and run against both books.

## Lab 4.4: count the work

Suppose the book holds N resting orders, about half buys and half sells. For a buy limit order that does not trade and then rests, count how many orders `do_submit` looks at:

1. In `match` (one call of `find_best`)?
2. If it were post-only, how many more in `would_cross`?
3. For a cancel of an order near the end of the vector, how many in `find_order`, and how many does `erase` move?

<details><summary>Answers</summary>

1. N (every order is visited; the sells are compared).
2. N more.
3. About N for the search; `erase` moves the orders behind it, so few here but up to N for an order near the front. Every operation is proportional to N: in the language of Module 5, O(N). With 1,000 resting orders and tens of thousands of requests a second, that adds up, which is why the simulator uses the fast book.

</details>

## Common questions

**If the reference book is "obviously right", why not just use it?** Speed. Lab 5.1 measures it: with 10,000 resting orders it is more than a thousand times slower per request than the fast book. The simulator processes hundreds of thousands of requests per hour of simulated time.

**What if both books have the same bug?** Then comparing them cannot find it. That is why there are also unit tests written by hand from the rules (Module 6), a conservation checker that does its own bookkeeping, and the spec as the final judge.

**Why not store orders sorted, to make `find_best` faster?** Because then it would no longer be the simple version. Every clever idea added here would be a new place for bugs to hide, in the one engine whose job is to have none.

**Why `orders_.size()` for "not found" instead of `std::optional`?** Either works. A position one past the end is a common C++ habit for "not found" (the standard library's `find` does the same), and it keeps the scans short.

## Quiz

1. What does `priority_seq` stand for, and when does an order get a new one?
2. In what order are buys compared in `has_priority_over`?
3. Which function decides the trade price, and what price does it use?
4. Name the five stages of `do_submit`.
5. What does `validate()` check in the reference book?

<details><summary>Answers</summary>

1. The order's place in time: a counter value given when it starts resting. It gets a new one when it re-enters after a modify that loses priority (`rest` is called again).
2. Higher price first; at the same price, lower `priority_seq` (earlier) first.
3. `match`, using `maker.price`.
4. Validate, accept (id and `Ack`), the fill-or-kill check, match, and decide about the leftover (done, rest, or cancel with a reason).
5. Every resting order has remaining > 0 and a price in `[1, max_price]`, no id appears twice, and the book is not crossed.

</details>

## What's next

Module 5 builds the same engine again, fast. It needs pointers, linked lists, sorted maps and a memory pool, and it has to obey a longer list of invariants.
