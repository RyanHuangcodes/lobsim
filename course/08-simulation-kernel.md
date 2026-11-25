# Module 8: The simulation clock

> **In one sentence:** the simulator keeps a to-do list sorted by time, always does the earliest item next, and makes every message between traders and the exchange arrive a little late.
>
> **You will be able to:** explain discrete-event simulation, read lobsim's event queue and its single tie-break rule, explain latency channels and why they are FIFO, describe the agent interface and the exchange, walk through the kernel's main loop, and read the start of an `orders.log` as a story.
>
> **Time:** about 4 hours.

## 8.1 Jumping from appointment to appointment

There are two ways to simulate time.

- **Time-stepped:** advance the clock by a fixed step (say 1 millisecond) and ask everyone "anything to do?". Simple, but wasteful when nothing happens, and anything faster than one step gets squashed together.
- **Discrete-event:** keep a list of things that will happen and when. Take the earliest, jump the clock straight to its time, do it. Doing it may add new items to the list ("my order will arrive at the exchange 200 µs from now"). Repeat.

lobsim is discrete-event (SPEC.md 5.1). Its clock is an `int64_t` count of nanoseconds since the start (`sim::TimeNs`). There is **no wall-clock time** anywhere in the simulation: one simulated hour takes about half a second of real time, and that real time is never written into any output (only `lobsim_run` prints it at the very end, to the screen).

## 8.2 The event queue and the one tie-break rule

`include/sim/event_queue.hpp`:

```cpp
enum class EventKind : std::uint8_t {
  Wakeup,              // an agent's timer
  RequestArrival,      // a request reaches the exchange (agent = sender)
  PrivateDelivery,     // an engine event reaches its owner (agent = receiver)
  MarketDataDelivery,  // public data reaches a subscriber (agent = receiver)
  FundamentalStep,
  Sample,
};

struct SimEvent {
  TimeNs time{0};
  std::uint64_t seq{0};  // global counter, incremented on every schedule()
  EventKind kind{EventKind::Wakeup};
  AgentIndex agent{0};
  Payload payload;
};
```

Six kinds of thing can happen. Each scheduled event gets a `seq` number from a counter that goes up by one every time *anything* is scheduled.

```cpp
class EventQueue {
 public:
  void schedule(TimeNs time, EventKind kind, AgentIndex agent, Payload payload = {}) {
    if (time < now_) throw std::logic_error("EventQueue: scheduling in the past");
    heap_.push_back(SimEvent{time, next_seq_++, kind, agent, std::move(payload)});
    std::push_heap(heap_.begin(), heap_.end(), later);
  }
  SimEvent pop() { ... }
 private:
  static bool later(const SimEvent& a, const SimEvent& b) noexcept {
    return a.time != b.time ? a.time > b.time : a.seq > b.seq;
  }
  ...
};
```

The queue is a **binary heap**: an arrangement inside a vector that always keeps the smallest item at the front, with adding and removing in O(log n). `std::push_heap` and `std::pop_heap` maintain it; `later` says which of two events comes later.

Read `later` carefully: **earlier time first; at equal times, the one scheduled first (smaller `seq`) first.** That is the simulator's **only** tie-break rule. Not the kind of event, not the agent. SPEC.md insists on this because of an earlier simulator, ABIDES, whose results other teams could only reproduce after reverse-engineering its undocumented ordering of simultaneous events. In lobsim the rule is one line, written down in the code, the README and DECISIONS.md D-070, and tested.

Scheduling into the past throws an error: a simulator that goes back in time has a bug.

## 8.3 Latency: every message is late

Each agent has three one-way delays (`include/sim/latency.hpp`, SPEC.md 5.3):

| Channel | Direction | Carries |
|---|---|---|
| entry | agent → exchange | the agent's requests |
| response | exchange → that agent | its private events: `Ack`, `Reject`, `Trade`, `Cancelled`, `Modified` |
| feed | exchange → every agent | public data: `BBO` and `TradePrint` |

Each delay is a constant plus a random **jitter**, uniform in `[0, jitter]`. The defaults: noise traders 200 µs ± up to 50 µs, the informed trader 100 µs ± 20, the market maker 50 µs ± 10. The market maker is the fastest, like a real trading firm that pays to be close to the exchange.

