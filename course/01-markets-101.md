# Module 1: Markets 101

> **In one sentence:** a market is a list of people waiting to buy, a list of people waiting to sell, and a rule for when they trade.
>
> **You will be able to:** read an order book, find the best bid, best ask, spread and mid, say what a tick and a lot are and why lobsim uses whole numbers for them, and read the order and BBO types in lobsim's code.
>
> **Time:** about 2 hours.

## 1.1 A trade is an agreement

A **trade** happens when a buyer and a seller agree on two numbers: **how many** and **at what price**. If you sell me 3 stickers at 11 cents each, then I own 3 more stickers, you own 3 fewer, and 33 cents moved from me to you. That is all a trade is.

On a stock exchange the "stickers" are **shares** of a company. Owning a share means owning a tiny piece of that company. lobsim trades one made-up stock, so you can think of it as one kind of sticker.

## 1.2 An order is a wish with limits

Most people cannot be at the market all day waiting for the perfect partner. Instead they leave an instruction called an **order**:

> "I want to **buy** **5** shares, and I will pay **at most 100** each."

That has three parts:

| Part | Name in lobsim | Example |
|---|---|---|
| Which way | `side`: `Buy` or `Sell` | Buy |
| How many | `qty` (quantity) | 5 |
| The worst price you accept | `price` (the "limit") | 100 |

An order with a worst acceptable price is a **limit order**. "At most 100" for a buyer means 100 or cheaper is fine. "At least 102" for a seller means 102 or more is fine. Nobody is ever forced to trade at a price worse than their limit.

## 1.3 The order book: two lists

Orders that cannot trade right away **rest** (wait) in the **order book**. The book has two sides:

- **Bids:** the buy orders, sorted with the **highest** price first (the most generous buyer leads).
- **Asks** (also called offers): the sell orders, sorted with the **lowest** price first (the cheapest seller leads).

Here is a small book. Read it like a ladder: sellers on top, buyers below, the gap in the middle.

```
           price   size
  ASKS      104     7      <- a seller wants at least 104 for 7 shares
            103     2
            102     6      <- best ask: the cheapest seller
         ------------------  the spread: 102 - 100 = 2
  BIDS      100     5      <- best bid: the most generous buyer
             99     8
             97     3
```

All orders at one price form a **price level**. If two people each want to buy 4 at 100, the level at 100 holds 8.

## 1.4 Four numbers everyone watches

| Name | Meaning | Above |
|---|---|---|
| **Best bid** | The highest price any buyer is waiting to pay | 100 |
| **Best ask** | The lowest price any seller is waiting to accept | 102 |
| **Spread** | best ask − best bid | 2 |
| **Mid** (mid-price) | (best bid + best ask) / 2 | 101 |

Together, the best bid and best ask (with their sizes) are called the **top of book**, or **BBO** (best bid and offer). This is what most traders look at first.

Why can't the best bid be higher than the best ask? If a buyer offered 103 while a seller asked 102, they would simply trade. The matching engine never lets that situation rest in the book; a book where bid ≥ ask is called **crossed**, and in lobsim a crossed book can only mean a bug (Module 5 shows the check).

The mid is not a price anyone traded at. It is a summary: "the market's price is somewhere around here". If the spread is odd, the mid lands between two ticks: with bid 9999 and ask 10000 the mid is 9999.5.

If one side is empty (no sellers at all, say) there is no best ask, no spread and no mid. The book is **one-sided**. Code that uses the mid must handle "there is no mid right now", and you will see lobsim do that many times.

## 1.5 Liquidity and depth

**Liquidity** means how easy it is to trade a lot without moving the price much. A book with thousands of shares waiting near the mid is **liquid**. A book with a handful of shares spread thinly is **illiquid**: a big order would eat through several levels and pay worse and worse prices.

**Depth** is how much is waiting at each level. "Depth at level 1" is the size at the best price; "depth at level 3" is the size at the third-best price. lobsim's `depth(side, n)` returns the top `n` levels of one side as (price, total size) pairs.

## 1.6 Ticks and lots: why whole numbers

Real exchanges only allow prices on a grid. In US stocks the smallest step, the **tick**, is usually one cent: you can bid 100.00 or 100.01, but not 100.005. Quantities also come in steps; lobsim calls its step a **lot**.

lobsim stores every price as a whole number of ticks and every quantity as a whole number of lots. The display setting `tick_display: 0.01` in the config only says "show 1 tick as 0.01 when printing". Inside, 10,000 ticks is the number 10000, never 100.00.

Why? Because computers store decimal fractions approximately. Try it in Python:

```python
>>> 0.1 + 0.2 == 0.3
False
>>> 0.1 + 0.2
0.30000000000000004
```

