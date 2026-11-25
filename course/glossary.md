# Glossary

Every special word used in the course, in alphabetical order, with the module where it is explained.

| Word | Meaning | Module |
|---|---|---|
| **Ack** | The engine's reply "your new order is accepted, here is its order id". Always the first event for a new order. | 2 |
| **Adverse selection** | Trading with someone who knows more than you, so your trades tend to happen just before the price moves against you. | 10, 11 |
| **Agent** | A simulated trader: a class with `on_start`, `on_wakeup`, `on_market_data` and `on_private`. | 8, 9 |
| **Aggressor** | The taker: the arriving order that trades against a resting one. `Trade.aggressor_side` is its side. | 1, 2 |
| **AddressSanitizer (ASan)** | A compiler option that adds checks around memory accesses and stops the program at the first memory error. | 5, 6 |
| **Ask** (offer) | A resting sell order. The best ask is the lowest one. | 1 |
| **Autocorrelation** | How much a series moves together with itself k steps later: +1 always together, −1 always opposite, 0 unrelated. | 11 |
| **Avellaneda–Stoikov** | A 2008 model of how a risk-averse market maker should quote: a reservation price and a total spread. | 10 |
| **BBO** | Best bid and offer: the top of the book, with sizes. Also lobsim's public market-data message. | 1, 8 |
| **Bid** | A resting buy order. The best bid is the highest one. | 1 |
| **Bid-ask bounce** | Prices ticking up and down as trades alternate between the bid and the ask, which makes returns negatively autocorrelated. | 11 |
| **Big-O** | How the work of an algorithm grows with the size of its input: O(1) constant, O(log n), O(n). | 5 |
| **Bootstrap** | Estimating uncertainty by resampling the data many times; a moving-block bootstrap resamples whole time blocks. | 14 |
| **Burst** | In LOBSTER data, a run of execution messages with the same time and direction, read as one aggressive order. | 13 |
| **Calibration** | Measuring a model's inputs from data, here the background order flow. | 14 |
| **Canary** | A deliberately injected bug that the tests must catch, proving the tests work. | 6, 13 |
| **Cancelled** | The event "this order (or the rest of it) is gone", with a reason: User, IOCRemainder, NoLiquidity, FOKUnfillable, SelfTrade. | 2 |
| **Censored** | A measurement where only a lower bound is known, such as the lifetime of an order that was filled before anyone cancelled it. | 14 |
| **CI** | Continuous integration: building and testing automatically on every change. | 15 |
| **CMake** | The tool that turns `CMakeLists.txt` into build commands. | 3 |
| **Compiler** | The program that translates C++ source code into a runnable program. | 3 |
| **Confidence interval** | A range that, under stated assumptions, would contain the true value in 95% of repeated experiments. | 11 |
| **Conservation** | Every lot of every order is accounted for: filled + cancelled + resting = accepted (adjusted at each modify). | 6 |
| **Coordinated omission** | In a closed-loop benchmark, slow requests delay later ones without that delay being measured, so tails look better than they are. | 12 |
| **Crossed book** | A book where the best bid is at or above the best ask. It cannot rest in lobsim; it would mean a bug. | 1, 5 |
| **Dangling pointer** | A pointer to memory that has been freed or reused. Following it is a serious bug. | 5 |
| **Depth** | How much is resting at each price level; `depth(side, n)` gives the top n levels. | 1 |
| **Determinism** | Same inputs, same outputs, every time. | 0, 7 |
| **Differential testing** | Feeding the same inputs to two implementations and requiring identical outputs. | 6 |
| **Discrete-event simulation** | A simulation whose clock jumps from one scheduled event to the next. | 8 |
| **Event** | One of the engine's answers: Ack, Reject, Trade, Cancelled, Modified. | 2 |
| **Event hash** | A 64-bit FNV-1a fingerprint of every event of a run, in a fixed byte encoding. | 12 |
| **Excess kurtosis** | A measure of fat tails: 0 for a bell curve, positive when extreme moves are more common. | 11 |
| **Exponential distribution** | The waiting time until the next event when events happen at random at a constant rate. | 7 |
| **FIFO** | First in, first out: earliest first, like a queue. | 2, 8 |
| **Fingerprint (market)** | lobsim's scorecard of market statistics, computed the same way for data and simulations. | 14 |
| **FOK** | Fill or kill: fill the whole order now or do nothing. | 2 |
| **Fundamental value** | The hidden "true worth" of the stock, a random walk only the informed trader can see. | 9 |
| **Fuzzing** | Testing with huge numbers of generated inputs; coverage-guided fuzzing keeps inputs that reach new code. | 6 |
| **GTC** | Good till cancelled: the unfilled rest of a limit order waits in the book. | 2 |
| **Header** | A `.hpp` file that declares what exists; other files `#include` it. | 3 |
| **Heap (data structure)** | An arrangement that keeps the smallest item at the front, with O(log n) insert and remove. The event queue is one. | 8 |
| **Informed trader** | The agent that trades when the fundamental value is far enough from the price. | 9 |
| **Invariant** | A fact that must always be true between operations, such as "levels are never empty". | 4, 5 |
| **Inventory** | How many lots a trader holds: positive long, negative short. | 1, 10 |
| **IOC** | Immediate or cancel: trade what you can now, cancel the rest. | 2 |
| **Kaplan–Meier** | The estimator of a survival curve that handles censored observations correctly. | 14 |
| **Latency** | The delay of a message; lobsim has entry, response and feed latencies per agent. | 8 |
| **Leakage** | When information from the test period sneaks into the fitting step, making an "out of sample" test fake. | 14 |
| **Level** | All resting orders at one price on one side. | 1, 5 |
| **Limit order** | An order with a worst acceptable price. | 1 |
| **Linked list** | A chain of items where each holds a pointer to the next (and, doubly linked, to the previous). | 5 |
| **Liquidity** | How easily you can trade a lot without moving the price much. | 1 |
| **Little's law** | Average number in a system = arrival rate × average time in the system. | 9 |
| **LOBSTER** | A data source of Nasdaq order books: a message file and an orderbook file per stock and day. | 13 |
| **Log return** | ln(m_t / m_{t−1}), the relative change of the price over one interval. | 11 |
| **Lot** | lobsim's unit of quantity. | 1 |
| **Maker** | The resting order in a trade. Trades print at the maker's price. | 1, 2 |
| **Market maker** | A trader who keeps a bid and an ask in the book to earn the spread. | 10 |
| **Market order** | An order that trades at any price; in lobsim always IOC. | 2 |
| **Markout** | How a fill looks h seconds later: mid(t + h) − price for a buy, price − mid(t + h) for a sell. | 11 |
| **Mark to market** | Valuing holdings at the current market price (here the mid). | 11 |
| **Mid** | (best bid + best ask) / 2. | 1 |
| **Modify** | Changing a resting order's price or remaining quantity; only "same price, smaller" keeps priority. | 2 |
| **Modulo bias** | The unfairness of `r % n` when the number of possible r is not a multiple of n. | 7 |
| **Mutation testing** | Changing code on purpose to check that the tests notice. | 6 |
| **Noise trader** | A zero-intelligence agent placing random orders around the price it sees. | 9 |
| **Normal distribution** | The bell curve; lobsim draws it with the Box–Muller method. | 7 |
| **Object pool** | Pre-allocated memory slots handed out and returned, so objects keep fixed addresses and allocation is cheap. | 5 |
| **Observer** | A read-only watcher of the simulation with zero latency; it must never change the run. | 8, 11 |
| **Order book** | The two sorted lists of resting buy and sell orders. | 1 |
| **orders.log** | The text file of every request that reached the exchange; replaying it reproduces the run. | 12 |
| **Out of sample** | Testing on data the model was not fitted to. | 14 |
| **Owner** | The trader an order belongs to; agent index + 1 in the simulator. | 1, 8 |
| **PMF** | Probability mass function: a histogram of a whole-number quantity, normalised. | 14 |
| **PnL** | Profit and loss. Here cash + inventory × mid − fees. | 11 |
| **Pointer** | A value holding the memory address of another value. | 5 |
| **Poisson process** | Events at random times with exponential gaps; the count per interval has a Poisson distribution. | 7 |
| **Post-only** | A limit order that is rejected if it would trade on arrival, guaranteeing maker status. | 2 |
| **Price-time priority** | Best price first; at the same price, earliest first. | 2 |
| **PRNG** | Pseudo-random number generator: a fixed recipe that produces random-looking numbers from a seed. | 7 |
| **Reference price** | The noise trader's view of the price, from which it offsets its limit orders. | 9 |
| **Reject** | The event "your request was refused; nothing changed", with a reason. | 2 |
| **Rejection sampling** | Throwing away some random draws so that the rest are exactly fair. | 7 |
| **Replay** | Feeding a recorded stream of requests (or messages) into a fresh engine. | 12, 13 |
| **Reservation price** | The market maker's personal price given its inventory: r = mid − q γ σ² (T − t). | 10 |
| **Resting** | Accepted, not fully filled, waiting in the book. | 1 |
| **Resync** | The replay mode that corrects the rebuilt book to the published one inside its window, counting every correction. | 13 |
| **Seed** | The number that starts a pseudo-random generator; same seed, same sequence. | 7 |
| **Self-trade prevention (STP)** | Stopping a trader from trading with itself; lobsim has CancelTaking and CancelResting. | 2 |
| **Spread** | best ask − best bid. A market maker's quoted spread is its own ask minus its own bid. | 1, 10 |
| **Spread capture** | What a fill earned against the mid just before it: mid_before − price for a buy. | 11 |
| **Static library** | A bundle of compiled code (`.a` file) copied into the programs that use it. | 3 |
| **std::optional** | A C++ type holding a value or nothing. | 3 |
| **std::variant** | A C++ type holding exactly one of several types. | 3 |
| **Stylized facts** | Statistical habits real returns tend to show: fat tails, no return autocorrelation, volatility clustering. | 11 |
| **Survival curve** | S(t), the chance that an order is still alive after time t. | 14 |
| **Synthetic** | Made by lobsim or by hand, not market data. Every such number is labelled. | 13, 14 |
| **Taker** | The arriving order that trades with a resting one. | 1, 2 |
| **Template** | C++ code written once for any type that fits, compiled separately for each type used. | 3, 5 |
| **Tick** | The smallest price step; lobsim stores prices as whole numbers of ticks. | 1 |
| **TIF** | Time in force: what happens to the unfilled rest of an order (GTC, IOC, FOK). | 2 |
| **Total variation (TV)** | Half the sum of absolute differences between two distributions: the share of probability in the wrong place. | 14 |
| **Trade** | The event for two orders trading: price (the maker's), quantity, both order ids and owners, aggressor side. | 2 |
| **Undefined behaviour** | Operations C++ gives no meaning to (such as signed overflow or reading freed memory). | 3, 6 |
| **Unit test** | A small, hand-checkable test of one rule. | 6 |
| **Validation (out of sample)** | Comparing a calibrated simulation's emergent statistics with a period of data never used for fitting. | 14 |
| **Volatility** | How much the price wiggles; here the standard deviation of log returns, or σ in ticks per √second. | 9, 11 |
| **Volatility clustering** | Big moves tending to follow big moves. | 11 |
| **Warm-up** | The first part of a run (60 s by default) left out of the statistics while the book fills up. | 10, 11 |
| **Wasserstein-1 (W1)** | A distance between distributions that also counts how far probability has to move, in the variable's units. | 14 |
| **Zero intelligence (ZI)** | Traders with random behaviour and no strategy, used as a baseline. | 9 |
