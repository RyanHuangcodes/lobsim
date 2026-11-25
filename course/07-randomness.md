# Module 7: Randomness you can repeat

> **In one sentence:** lobsim's traders act at random, but the random numbers come from a fixed recipe started by one seed, so every run can be repeated exactly, on any machine that follows the recipe.
>
> **You will be able to:** explain what a pseudo-random generator and a seed are, read lobsim's generators and distributions (uniform, exponential, normal), explain modulo bias and rejection sampling, describe a Poisson process, explain why each agent has its own random streams, and list the ways a "deterministic" program can still differ between machines.
>
> **Time:** about 3 hours.

## 7.1 Why a market simulator needs dice

Nobody knows exactly when the next order will arrive, whether it will be a buy or a sell, or how big it will be. A simulator models that uncertainty with random numbers: "this trader wakes up on average 5 times a second, picks buy or sell like a coin flip, and picks a size from 1 to 10 like a die".

But a simulator that gives a different answer every time cannot be tested, debugged or compared. If a change to the market maker makes it earn more, you want to know that the change did it, not luck. So lobsim needs randomness that **looks** random and **repeats** on demand.

## 7.2 Pseudo-random numbers

A **pseudo-random number generator** (PRNG) is a recipe: it keeps a few numbers of "state", and each time you ask for a number it scrambles the state in a fixed way and gives you some of the result. The output passes statistical tests for randomness, but it is completely determined by the starting state. The **seed** is the number that sets that starting state.

Same seed, same sequence. Seed 42 will tell the same story every time, on every computer that follows the recipe exactly. That is the whole trick behind "same config and seed give byte-identical output" (README).

## 7.3 lobsim's two generators

`include/sim/rng.hpp` writes out two well-known generators instead of using C++'s built-in ones.

**SplitMix64** (Steele, Lea and Flood, 2014) turns one seed into well-mixed numbers. lobsim uses it only to start other generators:

```cpp
constexpr std::uint64_t next() noexcept {
  std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}
```

**xoshiro256\*\*** (Blackman and Vigna, 2018) is the main generator. Its state is four 64-bit numbers, filled from SplitMix64:

```cpp
std::uint64_t next_u64() noexcept {
  const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
  const std::uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = rotl(s_[3], 45);
  return result;
}
```

You do not need to understand why these exact constants work (that is research). You need to know the operations, because they show up everywhere in low-level code:

| Operation | Meaning | Example on 8 bits |
|---|---|---|
| `a ^ b` (XOR) | each bit is 1 where `a` and `b` differ | `1100 ^ 1010 = 0110` |
| `a >> k` | slide the bits right by `k`, zeros come in on the left | `1100 >> 2 = 0011` |
| `a << k` | slide left, zeros come in on the right, bits fall off the left | `0011 << 2 = 1100` |
| `rotl(a, k)` | slide left, and the bits that fall off come back on the right | `1001 0000` rotated by 1 = `0010 0001` |
| `a * b` on `uint64_t` | multiply, keep only the lowest 64 bits (wraps around) | |

Unsigned arithmetic in C++ wraps around exactly (it is arithmetic modulo 2^64), so every machine computes the same bits. That is why the generator uses `std::uint64_t` everywhere.

### Why not `std::uniform_int_distribution`?