A matching engine compares prices all the time ("is this seller's price at or below the buyer's limit?"). With approximate numbers, two prices that should be equal can come out unequal, and a trade that should happen would not. With whole numbers, `100 == 100` is always exactly true. SPEC.md calls this out as its first design rule, and DECISIONS.md lists it among the main choices.

## 1.7 Makers, takers, and the trade price (a first look)

When an order arrives and can trade immediately with an order already resting, the resting one is the **maker** (it *made* the liquidity by waiting) and the arriving one is the **taker** (it *took* the liquidity). The taker is also called the **aggressor**.

The rule that surprises most people: **the trade happens at the maker's price**. If a seller rests at 102 and a buyer arrives willing to pay up to 105, they trade at 102. The buyer gets the good price because the seller asked for it first. Module 2 is all about these rules.

## 1.8 Long, short, and inventory

If you buy shares you are **long** (you own them; you win if the price rises). If you sell shares you do not own, you are **short** (you owe them; you win if the price falls). lobsim's market maker tracks its **inventory** `q` as a signed number: `q = +20` means long 20 lots, `q = -20` means short 20 lots. A trader's goal is usually to end with more money; how lobsim counts "more money" is Module 11.

## Read the real code

### The basic types: `include/lob/types.hpp`

Open the file. The first lines give everyday words a precise type:

```cpp
using Price = std::int64_t;    // ticks, valid range [1, max_price]
using Qty = std::int64_t;      // lots, > 0
using OrderId = std::uint64_t; // assigned by engine, starts at 1, strictly increasing
using OwnerId = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class TIF : std::uint8_t { GTC, IOC, FOK };
enum class STP : std::uint8_t { CancelTaking, CancelResting };
```

- `std::int64_t` is a whole number stored in 64 bits: it can go up to about 9 quintillion and can be negative. `std::uint64_t` is the same size but never negative. `std::uint32_t` is 32 bits, up to about 4 billion, and `std::uint8_t` is 8 bits, 0 to 255.
- `using Price = std::int64_t;` makes `Price` another name for that type. It does not create anything new for the computer, but it tells the reader what a number means. A function that takes `(Price, Qty)` is much clearer than one that takes `(long, long)`.
- `enum class Side { Buy, Sell }` is a list of allowed names. A `Side` can only be `Side::Buy` or `Side::Sell`. Module 3 explains the C++ details.
- An **owner** is a trader. Every agent has an owner id; the engine uses it to send replies to the right agent and to stop traders from trading with themselves.

The order itself:

```cpp
struct NewOrder {
  OwnerId owner{};
  std::uint64_t client_order_id{};
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  TIF tif{TIF::GTC};
  Price price{};
  Qty qty{};
  bool post_only{false};
  STP stp{STP::CancelTaking};
  bool operator==(const NewOrder&) const = default;
};
```

You already know `owner`, `side`, `price` and `qty`. `client_order_id` is the trader's own label for the order (so it can recognise the reply); the engine gives the order its official `OrderId` when it accepts it. `type`, `tif`, `post_only` and `stp` are rules about *how* the order may trade, covered in Module 2.

### The top of book: `include/sim/messages.hpp`

The simulator sends traders the top of book as a `BBO`:

```cpp
// Top of book. A side with qty 0 (and price 0) is empty.
struct BBO {
  lob::Price bid{0};
  lob::Qty bid_qty{0};
  lob::Price ask{0};
  lob::Qty ask_qty{0};
  bool has_bid() const noexcept { return bid_qty > 0; }
  bool has_ask() const noexcept { return ask_qty > 0; }
  bool two_sided() const noexcept { return has_bid() && has_ask(); }
  ...
};
```

Notice how "no best bid" is represented: a quantity of 0. Every trader checks `two_sided()` before computing a mid. Search the code for `two_sided()` (for example with `grep -rn "two_sided()" src`) and you will find it in the noise trader, the market maker, the exchange and the metrics.

The exchange computes the mid as `(top_.bid + top_.ask) / 2.0` in `src/sim/exchange.cpp` (line 64), a decimal number because it can end in .5. That is one of the few places a price becomes a fraction, and only for measuring, never for matching.

## Lab 1.1: build a book on paper

These orders arrive one after another. None of them trade (check that as you go: a new buy must be below the best ask, a new sell above the best bid).

| # | Trader | Order |
|---|---|---|
| 1 | Ann | sell 4 @ 103 |
| 2 | Ben | buy 2 @ 99 |
| 3 | Cat | sell 6 @ 102 |
| 4 | Dan | buy 5 @ 100 |
| 5 | Eve | sell 1 @ 102 |
| 6 | Fin | buy 3 @ 100 |