```cpp
class Channel {
 public:
  TimeNs arrival(TimeNs now, Xoshiro256StarStar& latency_rng) noexcept {
    const TimeNs sampled = now + base_ + (jitter_ > 0 ? latency_rng.uniform_int(0, jitter_) : 0);
    last_arrival_ = std::max(sampled, last_arrival_);
    return last_arrival_;
  }
 private:
  TimeNs base_{0};
  TimeNs jitter_{0};
  TimeNs last_arrival_{0};
};
```

The `std::max` makes each channel **FIFO**: a message never arrives before an earlier message on the same channel. Real network connections behave this way (TCP keeps order), and it matters for correctness: without it, a trader could receive a fill *before* the `Ack` of the order that was filled. If a message is sampled to arrive earlier than the previous one, it is held back to arrive at the same instant, and since it was scheduled second, the tie-break rule delivers it second.

One latency generator per agent is shared by its three channels, and the jitter is drawn at the moment a message is sent (D-081).

## 8.4 What an agent is allowed to do

`include/sim/agent.hpp`:

```cpp
class Agent {
 public:
  virtual ~Agent() = default;
  virtual void on_start(Context& ctx) = 0;
  virtual void on_wakeup(Context& ctx) = 0;
  virtual void on_market_data(Context& ctx, const MarketData& msg) = 0;
  virtual void on_private(Context& ctx, const lob::Event& event) = 0;
};

class Context {
 public:
  virtual TimeNs now() const = 0;
  virtual void send(const lob::Request& request) = 0;
  virtual void schedule_wakeup(TimeNs at) = 0;
  virtual Xoshiro256StarStar& rng() = 0;
  virtual lob::OwnerId self() const = 0;
  ...
};
```

An `Agent` is a class with four functions the simulator calls: at the start, when its alarm rings, when public data arrives, and when a private message about its own orders arrives. Through the `Context` it can ask the time, send a request, set an alarm, get its random stream and learn its owner id. **That is all.** There is no function to look at the book. SPEC.md 5.6: "Agents never read the exchange's book directly."

`virtual ... = 0` makes these **abstract** classes: they describe a shape that other classes fill in. `NoiseTrader`, `InformedTrader`, `AvellanedaStoikovMM` and `CalibratedFlowAgent` each `override` the four functions. Because `Context` is abstract too, a test can hand an agent a fake context that just records what it sends (`tests/support/fake_context.hpp`, `FakeCtx`) and drive it without any simulator. Most agent tests do exactly that.

**`TimerQueue`** (`include/sim/timer_queue.hpp`): `on_wakeup` does not say *which* alarm rang. A noise trader has two kinds (time to trade, time to cancel order 17), so it keeps its own little heap of timers ordered the same way as the event queue, and each `on_wakeup` pops exactly the one that is due. A wakeup with no matching timer throws: a lost or duplicate wakeup is a bug, not something to ignore (D-073).

## 8.5 The exchange

`src/sim/exchange.cpp`, `Exchange::process` (line 37), runs the instant a request arrives:

```cpp
const ExchangeOutput& Exchange::process(TimeNs now, lob::OwnerId sender, const lob::Request& request) {
  const lob::Request req = with_owner(request, sender);
  if (log_ != nullptr) { ... append "time + request" as one line to orders.log ... }
  ++requests_;

  const BBO before = top_;
  out_.events.clear();
  out_.public_data.clear();
  lob::apply(book_, req, out_.events);

  for (const lob::Event& ev : out_.events) {
    hasher_.add(ev);
    ++events_;
    if (const auto* t = std::get_if<lob::Trade>(&ev)) {
      ++trades_;
      out_.public_data.emplace_back(TradePrint{t->price, t->qty, t->aggressor_side});
    }
  }

  top_ = top_of_book(book_);
  // A crossed book is an engine bug. Checked in every build type, not only in Debug.
  if (top_.two_sided() && top_.bid >= top_.ask) throw std::logic_error("Exchange: the book is crossed");
  if (top_.two_sided()) last_mid_ = static_cast<double>(top_.bid + top_.ask) / 2.0;

  out_.top = top_;
  out_.top_changed = !(top_ == before);
  if (out_.top_changed) out_.public_data.emplace_back(top_);
  return out_;
}
```

Step by step:

1. `with_owner` overwrites the owner on the request with the sender's id. An agent cannot place or cancel orders in someone else's name.
2. The request is written to `orders.log` **before** it is applied, so the log is the exact input of the engine.
3. The engine applies it and every event goes into the **event hash** (Module 12).
4. Each trade becomes a public `TradePrint` (price, size, aggressor side; no names).
5. The new top of book is read. A crossed book throws, even in release builds (D-080).
6. If any of the four top-of-book fields changed, one `BBO` message is added after the trade prints.

## 8.6 The kernel: putting it together

`src/sim/simulator.cpp`. The constructor creates the agents in a fixed order, because agent order fixes the `seq` of everything scheduled at time 0: ten noise traders, then the informed trader, then the market maker (each only if its config section exists), then the calibrated background flow (Module 14). An agent's owner id is its position + 1, so in the default run the noise traders are owners 1 to 10, the informed trader is 11 and the market maker is 12.

`run()` (line 210):

```cpp
  now_ = 0;
  for (auto& slot : agents_) slot->agent->on_start(*slot->ctx);
  queue_.schedule(cfg_.fundamental_dt_ns, EventKind::FundamentalStep, 0);
  queue_.schedule(cfg_.sample_interval_ns, EventKind::Sample, 0);

  while (!queue_.empty() && queue_.top().time < end) {
    const SimEvent ev = queue_.pop();
    now_ = ev.time;
    ++processed_;
    switch (ev.kind) {
      case EventKind::Wakeup: { ... slot.agent->on_wakeup(*slot.ctx); break; }
      case EventKind::RequestArrival: handle_request(ev); break;
      case EventKind::PrivateDelivery: { ... slot.agent->on_private(*slot.ctx, event); break; }
      case EventKind::MarketDataDelivery: { ... slot.agent->on_market_data(*slot.ctx, ...); break; }
      case EventKind::FundamentalStep:
        fundamental_.step();
        ...
        ++fundamental_k_;
        queue_.schedule(static_cast<TimeNs>(fundamental_k_) * cfg_.fundamental_dt_ns, EventKind::FundamentalStep, 0);
        break;
      case EventKind::Sample: ...
    }
  }
```

- Every agent's `on_start` runs at time 0, in agent order.
- The loop takes events **strictly before** the end time; an event at exactly the end is never processed.
- A periodic event is rescheduled at `k × period` from an integer counter `k`, not "now + period". Adding a period over and over could drift if the period were not a whole number of nanoseconds; multiplying an integer never does.

`handle_request` (line 168) is where the exchange's answers become deliveries:

```cpp
  const ExchangeOutput& out = exchange_.process(now_, sender, request);

  for (const lob::Event& event : out.events) {
    for (Observer* o : observers_) o->on_engine_event(now_, event);
    if (const auto* t = std::get_if<lob::Trade>(&event)) {
      deliver_private(t->maker_owner, event);
      // Once per agent: one agent may be both sides of a trade under two of its owner ids.
      if (agent_of_owner(t->taker_owner) != agent_of_owner(t->maker_owner)) deliver_private(t->taker_owner, event);
    } else {
      ... deliver_private(owner of the event, event);
    }
  }
  // Public data: message-major, receivers in agent order.
  for (const MarketData& md : out.public_data) {
    for (AgentIndex i = 0; i < agents_.size(); ++i) {
      Slot& slot = *agents_[i];
      queue_.schedule(slot.feed.arrival(now_, slot.latency_rng), EventKind::MarketDataDelivery, i, md);
    }
  }
```

A `Trade` goes to both owners (maker first). Every other event goes to its one owner. Each public message is scheduled for **every** agent, through that agent's own feed channel. `deliver_private` uses the receiver's response channel. Notice `slot.latency_rng`: every market-data delivery rolls the receiving agent's latency dice, which is why the number of rolls depends on how busy the market is (Module 7.6).

## 8.7 Observers: watching without touching

`include/sim/observer.hpp`. An `Observer` is told about everything as it happens, with zero delay: each request as it arrives, each engine event, the true top of book after each request, each private delivery, each fundamental step, each sample tick and the end. The scorekeeper of Module 11 (`MetricsObserver`) is an observer.

The rule, in a comment in the file: an observer **must not** schedule events, send requests or draw random numbers, "so attaching or removing one can never change the simulation". `tests/determinism.cpp` checks it: the event hash and `orders.log` are the same with and without the metrics observer.