C++ provides generators *and* distributions (`std::normal_distribution`, ...). The generators are fully specified, but the **distributions are not**: each standard library (GCC's, Clang's, Microsoft's) may turn the same random bits into different numbers. A lobsim run built with Clang on a Mac would then differ from the same run built with GCC on Linux. SPEC.md 5.2 therefore requires lobsim's own distributions.

## 7.4 From random bits to useful numbers

### A number between 0 and 1

```cpp
double uniform01() noexcept { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }
```

A `double` holds 53 bits of precision. Keep the top 53 of the 64 random bits (`>> 11`) and multiply by 2^-53 (written `0x1.0p-53`): the result is one of 2^53 evenly spaced values in [0, 1). Used for "with probability p": `rng.uniform01() < p_limit`.

### A fair whole number: rejection sampling

```cpp
std::int64_t uniform_int(std::int64_t lo, std::int64_t hi) noexcept {
  const std::uint64_t span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
  if (span == ~std::uint64_t{0}) return static_cast<std::int64_t>(next_u64());
  const std::uint64_t n = span + 1;
  const std::uint64_t threshold = (0 - n) % n;  // 2^64 mod n
  for (;;) {
    const std::uint64_t r = next_u64();
    if (r >= threshold) return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + r % n);
  }
}
```

The obvious way, `r % n`, is slightly unfair. With 3 random bits (0 to 7) and a 3-sided die, `r % 3` gives face 0 for r = 0, 3, 6, face 1 for 1, 4, 7, and face 2 only for 2, 5. Faces 0 and 1 win more often. This is **modulo bias**. The fix: throw away the few smallest values (`r < 2^64 mod n`) so the rest split into exactly equal groups, and draw again. With 64-bit draws, a rejection almost never happens, but the die is now exactly fair. `(0 - n) % n` is a trick for "2^64 mod n" that uses the wrap-around of unsigned numbers.

### How long until the next event: exponential

```cpp
double exponential(double lambda) noexcept { return -std::log(1.0 - uniform01()) / lambda; }
```

If events happen at random at an average **rate** λ per second (λ is the Greek letter lambda), the waiting time until the next one follows the **exponential distribution**, with mean 1/λ. This formula turns a uniform number into such a wait: the **inverse transform method**. (The cumulative chance of waiting less than t is 1 − e^(−λt). Set it equal to u and solve for t.) `1.0 - uniform01()` is in (0, 1], so the logarithm is always finite.

The exponential has a special property: it **has no memory**. A trader who has already waited 2 seconds is no closer to waking up than one who just went to sleep. That makes "each resting order is cancelled at a constant rate" (SPEC.md 5.6) easy to model: draw one exponential lifetime per order.

### Bell curves: normal by Box–Muller

```cpp
double normal() noexcept {
  if (has_spare_) {
    has_spare_ = false;
    return spare_;
  }
  const double u1 = 1.0 - uniform01();  // in (0, 1], so the logarithm is finite
  const double u2 = uniform01();
  const double radius = std::sqrt(-2.0 * std::log(u1));
  const double angle = 6.283185307179586476925286766559 * u2;
  spare_ = radius * std::sin(angle);
  has_spare_ = true;
  return radius * std::cos(angle);
}
```

The **normal** (Gaussian, bell-curve) distribution describes sums of many small random pushes. The Box–Muller method picks a random angle and a random radius and returns the two coordinates of that point; each is an independent standard normal number (mean 0, standard deviation 1). The second one is kept as a "spare" for the next call. lobsim uses it for one thing: the steps of the hidden fundamental value (Module 9).

## 7.5 A Poisson process

A noise trader with λ = 5 does not wake every 0.2 s like a clock. It draws an exponential wait, sleeps, acts, draws the next wait, and so on. The resulting stream of wakeups is a **Poisson process**: on average 5 per second, but some seconds have 2 and some have 9. The number of events in one second follows the **Poisson distribution**: P(k events) = e^(−λ) λ^k / k!. Real order arrivals are often modelled this way as a first approximation (SPEC.md cites Smith, Farmer and others for zero-intelligence order flow as Poisson streams).

## 7.6 One stream per purpose

Every consumer of randomness has its **own** generator, seeded from the one master seed and a **stream id**:

```cpp
enum class StreamRole : std::uint32_t { System = 0, NoiseTrader = 1, InformedTrader = 2, MarketMaker = 3, CalibratedFlow = 4, Test = 15 };
enum class StreamPurpose : std::uint32_t {
  Decisions = 0,  // the agent's own-clock decisions: ctx.rng(), used in on_start/on_wakeup only
  Latency = 1,    // jitter on that agent's three channels
  Lifetimes = 2,  // order lifetimes drawn when a reply arrives (noise traders)
};

constexpr std::uint64_t stream_id(StreamRole role, std::uint32_t index, StreamPurpose purpose) noexcept {
  return (static_cast<std::uint64_t>(role) << 32) | (static_cast<std::uint64_t>(index) << 8) |
         static_cast<std::uint64_t>(purpose);
}

constexpr std::uint64_t stream_seed(std::uint64_t master_seed, StreamRole role, std::uint32_t index,
                                    StreamPurpose purpose) noexcept {
  return SplitMix64::mix(master_seed ^ (0x9E3779B97F4A7C15ULL * (stream_id(role, index, purpose) + 1)));
}
```

The stream id packs (role, index within the role, purpose) into one number; for example noise trader 3's latency stream is role 1, index 3, purpose 1. The id **does not depend on how many agents exist**. So adding the market maker cannot change noise trader 3's dice: noise trader 3 is still role 1, index 3. DECISIONS.md D-071.

The hidden fundamental value uses `(System, 0, Decisions)`, so it is the same random walk whatever the traders do.

### The subtle rule: draw decisions only on your own clock

`Context::rng()` (the agent's decision stream) may be used only in `on_start` and `on_wakeup`. A draw triggered by an **incoming message** must use a different stream. The noise trader draws each order's lifetime when the order's `Ack` arrives, from its separate `Lifetimes` stream (D-072).

Why? Each agent has one latency generator shared by its three channels, and its feed channel rolls those dice once for every piece of market data it receives (Module 8). How much market data arrives depends on every other agent. So the exact moment an agent's `Ack` arrives, before or after its next wakeup, depends on the whole market. If the lifetime came from the decision stream, the decision stream would sometimes be read "lifetime, decision" and sometimes "decision, lifetime", and adding any agent anywhere would shuffle this trader's later decisions. With separate streams, its decisions depend only on its own clock.

`tests/determinism.cpp` checks exactly this, including with a huge 300 ms jitter where `Ack`s often land on the far side of the next wakeup (`AddingANoiseTraderDoesNotChangeTheOthersDecisionsUnderHugeJitter`). With the default 50 µs jitter the bug would almost never show, which is why the test makes the jitter absurd on purpose.

## 7.7 What can still differ between machines

The README is careful: "Results are bit-identical for the same build and platform." Across platforms, three things can break that, and lobsim deals with each:

1. **Different distributions in different standard libraries.** Solved by writing lobsim's own (7.3).
2. **Fused multiply-add.** A compiler may turn `a * b + c` into a single instruction that rounds once instead of twice, so the last bit of a result can differ between an optimised and an unoptimised build. lobsim compiles the simulator with `-ffp-contract=off`, which forbids that (D-078).
3. **The maths library.** `std::log`, `std::sin` and `std::cos` are not required by the floating-point standard to give the correctly rounded answer, so two systems may differ in the last bit. One different bit in one exponential wait can move an event by a nanosecond and change the order of later events. (`std::sqrt` *is* required to be correctly rounded.) lobsim does not fix this; it documents it, and no test pins a fixed simulation hash for that reason. The tests compare two runs on the same machine instead.

(When this course was written, the default run on Linux with GCC 11 printed the same event hash, `5aa4b082b3f2168c`, as the README's run on an Apple M4. That is typical, not promised.)

And one trap in C++ itself, the subject of the puzzle in Lab 7.1: in `f(g(), h())`, C++ does **not** say whether `g()` or `h()` runs first. If both draw random numbers, different compilers can give different answers. lobsim's rule (D-047): never draw two random values inside one function call's argument list.

## Lab 7.1: the same numbers in two languages

```sh
course/labs/build_lab.sh course/labs/lab07_rng.cpp
build/release/labs/lab07_rng
python3 course/labs/py/xoshiro.py
```

`course/labs/py/xoshiro.py` is a line-by-line Python copy of `rng.hpp`. Both print, when this course was written:

```
next_u64      1546998764402558742 6990951692964543102 12544586762248559009
uniform01     0.924692945325 0.991803914282 0.769739460434
uniform_int   5 8 9 6 10
exponential   0.068689071779 0.322914511969 0.077548113978
normal        1.133588510677 -1.094838901638 0.823831637279
noise trader 0's decision seed: 1263617121928133878
```

(Python prints them as lists; the numbers are the same.) Two languages, two compilers, one recipe, one seed: one sequence.

Now look at the last line of the C++ program, `puzzle`. It calls `next_u64()` three times inside one `printf`. Compare it with the first line. Then rebuild with the other compiler and run again (`CXX=clang++ course/labs/build_lab.sh course/labs/lab07_rng.cpp`, or `CXX=g++` on a Mac with GCC installed).

<details><summary>What happened when this course was written</summary>

With GCC 11 the puzzle line printed the three numbers in **reverse** order: `12544586762248559009 6990951692964543102 1546998764402558742`. GCC evaluated the arguments right to left. With Clang 15 they came out in the original order. Both compilers are correct: the language leaves the order open. That is the reason for D-047.

</details>

## Lab 7.2: modulo bias

```sh
python3 course/labs/py/modulo_bias.py
```

It counts every possible outcome exactly. Expected first block:

```
3 random bits (8 values), a die with 3 faces
   r % n            : {0: 3, 1: 3, 2: 2}
   with rejection   : {0: 2, 1: 2, 2: 2}   (2 values thrown away)
```

Then explain in your own words why `tests/unit/sim_rng_test.cpp` tests unbiasedness on a range of size 3 × 2^62 rather than a range of size 3 (DECISIONS.md D-082 has the answer).

<details><summary>Answer</summary>

On a range of 3 values the bias of `r % 3` with 64-bit draws is about 2^-64: far too small to measure in any test. With n = 3 × 2^62, 2^64 mod n = 2^62, so without rejection the lowest third of the range would be hit for r < 2^62 *and* for r in the top part: probability 1/2 instead of 1/3. A test can see that easily, so it would catch a missing rejection step.

</details>

## Lab 7.3: a Poisson process

```sh
python3 course/labs/py/poisson.py
```

It uses the Python copy of lobsim's generator to simulate 2,000 seconds of one noise trader's wakeups. Expected beginning:

```
10088 wakeups in 2000 s: 5.044 per second (lambda = 5.0)
mean wait 0.1982 s (1/lambda = 0.2000), shortest 0.010 ms, longest 2.169 s
```

followed by a table comparing "wakeups in one second" with the Poisson formula. Questions:

1. The longest wait was over 2 seconds although the mean is 0.2 s. Is that a bug?
2. About what share of seconds had no wakeup at all? What does the formula say?
3. Change `LAMBDA` to 50 (the whole noise crowd of 10 traders together). What happens to the table's shape?

<details><summary>Answers</summary>

1. No. Exponential waits are often short and occasionally long. A wait of more than 2 s has probability e^(−5 × 2) ≈ 0.000045 per wait; with about 10,000 waits you expect about 0.45 of them, so seeing one is not surprising.
2. About 0.5% in the run (0.005), against e^(−5) ≈ 0.0067 from the formula.
3. The counts center on 50 and the shape looks more and more like a bell curve. (You will need to print more rows to see it.)

</details>

## Lab 7.4: prove that adding a trader changes nobody's dice

```sh
build/release/lobsim_run --config configs/default.json --out out/run1
build/release/lobsim_run --config course/labs/configs/no_informed.json --out out/no_informed
python3 course/labs/py/same_decisions.py out/run1 out/no_informed
```

`no_informed.json` is the default config with the `informed` section removed. The script compares each noise trader's new orders in the two runs. When this course was written:

```
noise trader  1: 17892 orders compared, same decision 17892, same price 2900
noise trader  2: 18060 orders compared, same decision 18060, same price 2938
...
```

Every decision (limit or market, buy or sell, size, and even the number of orders) is identical, yet most prices differ. Explain why both facts are what lobsim's design promises.

<details><summary>Answer</summary>

The decisions come from each noise trader's own decision stream, seeded by (seed, NoiseTrader, index, Decisions), and are drawn only on its own wakeup clock, which comes from the same stream. Removing the informed trader changes neither. The limit price, though, is "reference price ± d", and the reference is the trader's view of the market's mid, which does change when the informed trader is gone. The "same price" count is mostly the market orders, whose price field is always 0 (about 15% of orders).

</details>

## Lab 7.5: read the determinism tests

Open `tests/determinism.cpp` and find, for each claim, the test that checks it:

1. The same seed twice gives identical output files.
2. A different seed changes every output file.
3. Attaching the metrics observer changes nothing.
4. The summary contains no wall-clock time, path or host name.
5. Adding the market maker leaves the noise traders' decisions unchanged, even with huge jitter.

Then run them: `build/release/sim_tests --gtest_filter='Determinism.*'`.

## Common questions

**Is pseudo-random "random enough"?** For simulation, yes: xoshiro256** passes demanding statistical test suites. It is **not** suitable for cryptography (passwords, keys), where unpredictability matters; that is a different job.

**Why derive the stream seeds through SplitMix64 instead of using seed, seed + 1, seed + 2?** Nearby seeds would give generators whose starting states are nearly the same. Mixing the stream id through SplitMix64 makes every stream's start look unrelated to every other's.

**Why is the market maker given no random stream at all?** Its rules are deterministic: it computes quotes from what it sees. It is still given streams for latency (every agent has those), but `ctx.rng()` is never drawn (see the comment in `as_market_maker.hpp`).

**Why does the informed trader draw its next wakeup *before* deciding?** So that its wakeup times never depend on prices (D-085). If it drew only when it traded, the market would change its clock.

## Quiz

1. What is a seed?
2. Why does lobsim not use `std::normal_distribution`?
3. Why is `r % n` slightly unfair, and how does `uniform_int` fix it?
4. Waits are exponential with λ = 4 per second. What is the mean wait?
5. Which stream does a noise trader use for its order lifetimes, and why not its decision stream?
6. Name two reasons a lobsim run might differ between two different machines, and what lobsim does about each.

<details><summary>Answers</summary>

1. The number that sets a pseudo-random generator's starting state; the same seed always gives the same sequence.
2. The standard distributions are implementation-defined: different standard libraries give different numbers from the same bits.
3. When 2^64 is not a multiple of n, some remainders occur one more time than others. `uniform_int` rejects draws below 2^64 mod n and draws again, so every remainder is equally likely.
4. 1/4 s = 0.25 s.
5. Its own `Lifetimes` stream. A lifetime is drawn when an `Ack` arrives, whose timing depends on the whole market; using the decision stream would let other agents shift this trader's decisions.
6. Fused multiply-add (forbidden with `-ffp-contract=off`); different maths libraries for `log`, `sin`, `cos` (documented, not fixed; tests compare runs instead of pinning a hash); also standard-library distributions (avoided by writing its own) and argument evaluation order (never draw twice in one call).

</details>

## What's next

Module 8 builds the clock: an event queue that decides what happens next, latency channels that deliver messages late, and the kernel loop that ties the exchange and the agents together.