Draw the book as a ladder like the one in 1.3. Then write down: the best bid and its size, the best ask and its size, the spread, the mid, and the depth at level 2 on each side.

<details><summary>Answer</summary>

```
  ASKS   103   4      (Ann)
         102   7      (Cat 6, then Eve 1)
  ----- spread 2, mid 101
  BIDS   100   8      (Dan 5, then Fin 3)
          99   2      (Ben)
```

Best bid 100 × 8, best ask 102 × 7, spread 2 ticks, mid 101. Level 2: bids 99 × 2, asks 103 × 4.

</details>

## Lab 1.2: check it with the real engine

Build the order book playground (it is the real lobsim engine with a small typing interface):

```sh
course/labs/build_lab.sh course/labs/lab02_book_repl.cpp
build/release/labs/lab02_book_repl
```

Type the six orders. Use `as 1` before Ann's order, `as 2` before Ben's, and so on, to switch traders. Then type `book`. Compare with your drawing. Each order should print one `Ack`, meaning "accepted, here is your order id".

## Lab 1.3: read the real top of book

Open `out/run1/bbo.csv` from Module 0 (`sed -n '1000,1004p' out/run1/bbo.csv` prints a few rows). When this course was written, these were rows 1000 to 1004:

```
time_ns,bid,bid_qty,ask,ask_qty,mid,fundamental,mm_inventory,mm_pnl
998000000000,9979,4,9981,38,9980.0,9989.136947,-2,1372.940000
999000000000,9979,5,9981,24,9980.0,9989.324687,-2,1381.976000
1000000000000,9980,5,9981,11,9980.5,9989.793441,-4,1387.040000
1001000000000,9980,11,9982,242,9981.0,9990.471913,-1,1385.046000
```

1. Convert the first `time_ns` to seconds.
2. Check the `mid` of each row from `bid` and `ask`.
3. What is the spread in each row?
4. The `fundamental` column is the hidden "true value" that only the informed trader can see (Module 9). Is the market's mid above or below it here?

Then look at how the spread behaved over the whole hour:

```sh
python3 -c "import json; print(json.load(open('out/run1/summary.json'))['spread'])"
```

When this course was written: mean 1.94 ticks, 1 tick 10.3% of the time, 2 ticks 85.3%, 3 ticks 4.5%.

<details><summary>Answers</summary>

1. 998,000,000,000 ns is 998 seconds (divide by 1,000,000,000).
2. (9979 + 9981) / 2 = 9980; (9980 + 9981) / 2 = 9980.5; (9980 + 9982) / 2 = 9981.
3. 2, 2, 1, 2 ticks.
4. Below: the mid is about 9980 while the fundamental is about 9989. The market price follows the true value only loosely, which you saw on the dashboard and which Module 9 explains.

</details>

## Common questions

**Why would a seller ask 103 when someone else asks 102?** They are not in a hurry. If the price rises, their order at 103 will trade later and they will be glad they waited. Every resting order is a bet about the future plus a willingness to wait.

**Who decides the tick size?** The exchange. lobsim has no fixed money unit: a tick is "1" and `tick_display` only controls printing.

**Is the mid the "real" price?** It is a convenient summary, not a trade price. Module 11 measures the market maker against the mid, and also explains why that choice matters.

**What is `max_price`?** The largest allowed price (1,000,000 ticks by default). Any limit price outside 1 to `max_price` is refused with `InvalidPrice`. It keeps the engine away from silly numbers.

**What does "resting" mean exactly?** An order that has been accepted, has not traded completely and is waiting in the book. Only resting orders can be cancelled or modified.

## Quiz

1. The best bid is 250 and the best ask is 253. What are the spread and the mid?
2. A new sell order arrives at 249 in that book. Can it rest at 249?
3. Why does lobsim store the price 100.25 dollars (with a 1-cent tick) as 10025?
4. In `BBO`, how do you know the ask side is empty?
5. Which is the maker when a new buy order trades with a resting sell order?

<details><summary>Answers</summary>

1. Spread 3 ticks, mid 251.5.
2. No. 249 is at or below the best bid of 250, so it would trade immediately with the buyer (at 250, the maker's price). Only what is left after trading could rest.
3. Whole numbers compare and add exactly; decimal fractions in a computer are approximate (0.1 + 0.2 is not exactly 0.3).
4. `ask_qty == 0`, so `has_ask()` is false.
5. The resting sell order. The arriving buy order is the taker (the aggressor).

</details>

## What's next

Module 2 gives the exact rules the matching engine follows: which order trades first, at what price, what each order type does, and what the engine says back.
