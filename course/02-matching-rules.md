# Module 2: The rules of matching

> **In one sentence:** the matching engine is a referee with a short, exact rulebook: best price first, earliest first at the same price, and every trade at the waiting order's price.
>
> **You will be able to:** predict, for any order arriving at any book, exactly which events the engine produces and in what order; explain each order type, the modify rules and self-trade prevention; and read lobsim's event types.
>
> **Time:** about 3 hours. This is one of the most important modules. Do every lab.

## 2.1 Price-time priority: the ice-cream line, with a twist

At an ice-cream truck, whoever arrived first is served first. That is **time priority**, also called **FIFO** (first in, first out).

A market adds one rule in front of it: the **best price** is served first. For a buyer arriving at the book, the best sellers are the cheapest ones. So the engine:

1. Looks at the best price level on the other side.
2. Inside that level, takes the earliest order first.
3. When the level is used up, moves to the next best level.
4. Stops when the arriving order is filled, or when the next level's price is worse than the arriving order's limit.

This is **price-time priority**. SPEC.md section 1 states it in two lines, and everything else in the engine follows from it.

## 2.2 The trade price is the maker's price

When the arriving order (the **taker**) trades against a resting order (the **maker**), the trade prints at the **maker's price**. The taker's limit only decides *whether* it may trade at that price, never *what* the price is.

Why is that fair? The maker announced its price first and waited. A buyer who says "up to 105" is saying the most they are willing to pay, not what they want to pay. If a seller is already offering 101, the buyer gets 101.

### The worked example (from SPEC.md, section 1)

The asks: order **A** sells 5 @ 101 (arrived first), **B** sells 3 @ 101, **C** sells 4 @ 102. A buy limit order for 10 @ 101 arrives.

- It fills 5 against A at 101, then 3 against B at 101 (A before B: time priority).
- C at 102 is above the buyer's limit, so matching stops.
- The remaining 2 rest as a bid at 101.
- Book after: best bid 101 × 2, best ask 102 × 4.

If the buyer's limit had been 105 instead, the first two fills would still print at **101** (the makers' price), and then 2 more would fill against C at **102**.

## 2.3 Order types and time in force

"Time in force" (TIF) says what happens to the part of an order that cannot trade immediately.

| Type | What it does | What happens to the leftover |
|---|---|---|
| **Limit GTC** (good till cancelled) | Trades what crosses its limit | Rests at the back of its price level |
| **Limit IOC** (immediate or cancel) | Trades what crosses its limit | Cancelled, reason `IOCRemainder` |
| **Limit FOK** (fill or kill) | Fills completely right now, or does nothing at all | If it cannot fill all of it: `Ack`, then `Cancelled(FOKUnfillable)`, and nothing else changes |
| **Market** | Trades at any price, best first | Cancelled: `IOCRemainder`, or `NoLiquidity` if nothing at all filled |
| **Post-only** | A GTC limit that must not trade on arrival | Rejected with `PostOnlyWouldCross` if its price reaches the other side's best price |

A few details that matter:

- **Market orders are always IOC** in lobsim. A market order with any other TIF is invalid. Its price field is ignored.
- **FOK needs a dry run.** To know whether it can fill *all* of its quantity, the engine first walks the book in priority order *without changing anything*, adding up what it could take. Only if the total is enough does it trade for real. The dry run follows the same self-trade rules as real matching (2.6), otherwise it could count quantity it would not actually be allowed to take.
- **Post-only guarantees you are a maker.** Market makers use it because makers often pay lower fees (in lobsim's default config the market maker even gets a small rebate as a maker). "Would cross" is a pure price test: a post-only buy is rejected if its price is at or above the best ask, and a post-only sell if its price is at or below the best bid, whoever owns those orders.

## 2.4 Validation: refuse bad requests before touching anything

Before any change to the book, the engine checks the request. The **first** failing check decides the reason, and a refused request changes nothing and does **not** use up an order id.

For a new order, in this order:

1. `qty <= 0` → `InvalidQty`
2. post-only combined with a market order or with a TIF other than GTC, or a market order with a TIF other than IOC → `InvalidFlags`
3. a limit price outside `[1, max_price]` → `InvalidPrice` (a market order's price is ignored)
4. post-only that would cross → `PostOnlyWouldCross`

For a cancel: the id is not resting → `UnknownOrder`; the owner does not match → `NotOwner`.

For a modify: `UnknownOrder`, `NotOwner`, `InvalidQty`, `InvalidPrice`, then **same price and same quantity** → `NoChange`, then a post-only order modified to a crossing price → `PostOnlyWouldCross`.

Why does the order of checks matter? A request can be wrong in two ways at once (say, quantity 0 *and* price −5). Both of lobsim's books must report the same reason, or the tests comparing them would fail. Fixing the order removes the ambiguity.

`UnknownOrder` means "not resting right now". An id that never existed, an order that was fully filled and an order already cancelled all look the same (DECISIONS.md D-020). This is exactly what happens in the simulator when a trader sends a cancel for an order that got filled while the cancel was travelling.

## 2.5 Cancel and modify

**Cancel** removes a resting order: `Cancelled{reason = User}`, with the quantity that was still resting.

**Modify** changes a resting order's price and/or its quantity. `new_qty` is the new **remaining** quantity, not the original size. The rules, from SPEC.md 3.3.6 (they follow the Cboe exchange's documented behaviour):

- **Same price, smaller quantity:** the order shrinks in place and **keeps its place** in line (`priority_kept = true`).
- **Anything else** (a bigger quantity, or any price change): the order **loses its place**. It leaves the book and re-enters as if it had just arrived, with the **same order id** and its old `post_only` and `stp` settings. It goes to the back of its new price level, and if the new price crosses the other side, it **trades as a taker** right away.
- The engine emits `Modified{..., priority_kept}` **before** any trades the modify causes.

Why should a bigger order lose its place? Otherwise a trader could put a tiny order at the front of the line early and then grow it into a huge one, jumping ahead of everyone who arrived in between. Shrinking hurts nobody behind you, so it keeps priority.

## 2.6 Self-trade prevention (STP)

A **self-trade** is a trade where the buyer and the seller are the same trader. It moves no real ownership, and real exchanges block it (it can look like fake trading activity). In lobsim, STP applies when the taker meets a resting order with the same `owner`, and the **taker's** `stp` setting decides what happens:

| Mode | What happens |
|---|---|
| `CancelTaking` (the default) | The taker's remaining quantity is cancelled, `Cancelled(SelfTrade)`. Fills it already made stand; the resting order is untouched. |
| `CancelResting` | The resting order is cancelled, `Cancelled(SelfTrade)` for it, and matching continues with the next order. |

In the FOK dry run: under `CancelResting`, a same-owner order is skipped (it would be cancelled, so it adds nothing). Under `CancelTaking` it ends the dry run, and the FOK is fillable only if enough quantity was found *before* reaching it.

## 2.7 What the engine says back: events

The engine answers every request by appending **events** to a list owned by the caller. There are exactly five kinds (`include/lob/events.hpp`):

| Event | When | Fields |
|---|---|---|
| `Ack` | A new order was accepted. Always the first event for that order. | `order_id` (fresh, from 1 upwards), `owner`, `client_order_id` |
| `Reject` | A request was refused; nothing changed. | `owner`, `request_kind` (New, Cancel, Modify), `client_order_id` (new orders) or `order_id` (cancel, modify), `reason` |
| `Trade` | Two orders traded. | `trade_id`, `price` (the maker's), `qty`, `maker_order_id`, `taker_order_id`, `maker_owner`, `taker_owner`, `aggressor_side` (the taker's side) |
| `Cancelled` | An order left the book or was not allowed to rest. | `order_id`, `owner`, `qty_cancelled`, `reason` (User, IOCRemainder, NoLiquidity, FOKUnfillable, SelfTrade) |
| `Modified` | A modify was accepted. Emitted before any trade it causes. | `order_id`, `owner`, `new_price`, `new_qty`, `priority_kept` |

Within one request, events appear in the order things happen: `Ack` or `Modified` first, then trades and STP cancels in match order, then any final `Cancelled` for the leftover.

`Reject` and `Cancelled` are different things. A `Reject` means the request never became an order (or the cancel or modify was refused). A `Cancelled` means an order existed, and now it is gone or the rest of it is gone. An IOC that trades 3 of its 5 lots gets an `Ack`, a `Trade`, and a `Cancelled(IOCRemainder)` for 2: it was accepted, so it is not a reject.

## Read the real code

### The events: `include/lob/events.hpp`

```cpp
enum class RejectReason : std::uint8_t {
  InvalidQty, InvalidPrice, InvalidFlags, UnknownOrder, NotOwner, NoChange, PostOnlyWouldCross
};
enum class CancelReason : std::uint8_t { User, IOCRemainder, NoLiquidity, FOKUnfillable, SelfTrade };
enum class RequestKind : std::uint8_t { New, Cancel, Modify };

struct Trade {
  std::uint64_t trade_id{};
  Price price{};
  Qty qty{};
  OrderId maker_order_id{};
  OrderId taker_order_id{};
  OwnerId maker_owner{};
  OwnerId taker_owner{};
  Side aggressor_side{};
  bool operator==(const Trade&) const = default;
};
...
using Event = std::variant<Ack, Reject, Trade, Cancelled, Modified>;
using EventBuffer = std::vector<Event>;
```

An `Event` is "exactly one of these five". An `EventBuffer` is a growable list of them. Module 3 explains `std::variant`.

**Skipped on purpose:** `Ack`, `Reject`, `Cancelled` and `Modified` are written the same way as `Trade`: a plain struct of fields with a default `==`. The `to_string` functions below them turn each enum and each event into readable text, one `switch` per enum; read one and you have read them all. `EventHasher` at the bottom of the file is covered in Module 12.

### One function for every kind of request: `include/lob/apply.hpp`

```cpp
template <class Book>
void apply(Book& book, const Request& req, EventBuffer& out) {
  struct Dispatch {
    Book& book;
    EventBuffer& out;
    void operator()(const NewOrder& r) const { book.submit(r, out); }
    void operator()(const Cancel& r) const { book.cancel(r, out); }
    void operator()(const Modify& r) const { book.modify(r, out); }
  };
  std::visit(Dispatch{book, out}, req);
}
```

`apply` looks at which kind of request it was given and calls the matching function. It works for either book (that is what `template <class Book>` means). The exchange, the replay tool, the fuzzer, the benchmark and the playground all feed requests through this one function.

### The rules written as code: validation in `src/lob/order_book.cpp`

`OrderBook::do_submit` (line 185) starts with exactly the checks of 2.4, in the same order:

```cpp
  // 3.2 validation, first failing check wins.
  if (req.qty <= 0) return reject(RejectReason::InvalidQty);
  if ((req.post_only && (is_market || req.tif != TIF::GTC)) || (is_market && req.tif != TIF::IOC))
    return reject(RejectReason::InvalidFlags);
  if (!is_market && !price_in_range(req.price)) return reject(RejectReason::InvalidPrice);
  if (req.post_only && would_cross(req.side, req.price))
    return reject(RejectReason::PostOnlyWouldCross);

  const OrderId id = next_order_id_++;
  out.push_back(Ack{id, req.owner, req.client_order_id});
```

Each line is one rule. `return reject(...)` stops the function, so nothing after it runs: no id is taken and the book is untouched. Only when every check passes does `next_order_id_++` hand out the next id, and the first event pushed is the `Ack`. Module 4 reads the rest of this function.

## Lab 2.1: the worked example, by hand and by engine

On paper, write the events you expect for the worked example (2.2), including every field of each `Trade`. Number the orders: A is order 1 (owner 1), B is 2 (owner 2), C is 3 (owner 3), the buyer is order 4 (owner 9).

Then run it in the playground:

```sh
course/labs/build_lab.sh course/labs/lab02_book_repl.cpp
build/release/labs/lab02_book_repl < course/labs/scripts/worked_example.txt
```

The important lines, as printed when this course was written:

```
trader 9> buy 10 @ 101
  Ack{order_id=4, owner=9, client_order_id=4}
  Trade{trade_id=1, price=101, qty=5, maker_order_id=1, taker_order_id=4, maker_owner=1, taker_owner=9, aggressor_side=Buy}
  Trade{trade_id=2, price=101, qty=3, maker_order_id=2, taker_order_id=4, maker_owner=2, taker_owner=9, aggressor_side=Buy}
trader 9> book
  ASKS (sellers), the lowest ask is next to the line
  102 | 4 lots   [#3 trader 3: 4]
  ------ spread 1 tick, mid 101.5
  101 | 2 lots   [#4 trader 9: 2]
  BIDS (buyers), the highest bid is next to the line
```

Now edit a copy of the script so the buyer's limit is 105 and predict the events before you run it.

<details><summary>What you should see with limit 105</summary>

`Ack(4)`, then trades of 5 @ 101 (maker 1), 3 @ 101 (maker 2), 2 @ 102 (maker 3). Nothing rests on the bid side; C keeps 2 lots at 102.

</details>

## Lab 2.2: every order type

```sh
build/release/labs/lab02_book_repl < course/labs/scripts/order_types.txt
```

Open `course/labs/scripts/order_types.txt` first. For each command marked A to F, write down the events you expect. Then run and compare. A few of the answers, as printed:

```
trader 2> buy 5 @ 100 ioc
  Ack{order_id=3, owner=2, client_order_id=3}
  Trade{trade_id=1, price=100, qty=3, maker_order_id=1, taker_order_id=3, maker_owner=1, taker_owner=2, aggressor_side=Buy}
  Cancelled{order_id=3, owner=2, qty_cancelled=2, reason=IOCRemainder}
trader 2> buy 5 @ 101 fok
  Ack{order_id=4, owner=2, client_order_id=4}
  Cancelled{order_id=4, owner=2, qty_cancelled=5, reason=FOKUnfillable}
...
trader 2> buy 2 market
  Ack{order_id=6, owner=2, client_order_id=6}
  Cancelled{order_id=6, owner=2, qty_cancelled=2, reason=NoLiquidity}
...
trader 4> sell 1 market post
  Reject{owner=4, request_kind=New, client_order_id=11, order_id=0, reason=InvalidFlags}
```

Notice that the refused orders in part F did not use an order id: the next accepted order still gets the next number in line.

## Lab 2.3: modify and self-trade prevention

```sh
build/release/labs/lab02_book_repl < course/labs/scripts/modify_and_stp.txt
```

Three bids rest at 100: #1, #2, #3. Then #1 shrinks (keeps its place), #2 grows (goes to the back), trader 3 tries to cancel someone else's order, a seller arrives, and finally trader 3 sells into a book where its own order is first in line, once with each STP mode. As printed:

```
trader 3> sell 10 @ 100
  Ack{order_id=5, owner=3, client_order_id=5}
  Cancelled{order_id=5, owner=3, qty_cancelled=10, reason=SelfTrade}
...
trader 3> sell 5 @ 100 resting
  Ack{order_id=6, owner=3, client_order_id=6}
  Cancelled{order_id=3, owner=3, qty_cancelled=3, reason=SelfTrade}
  Trade{trade_id=3, price=100, qty=5, maker_order_id=2, taker_order_id=6, maker_owner=2, taker_owner=3, aggressor_side=Sell}
```

Look at the `order_id` of the two `Cancelled(SelfTrade)` events: in the first, the cancelled order is the arriving one (5); in the second, it is the resting one (3).

## Lab 2.4: puzzles

Solve each on paper first, then prove it in the playground (type the commands, or put them in a file).

1. Make the engine answer `Cancelled(FOKUnfillable)` to a fill-or-kill buy of 4 lots **even though 10 lots are for sale at the FOK's price**.
2. Then change one word so the same FOK buy of 4 succeeds.
3. Make a `Modified` event with `priority_kept=false` followed by a `Trade`, using only one modify request.
4. Make the engine answer `Reject` with reason `NoChange`.
5. Make one request produce a `Trade` and then a `Cancelled(SelfTrade)` for the same arriving order.

<details><summary>Answers</summary>

1. Put your own order first in line, then someone else's: `as 2`, `sell 5 @ 100`, `as 1`, `sell 5 @ 100`, `as 2`, `buy 4 @ 100 fok`. The dry run meets trader 2's own order first; under the default `CancelTaking` that ends the walk with 0 lots found, so the FOK is unfillable.
2. Add `resting`: `buy 4 @ 100 fok resting`. Now the dry run skips the own order and finds 5 lots from trader 1. Execution cancels the own resting order (`Cancelled(SelfTrade)` for order 1), then trades 4 with trader 1's order.
3. `as 4`, `sell 2 @ 102`, `as 3`, `buy 1 @ 100`, `modify 2 102 3`: the bid moves to 102, re-enters as a taker and trades 2 lots at 102; 1 lot rests at 102.
4. Modify an order to its current price and current remaining quantity, for example `buy 5 @ 100` then `modify 1 100 5`.
5. Let another trader's order come first and yours second at the same price: `as 1`, `sell 2 @ 100`, `as 2`, `sell 3 @ 100`, `buy 5 @ 100`. Trader 2's buy trades 2 lots with trader 1, then meets its own sell and its remaining 3 are cancelled (`SelfTrade`).

</details>

## Lab 2.5: find the rules in the tests

The unit tests are the rulebook written as checks. Open `tests/unit/matching_test.cpp` and find the test named `WorkedExampleLimit101_FillsAThenBThenRestsRemainder`. It builds the same book as Lab 2.1 and checks the exact events. Then skim the names of the tests in `tests/unit/fok_postonly_test.cpp`, `cancel_modify_test.cpp`, `stp_test.cpp` and `validation_test.cpp`: each name is one rule from this module. Module 6 explains how they work.

## Common questions

**Why does a market order get an `Ack` if it never rests?** It was accepted as an order: it got an id, and it may trade. The `Ack` always comes first so that every later event can refer to the order's id.

**Why is there `Cancelled(NoLiquidity)` and not just `IOCRemainder`?** It says something useful: the market order found nothing at all to trade with. A limit IOC that fills nothing still gets `IOCRemainder` (DECISIONS.md D-021), because the spec reserves `NoLiquidity` for market orders.

**Can a modified order trade?** Yes, if its new price crosses the other side. It then trades exactly like a new arrival, as the taker.

**Does a post-only order ever trade?** Yes, later, as a maker, when someone else's order arrives and trades against it. It just never trades on its own arrival.

**What if a trader tries to cancel an order that already traded completely?** `Reject(UnknownOrder)`: the order is no longer resting. lobsim's traders expect this and ignore it.

**Why does STP look at the taker's mode and not the maker's?** That is the spec's choice (SPEC.md 3.4), and it is how many exchanges do it: the arriving order carries the instruction.

## Quiz

1. Asks: 3 @ 50 (order 1), 4 @ 50 (order 2), 2 @ 51 (order 3). A market buy for 8 arrives. List the trades.
2. Same book. A limit buy 8 @ 50 IOC arrives. What events?
3. Same book. A limit buy 8 @ 50 FOK arrives. What events?
4. A post-only sell at 60 arrives when the best bid is 60. What happens?
5. Order 7 rests as a buy of 10 @ 99. Its owner sends modify(7, 99, 4). Does it keep its place? What if it sends modify(7, 99, 12)?
6. A request has `qty = 0` and price `-3`. What is the reject reason?
7. What is the difference between `Reject` and `Cancelled`?

<details><summary>Answers</summary>

1. 3 @ 50 (maker 1), 4 @ 50 (maker 2), 1 @ 51 (maker 3). Order 3 keeps 1 lot.
2. `Ack`, trade 3 @ 50 (maker 1), trade 4 @ 50 (maker 2), `Cancelled(IOCRemainder)` for 1. It cannot reach 51.
3. `Ack`, then `Cancelled(FOKUnfillable)` for 8: only 7 lots are at 50 or better. Nothing trades and the book is unchanged.
4. It is rejected with `PostOnlyWouldCross` (a sell at or below the best bid would cross).
5. 4 lots at the same price: it keeps its place (`priority_kept=true`). 12 lots: it loses its place and goes to the back of the 99 level.
6. `InvalidQty`, because the quantity check comes first.
7. `Reject`: the request was refused and nothing changed (a refused new order never got an id). `Cancelled`: an order that existed is now gone, or the unfilled rest of an accepted order was not allowed to stay.

</details>

## What's next

You now know what the engine must do. Module 3 gives you the C++ you need to read how it does it.
