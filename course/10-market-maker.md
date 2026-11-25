# Module 10: The market maker

> **In one sentence:** a market maker always offers to buy a little below the price and sell a little above it, and the Avellaneda–Stoikov formulas tell it how far apart to quote and how to lean its quotes when it is holding too much or too little.
>
> **You will be able to:** explain how a market maker earns and loses money, compute Avellaneda–Stoikov quotes by hand (including lobsim's rounding and clamps), explain each parameter, read the market maker's code, explain the "one request in flight per side" rule and the modify-refill race, and predict how the market maker's results change when you change its parameters.
>
> **Time:** about 5 hours, in two sittings.

## 10.1 The shopkeeper in the middle

At an airport currency booth the board says: *we buy euros at 0.90, we sell euros at 1.10*. The booth does not care whether euros go up or down. It earns the gap, 0.20, every time one customer sells to it and another buys from it. That is **market making**, and the gap is the **spread**.

On an exchange, a market maker (MM) keeps a **bid** (an offer to buy) and an **ask** (an offer to sell) resting in the book all the time. When a seller hits its bid and later a buyer lifts its ask, it has bought low and sold high.

Two things can go wrong:

1. **Inventory risk.** If many sellers come and no buyers, the MM ends up holding a big pile of stock. If the price then falls, the pile loses value. Picture the umbrella shop that bought a hundred umbrellas just before a month of sunshine.
2. **Adverse selection.** Some customers know something the MM does not. They sell to the MM just before bad news and buy just before good news. Each trade with them loses money, however wide the spread. In lobsim, that customer is the informed trader.

A good market maker earns the spread from everyone else and limits the damage from both.

## 10.2 The Avellaneda–Stoikov formulas

In 2008 Marco Avellaneda and Sasha Stoikov worked out, under simplifying assumptions, how a market maker who dislikes risk should quote. lobsim uses their two formulas as SPEC.md section 1 gives them. With

- `s` the mid price the MM sees (ticks),
- `q` its inventory (lots: positive = long, negative = short),
- `γ` (gamma) its **risk aversion** (how much it dislikes uncertainty),
- `σ` (sigma) the price's **volatility** (ticks per square-root second),
- `κ` (kappa) the **order-book liquidity** (how quickly the chance of being filled falls as it quotes further from the mid),
- `T − t` the time left in the session (seconds),

the **reservation price** is

$$r = s - q\,\gamma\,\sigma^2\,(T-t)$$

and the **total spread** (ask distance plus bid distance) is

$$\delta = \gamma\,\sigma^2\,(T-t) + \frac{2}{\gamma}\ln\!\left(1+\frac{\gamma}{\kappa}\right).$$

The quotes sit symmetrically around `r`: bid = r − δ/2, ask = r + δ/2.

**The reservation price in words.** It is the price at which the MM would be equally happy to buy or sell one more lot, *given what it already holds*. If it is long (q > 0), r is **below** the mid: it values more stock less than the market does. Both its quotes move down, so its ask becomes more attractive (it sells more) and its bid less attractive (it buys less), which pushes its inventory back toward zero. Short is the mirror image. The size of the lean is `q γ σ² (T − t)`: more inventory, more risk aversion, a jumpier price, or more time left to be stuck with it all mean a bigger lean.

**The spread in words.** The first term, `γ σ² (T − t)`, is a risk premium: with a lot of time left, holding inventory is riskier, so quote wider. The second term comes from how fills depend on distance: the model assumes the rate at which an order `δ` ticks from the mid gets filled falls like e^(−κδ). If fills drop off fast with distance (large κ), quoting wide loses too much business, so quote tighter. When γ is tiny, this term is almost exactly 2/κ.

### The numbers in the default config

`mm`: γ = 0.0001, κ = 1.5, σ = 1.0, T = 3,600 s (the run's duration). SPEC.md 5.8 does this sanity check:

- At t = 0: γσ²T = 0.0001 × 1 × 3,600 = **0.36 ticks**, and (2/γ) ln(1 + γ/κ) = 20,000 × ln(1.0000667) ≈ **1.33 ticks**. Total spread about **1.69 ticks** at the start, shrinking to about **1.33** at the end.
- Each lot of inventory moves r by at most **0.36 ticks**, so one 5-lot fill moves it about **1.8 ticks**.

## 10.3 From a formula to real prices

Prices must be whole ticks and the order must be allowed by the exchange. `compute_quotes` in `src/sim/as_market_maker.cpp` (line 18) does it in five steps:

```cpp
Quotes compute_quotes(const MmParams& p, const BBO& b, lob::Qty inventory, TimeNs now, TimeNs end) {
  Quotes out;
  if (!b.two_sided()) return out;
  const double s = static_cast<double>(b.bid + b.ask) / 2.0;
  const double tau = now < end ? static_cast<double>(end - now) / 1e9 : 0.0;
  const double r = reservation_price(p, s, inventory, tau);
  const double half = total_spread(p, tau) / 2.0;

  double bid = std::min(std::floor(r - half), static_cast<double>(b.ask - 1));
  const double ask = std::max(std::ceil(r + half), static_cast<double>(b.bid + 1));
  if (!(bid < ask)) bid = ask - 1.0;

  const double max_price = static_cast<double>(p.max_price);
  if (inventory < p.q_max && bid >= 1.0 && bid <= max_price) out.bid = static_cast<lob::Price>(bid);
  if (inventory > -p.q_max && ask >= 1.0 && ask <= max_price) out.ask = static_cast<lob::Price>(ask);
  return out;
}
```

1. **No view, no quotes.** Without a two-sided BBO there is no mid.
2. **Round outwards.** bid = `floor(r − δ/2)`, ask = `ceil(r + δ/2)`. Rounding away from r keeps the spread at least δ.
3. **Do not cross.** The bid is at most the best ask − 1 and the ask at least the best bid + 1. (Its orders are post-only, so a crossing quote would be rejected anyway.)
4. **Keep bid < ask** (needed only in a rounding corner case, D-091).
5. **Inventory limit.** No bid at all if q ≥ q_max (50), no ask if q ≤ −q_max.

It is a **pure function**: the same inputs always give the same quotes, and it has no hidden state. That makes it easy to test on its own (`tests/unit/sim_as_market_maker_test.cpp`), and easy to copy into Python (Lab 10.1).

**Why the MM quotes about 2.7 ticks wide when the formula says 1.3 to 1.7.** The README works this out. With δ ≈ 1.7 and rounding outwards, the quote is 2 ticks wide only when r sits close to a whole tick, and 3 ticks otherwise. At zero inventory r is the mid, which is either a whole tick or a half tick, but inventory moves r by up to 0.36 ticks per lot, and the MM has some inventory most of the time. Averaged over the hour, its quoted spread is 2.70 ticks against the market's 1.94.

## 10.4 The agent around the formula

`AvellanedaStoikovMM` keeps, for each side (bid and ask):

- `working`: the order it believes is resting (id, price, remaining), or none;
- `pending`: at most **one** request it has sent and not yet had answered (New, Modify or Cancel), or none.

It calls `requote` after every BBO, every private message, and every 100 ms (`requote_interval_ms`). `requote` computes the desired quotes and then, for each side (line 73):

```cpp
void AvellanedaStoikovMM::requote_side(Context& ctx, lob::Side side, std::optional<lob::Price> desired) {
  SideState& st = sides_[idx(side)];
  if (st.pending.kind != PendingKind::None) return;  // never two requests in flight on one side

  if (!desired) {
    if (st.working) { ... send Cancel ... }
    return;
  }
  if (!st.working) { ... send a post-only GTC NewOrder for quote_size at *desired ... return; }
  if (st.working->price == *desired) return;  // also when partially filled

  const lob::Qty exposure = side == lob::Side::Buy ? q_ : -q_;
  if (exposure + p_.quote_size <= p_.q_max) {
    ctx.send(lob::Modify{ctx.self(), st.working->id, *desired, p_.quote_size});
    ...
  } else {
    ctx.send(lob::Cancel{ctx.self(), st.working->id});
    ...
  }
}
```

Per side: nothing should be quoted → cancel; nothing is resting → new order; resting at the wrong price → reprice (modify, or near the inventory limit, cancel and let the next requote send a new order); already right → do nothing.

### The pending-request rule

The first line is the most important one: **never send a request for a side while an earlier request on that side is unanswered** (SPEC.md 5.6). Remember the MM's view is always late. Without this rule, it could send "move my bid to 100", then, before hearing back, "move my bid to 101", and its idea of where its order is would drift away from the truth. This is called the stale-state bug, and it is a classic in real trading systems.

The rule works because of facts about lobsim's exchange, written in the header:

- every request gets **exactly one** reply: New → `Ack` or `Reject`; Modify → `Modified` or `Reject`; Cancel → `Cancelled(User)` or `Reject`;
- **fills are not replies**: a `Trade` changes inventory and the working order, but never clears `pending`;
- all private events for one owner arrive **in exchange order** (one FIFO response channel), so a fill that beat a modify to the exchange arrives before the modify's reply. A `Reject(UnknownOrder)` to a modify or cancel therefore means "your order was already completely filled", and those fills have already been seen.

The reply handlers (`on_ack`, `on_reject`, `on_modified`, `on_cancelled`, `on_fill`, lines 148 to 226) match each reply to the pending request by client id or order id and kind. Anything that matches nothing is counted in `anomalies()`, and never crashes the agent. In a correct run that counter is 0 (`lobsim_run` prints `mm anomalies 0`).

### The refill race, and why near the limit it cancels instead

A `Modify` sets the order's **remaining** quantity (Module 2.5). Suppose the bid has 5 lots resting and the MM sends "move to 101, 5 lots". On the way, a seller fills 4 of the old 5. The exchange applies the modify to the 1 lot left and sets it back to 5: the MM's side is **refilled** with lots it has not yet seen being bought. If that happens near the inventory limit, inventory can overshoot.

The comment above the `exposure` line (`as_market_maker.cpp`, lines 95 to 109) works out the bound: always modifying would allow |q| up to `q_max + 2 × quote_size − 2`. So the MM modifies only when even a full extra quote cannot break the limit (`exposure + quote_size <= q_max`); otherwise it cancels, waits for the reply, and sends a fresh order from the next requote. That bounds |q| by `q_max + quote_size − 1`, inside SPEC.md's requirement of `q_max + quote_size`. DECISIONS.md D-092 records this as a departure from the spec, with its reason.

`tests/unit/sim_mm_integration_test.cpp`, `MarketMakerStress.ProbeChecksHoldUnderRacesAcrossSeeds`, drives the MM with an aggressive test trader, q_max = 10, quote size 5 and heavy jitter, over six seeds, and an outside observer checks every rule. It requires the bound `max_abs_q <= 10 + 5 - 1` **and** requires that the races really happened (modifies raced by full fills, cancels raced by full fills, modifies after unseen partial fills), so the test cannot pass by being too gentle.

**Skipped on purpose:** the reply handlers all follow the same pattern (find the matching pending request, clear it, update `working`, count anomalies); read `on_modified` (line 187) as the example. The MM never draws random numbers.

## 10.5 What the market maker achieved (README, default run, seed 42)

| Measure (after the 60 s warm-up) | Value |
|---|---|
| Fills | 7,577 (3,755 buys, 3,822 sells), 26,670 lots |
| Share of all traded volume | 17%, always as maker |
| Mark-to-market PnL | +12,263 ticks (+0.46 per lot) |
| Inventory at the end; largest | −34; 51 lots |
| Time with both a bid and an ask resting | 99.93% |
| Average quoted spread | 2.70 ticks (market average 1.94) |
| Anomalies | 0 |

The README's own honest reading: "The market maker makes money because most of its counterparties are zero-intelligence traders who pay the spread for no reason. ... It is not evidence that this strategy earns money in a real market." Module 11 measures exactly how much the informed trader costs it.

## Lab 10.1: compute the quotes by hand

```sh
python3 course/labs/py/as_quotes.py
```

It is a copy of `compute_quotes`. The first line checks it against the real run: in the default run, the MM's very first view of the market was bid 9990 (a noise trader), ask 10003 (another noise trader), with no inventory, at about 0.021 s. As printed:

```
first quote: mid 9996.5, r 9996.5000, half spread 0.8466 -> bid 9995, ask 9998
orders.log says: 21120731 N 12 1 0 0 0 9995 5 1 0  and  21120731 N 12 2 1 0 0 9998 5 1 0
```

Do it on paper first: mid = (9990 + 10003)/2 = 9996.5; τ ≈ 3,600 s; half spread ≈ (0.36 + 1.333)/2 ≈ 0.847; bid = floor(9995.653) = 9995; ask = ceil(9997.347) = 9998. The formula, a Python copy and the C++ simulator agree to the tick.

Then read the two tables it prints:

```
Same book (bid 9999, ask 10001), inventory q changes, start of the hour:
  q =  -20: r =  10007.20  bid 10000  ask 10009
  q =   -5: r =  10001.80  bid 10000  ask 10003
  q =    0: r =  10000.00  bid 9999  ask 10001
  q =    5: r =   9998.20  bid 9997  ask 10000
  q =   20: r =   9992.80  bid 9991  ask 10000
  q =   50: r =   9982.00  bid None  ask 10000

q = 10 lots long, as the hour runs out (the inventory penalty shrinks):
  t =    0 s: r =  9996.400  total spread 1.693  bid 9995  ask 10000
  t = 1800 s: r =  9998.200  total spread 1.513  bid 9997  ask 10000
  t = 3300 s: r =  9999.700  total spread 1.363  bid 9999  ask 10001
  t = 3590 s: r =  9999.990  total spread 1.334  bid 9999  ask 10001
```

1. At q = −20, the formula wants a bid of floor(10007.2 − 0.85) = 10006. Why does the MM bid 10000?
2. At q = +20, why is the ask 10000 and not 9994?
3. At q = 50, why is there no bid?
4. In the second table, why does a 10-lot long position barely move the quotes at the end of the hour?

<details><summary>Answers</summary>

1. The clamp: a bid may be at most the best ask − 1 = 10000. Short 20 lots, the MM badly wants to buy back, so it bids as high as it can without crossing.
2. The other clamp: an ask must be at least the best bid + 1 = 10000. Long 20 lots, it wants to sell cheaply, but 9994 would cross the bid at 9999.
3. q ≥ q_max: it stops buying altogether.
4. The lean is q γ σ² (T − t): with 10 s left it is 10 × 0.0001 × 10 = 0.01 ticks. With little time left, holding inventory is not considered risky, so the quotes stop leaning.

</details>

## Lab 10.2: the market maker chases its own tail

Trace the first 40 requests of the default run:

```sh
build/release/labs/lab08_trace_log out/run1/orders.log 40 | less
```

(Build it with `course/labs/build_lab.sh course/labs/lab08_trace_log.cpp` if you skipped Module 8.) After the first trade (the MM sold 3 lots at 9998, so q = −3) you will see the MM's bid and ask (orders 3 and 4) modified upward again and again within one millisecond:

```
21345475 M 12 3 9997 5
21458022 M 12 3 9998 5
21458022 M 12 4 10001 5
21566784 M 12 3 9999 5
21569728 M 12 4 10002 5
...
21903398 M 12 3 10002 5
22010967 M 12 4 10005 5
```

and then nothing from the MM until a noise trader's market sell at 41.78 ms hits its bid at 10002. Explain:

1. Why does each requote move both quotes up by one tick? (Hint: short 3 lots, how far above the mid is r? And whose orders are the best bid and best ask?)
2. Why does the climb stop at bid 10002, ask 10005?
3. Look at `21235939 M 12 4 10000 5`, the first modify of the ask. The ask had 2 lots left after the first trade. How many lots does it have after this modify?
4. Why do lobsim's statistics ignore the first 60 seconds of every run?

<details><summary>Answers</summary>

1. With q = −3 and τ ≈ 3,600 s, r = mid + 3 × 0.36 = mid + 1.08. In the empty early book the MM's own bid and ask *are* the best bid and ask, so the mid it sees is the middle of its own quotes. Centring the quotes on r moves them up a tick, which moves the mid up a tick, which moves r up a tick: it is chasing itself. Example: own quotes 9997/10000, mid 9998.5, r = 9999.58, new bid floor(9998.73) = 9998, new ask ceil(10000.43) = 10001.
2. Once its ask rises above 10003, a noise trader's ask at 10003 becomes the best ask. With bid 10002 (its own) and ask 10003, the mid is 10002.5 and r = 10003.58; the bid can be at most 10003 − 1 = 10002 (unchanged) and the ask is ceil(10004.43) = 10005 (unchanged). A fixed point: no more changes.
3. Five. A modify sets the remaining quantity: this is the refill of 10.4, harmless here because the MM is far from its limit.
4. The start of a run is not representative: the book is nearly empty and odd things like this happen. The 60 s warm-up (`warmup_s`) lets the book fill up first.

</details>

## Lab 10.3: watch inventory control fade

```sh
python3 course/labs/py/mm_inventory.py out/run1
```

When this course was written:

```
window (s)      mean |q|   max |q|   penalty per lot at the window start (ticks)
   60-600          1.5         7   0.354
  600-1200         1.7         6   0.300
 1200-1800         2.1         5   0.240
 1800-2400         2.4         7   0.180
 2400-3000         2.9        10   0.120
 3000-3300         4.6        13   0.060
 3300-3600        11.9        51   0.030
```

The inventory penalty γσ²(T − t) shrinks to almost nothing as the end approaches, so the MM stops leaning against its position and lets it grow. That is what the A–S formulas say to do when the session is about to end (the model assumes the position no longer matters after T), and the README points out it is the reason the largest inventory, 51 lots, is reached at 3,561 s.

## Lab 10.4: turn the knobs

Copy `configs/default.json` and change one value in the `mm` section at a time. Predict the direction of each column before you run. When this course was written (one hour, seed 42):

| Change | Quoted spread | MM lots | MM PnL | PnL per lot | Largest \|q\| | Mean \|q\| |
|---|---|---|---|---|---|---|
| none | 2.70 | 26,670 | 12,263 | 0.460 | 51 | 3.2 |
| `gamma` × 10 (0.001) | 4.44 | 9,242 | 1,239 | 0.134 | 26 | 0.7 |
| `gamma` ÷ 10 (0.00001) | 2.15 | 31,602 | 19,735 | 0.624 | 54 | 10.5 |
| `kappa` 0.5 | 5.47 | 211 | 331 | 1.571 | 10 | 1.0 |
| `kappa` 5 | 1.85 | 29,139 | 2,956 | 0.101 | 38 | 2.8 |
| `q_max` 10 | 2.71 | 26,388 | 12,022 | 0.456 | **14** | 2.7 |

(Mean |q| is from the one-second samples after the warm-up.)

1. Explain the `gamma` rows using both terms of the spread formula and the reservation price.
2. Why does `kappa` 0.5 trade almost nothing?
3. Look at the largest |q| with `q_max` 10. Which bound from 10.4 is it exactly equal to?
4. The least risk-averse market maker earned the most here. Does that mean lower γ is better?

<details><summary>Answers</summary>

1. Bigger γ: the first term (0.36 → 3.6 ticks at the start) and the inventory lean (3.6 ticks per lot) are 10 times bigger, so quotes are wide and lean hard, it trades less and holds almost nothing. Smaller γ: both are tiny, the spread is about 2/κ ≈ 1.33, the quotes stay close to the mid and symmetric, so it trades more and earns close to a full tick per lot, but lets inventory wander (mean 10.5 lots).
2. 2/κ = 4 ticks: its quotes sit about 2.7 ticks on each side of the mid, far behind the noise traders' orders, and market orders of at most 10 lots rarely reach them.
3. `q_max + quote_size − 1` = 10 + 5 − 1 = 14, the bound proved in the code comment.
4. Not in general. It carried far more inventory, so it was exposed to big price moves; on this seed and in this market (driftless fundamental, mostly random counterparties) that risk did not hurt. A fair comparison needs many seeds (`tools/sweep.py` runs 89 hours of variants) and, ideally, a harder market.

</details>

## Lab 10.5: read the stress test

Open `tests/unit/sim_mm_integration_test.cpp` and read `MarketMakerStress.ProbeChecksHoldUnderRacesAcrossSeeds` (line 410). Then run it:

```sh
build/release/sim_tests --gtest_filter='MarketMakerStress.*'
```

It prints one line per seed with counts such as `modify/full-fill` and `fill/other-side-pending`. Which `EXPECT_GT` lines make sure the test is really stressful, and why would a test without them be weaker?

## Common questions

**Is the A–S market maker what real firms use?** It is the classic textbook starting point. Real firms add many things: forecasts of short-term price moves, queue position, fees, competition with other market makers, risk limits across many stocks.

**Why post-only orders?** So the MM is always the maker: it never pays the spread by accident, and in the default config makers get a small rebate (`maker_fee_ticks: -0.002`, a negative fee). If its view is stale and its quote would cross, the exchange rejects it instead of trading.

**Why does the MM requote every 100 ms as well as on every message?** Time matters in the formula (T − t changes), and a timer guarantees it reacts even when the market is quiet.

**What does τ mean in the code?** `tau`, the time left, T − t, in seconds.

**Does σ in the MM's config have to match the fundamental's?** No. It is the MM's own assumption about volatility. The default sets both to 1 tick per √s, but the mid's actual volatility is different (Module 11 measures it).

## Quiz

1. Name the two main risks of a market maker and give an everyday example of each.
2. The MM is short 10 lots. Is its reservation price above or below the mid? Which of its quotes becomes more attractive to other traders?
3. Compute the default total spread with 900 s left.
4. Mid 5000.5, q = 0, half spread 0.8466, best bid 5000, best ask 5001. What does the MM quote?
5. State the pending-request rule. What clears a pending request? What does not?
6. Why does the MM cancel instead of modify when it is near its inventory limit?

<details><summary>Answers</summary>

1. Inventory risk (an umbrella shop stuck with stock before a sunny month) and adverse selection (buying a card from a kid who knows it is about to be banned).
2. Above (r = s − qγσ²τ with q negative). Its bid becomes more attractive (higher), so it buys back.
3. 0.0001 × 900 + 1.3333 ≈ 1.42 ticks.
4. bid = floor(5000.5 − 0.8466) = floor(4999.65) = 4999; ask = ceil(5001.35) = 5002. Clamps: bid ≤ 5000 and ask ≥ 5001 are already satisfied. Quotes 4999 / 5002.
5. Never send a request for a side while an earlier request on that side is unanswered. Only its one reply clears it (`Ack`/`Reject` for a new order, `Modified`/`Reject` for a modify, `Cancelled(User)`/`Reject` for a cancel). Fills (`Trade`) never clear it.
6. A modify resets the remaining quantity and can arrive after fills the MM has not seen, refilling the order. Near the limit that could push inventory past `q_max + quote_size − 1`; a cancel followed by a fresh order sent only after the cancel's reply cannot.

</details>

## What's next

Module 11 is the scorekeeper: how lobsim measures profit, whether the market maker was picked off by the informed trader, and whether the simulated market behaves like a real one.