## Lab 8.1: the queue and a channel

```sh
course/labs/build_lab.sh course/labs/lab08_event_queue.cpp
build/release/labs/lab08_event_queue
```

Part 1 schedules six events with times 300, 100, 300, 200, 300, 100 (in that order of scheduling). Write down the order you expect them to come out, with their `seq`. Expected:

```
Part 1: the event queue
  time 100  seq 1  Sample              agent 0
  time 100  seq 5  MarketDataDelivery  agent 5
  time 200  seq 3  FundamentalStep     agent 0
  time 300  seq 0  Wakeup              agent 2
  time 300  seq 2  RequestArrival      agent 1
  time 300  seq 4  PrivateDelivery     agent 0
```

Part 2 sends ten messages 50 µs apart through one channel with 100 µs base delay and up to 500 µs of jitter:

```
  message 5 sent at  250 us, arrives at  692 us
  message 6 sent at  300 us, arrives at  895 us
  message 7 sent at  350 us, arrives at  895 us
  message 8 sent at  400 us, arrives at  895 us
  message 9 sent at  450 us, arrives at  895 us
```

Why do messages 7, 8 and 9 all arrive at 895 µs? In which order will they be delivered?

<details><summary>Answer</summary>

Message 6 drew a large jitter and is due at 895 µs. Messages 7 to 9 drew arrival times earlier than that, but a channel never lets a message overtake an earlier one, so each was held back to 895 µs (`max(sampled, last_arrival)`). They were scheduled in the order 6, 7, 8, 9, so the tie-break rule delivers them in that order. One slow message delays everything behind it: "head-of-line blocking", as on a real network connection.

</details>

## Lab 8.2: read the first 22 milliseconds of a run

```sh
course/labs/build_lab.sh course/labs/lab08_trace_log.cpp
build/release/labs/lab08_trace_log out/run1/orders.log 12
```

This replays the first 12 requests of the default run and prints the engine's answers. Each request line is `<time_ns> N <owner> <client_order_id> <side> <type> <tif> <price> <qty> <post_only> <stp>` for a new order (side 0 = Buy, 1 = Sell; type 0 = Limit, 1 = Market; tif 0 = GTC, 1 = IOC, 2 = FOK), `<time_ns> C <owner> <order_id>` for a cancel and `<time_ns> M <owner> <order_id> <new_price> <new_qty>` for a modify. The start, as printed:

```
14113737 N 2 1 0 0 0 9990 2 0 0
    Ack{order_id=1, owner=2, client_order_id=1}
21005236 N 7 1 1 0 0 10003 4 0 0
    Ack{order_id=2, owner=7, client_order_id=1}
21120731 N 12 1 0 0 0 9995 5 1 0
    Ack{order_id=3, owner=12, client_order_id=1}
21120731 N 12 2 1 0 0 9998 5 1 0
    Ack{order_id=4, owner=12, client_order_id=2}
21168497 N 5 1 0 1 1 0 3 0 0
    Ack{order_id=5, owner=5, client_order_id=1}
    Trade{trade_id=1, price=9998, qty=3, maker_order_id=4, taker_order_id=5, maker_owner=12, taker_owner=5, aggressor_side=Buy}
21235007 M 12 3 9998 5
    Reject{owner=12, request_kind=Modify, client_order_id=0, order_id=3, reason=PostOnlyWouldCross}
```

Answer in your notebook:

1. Who is owner 2, owner 7, owner 12 and owner 5?
2. At what simulated time (in milliseconds) does the first order arrive?
3. The 3rd and 4th requests are the market maker's first bid and ask. They arrived at exactly the same nanosecond. Why is that likely not a coincidence?
4. The 5th request is a market buy. Whose order did it trade with, and at what price?
5. The 6th request was refused with `PostOnlyWouldCross`. The market maker tried to move its bid to 9998. Why would that cross, and why did the market maker think it was a good idea? (Hint: when it decided, which messages had it received?)

<details><summary>Answers</summary>

1. Owners are agent index + 1 in creation order: owners 2, 7 and 5 are noise traders (indices 1, 6 and 4), owner 12 is the market maker.
2. 14,113,737 ns ≈ 14.1 ms.
3. The market maker sends both quotes at the same moment. The second one was probably sampled to arrive slightly earlier than the first and was held back by the FIFO rule to the same arrival time, then delivered second because it was scheduled second.
4. With order 4, the market maker's ask, at 9998, the maker's price. The market maker sold 3 lots and is now short 3.
5. The market maker's own ask was resting at 9998 (2 lots left), and a post-only bid at 9998 would trade with it. It decided on an **old** view of the book: a BBO message showing its own new bid at 9995 as the best bid and the noise trader's ask at 10003 as the best ask (mid 9999), sent before its own ask at 9998 had reached the book. By the time its modify arrived, the book had changed. This is the only `PostOnlyWouldCross` reject of the whole hour (the run's summary counts exactly 1), and Module 10 shows how the market maker handles it.

</details>

## Lab 8.3: where do 2.6 million events come from?

`lobsim_run` printed `simulator events 2589842` for 331,640 requests. That is almost 8 events per request. Before reading the answer, estimate each kind using numbers you have: 12 agents; 387,289 engine events, 51,653 of them trades; noise traders wake 5 times a second, the informed trader 2, the market maker every 100 ms; the fundamental steps every 0.1 s; one sample a second; and each change of the top of book is one public message.

<details><summary>The exact counts for the default run (measured with a small counting observer)</summary>

| Kind | Count | Why |
|---|---|---|
| Request arrivals | 331,640 | one per request |
| Private deliveries | 438,942 | one per engine event, plus a second copy of each of the 51,653 trades (maker and taker) |
| Market-data deliveries | 1,406,316 | 117,193 public messages (51,653 trade prints + 65,540 changes of the top of book) × 12 agents |
| Wakeups | 373,346 | trading wakeups (about 10 × 5 × 3,600 + 2 × 3,600), the market maker's 36,000 requote alarms, and one cancel timer per accepted noise-trader limit order |
| Fundamental steps | 35,999 | every 0.1 s; the one at exactly 3,600 s is not processed |
| Samples | 3,599 | once a second from 1 s to 3,599 s; the one due at exactly 3,600 s is not processed (the metrics observer takes that last sample itself, in `on_end`) |

Market data is more than half of all the work: every public message is delivered once to every agent.

</details>

## Common questions

**Why nanoseconds?** Latencies are microseconds with jitter; nanoseconds give room for fine differences and still fit any realistic run in 64 bits (a run may last up to 10^9 seconds, D-083).

**Two events at the same time: does it matter which is first?** Yes, it can change who trades. That is exactly why there is one written rule (scheduling order) instead of whatever a container happens to do.

**Why does the exchange log the request before applying it?** So `orders.log` holds exactly what went *into* the engine. Replaying it reproduces the output. If it logged after, a request that made the engine throw would be missing.

**Does the market maker see its own quotes in the BBO?** Yes: the BBO is the true top of book, whoever's orders are there, delivered late. Lab 8.2 shows how that can mislead it.

**Can an agent send a request "now" and have it arrive "now"?** Only if its entry latency and jitter are 0. The calibrated background flow of Module 14 runs with zero latency by default.

## Quiz

1. What is the difference between time-stepped and discrete-event simulation?
2. State the simulator's tie-break rule. What does it not depend on?
3. What does `std::max(sampled, last_arrival_)` guarantee?
4. Which three channels does each agent have, and what goes through each?
5. Name the four functions every agent implements. Which one may draw from `ctx.rng()`?
6. Why does `Exchange::process` call `with_owner`?
7. What is an observer forbidden to do, and why?

<details><summary>Answers</summary>

1. Time-stepped advances the clock by a fixed step and checks everyone; discrete-event jumps straight to the time of the next scheduled event.
2. Earlier time first; at equal times, the event scheduled first. It does not depend on the kind of event or on the agent.
3. A message never arrives before an earlier message on the same channel (per-channel FIFO).
4. Entry (requests to the exchange), response (private events about its own orders) and feed (public `BBO` and `TradePrint` data).
5. `on_start`, `on_wakeup`, `on_market_data`, `on_private`. Only `on_start` and `on_wakeup` may draw from the decision stream `ctx.rng()`.
6. So an agent cannot act in another owner's name: the exchange always uses the sender's own id.
7. Schedule events, send requests or draw random numbers. Any of those would change the run, and metrics must be able to watch without changing what they measure.

</details>

## What's next

Module 9 meets the traders who live in this world: random noise traders, the hidden true value, and the informed trader who can see it.
