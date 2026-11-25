# Module 3: Your C++ toolkit

> **In one sentence:** C++ is a language where you describe exactly what the computer stores and does, and a compiler turns that description into a fast program.
>
> **You will be able to:** compile a program by hand, explain what `cmake --preset release` and `cmake --build` do, and read the C++ in lobsim's headers: types, structs, enums, `std::vector`, `std::optional`, `std::variant`, `std::visit`, lambdas, templates, `const`, references and exceptions.
>
> **Time:** about 4 hours. If you already program in C++, skim the lecture and do the labs.

This module teaches only the C++ that lobsim uses, and always with lobsim's own code as the example. Pointers and memory come later, in Module 5, where the fast book needs them.

## 3.1 From text to program

A **program** is a list of instructions. People write it as text, called **source code**. The computer's processor does not understand text; it understands **machine code**, long lists of tiny numeric instructions.

- **Python** reads your text and carries out each line as it goes, every time you run it. That is an **interpreter**.
- **C++** is translated once, ahead of time, by a **compiler** (lobsim uses GCC or Clang). The result is a ready-to-run program file. Running it later needs no translation, which is one reason C++ is fast.

Think of a recipe written in English (source code) and a robot cook that only understands a numeric code. Python is a translator standing next to the robot, translating each line while it cooks. C++ hands the robot a fully translated recipe card before it starts.

## 3.2 Lab 3.1: your first compile

Make a file `hello.cpp` anywhere outside the lobsim folder:

```cpp
#include <cstdio>

int main() {
  std::printf("hello, lobsim\n");
  return 0;
}
```

Compile and run:

```sh
c++ -std=c++20 hello.cpp -o hello
./hello
```

It prints `hello, lobsim`. What happened:

- `#include <cstdio>` copies in the declarations of the standard input/output functions, including `std::printf`.
- `int main()` is where every C++ program starts. It returns an `int` (a whole number) to the operating system: 0 means "everything went fine". lobsim's programs return other numbers to say what went wrong (for example `lobsim_lobster` returns 2 on a strict failure and 3 when a data file is missing).
- `std::printf` prints text. `\n` is a new line.
- `c++` runs your system's C++ compiler. `-std=c++20` asks for the 2020 version of the language (lobsim needs it). `-o hello` names the output program.

Now delete the `;` after the `printf(...)` line and compile again. Read the error message: it tells you the file, the line and what it expected. Compiler errors look scary but they are precise. Put the `;` back.

## 3.3 Headers and sources

lobsim splits most code into two kinds of file:

- **Headers** (`.hpp`, in `include/`) say **what exists**: the types, and the names and arguments of the functions. Other files `#include` them.
- **Sources** (`.cpp`, in `src/`) say **how it works**: the bodies of the functions.

For example `include/lob/order_book.hpp` says that an `OrderBook` has a function `void submit(const NewOrder& req, EventBuffer& out);`, and `src/lob/order_book.cpp` contains the 40 lines that do it. Code that only wants to *use* an order book includes the header and never needs to see the body.

Two lines you will see at the top of every header:

- `#pragma once` means "include this file at most once", even if several files ask for it.
- `namespace lob { ... }` puts everything inside into the family name `lob`. Outside, you write `lob::OrderBook`, `lob::Side::Buy`. The simulator uses `sim::`, the replayer `replay::`, calibration `calib::`, the dashboard exporter `viz::`, and the C++ standard library `std::`. Namespaces stop names from clashing, the way surnames tell apart two people called Sam.

## 3.4 Building a real project: CMake

lobsim has more than 100 source files. Typing a compiler command for each would be miserable, so a tool called **CMake** reads a description of the project, `CMakeLists.txt`, and generates the commands. Open `CMakeLists.txt` and find these lines:

```cmake
add_library(lob STATIC src/lob/order_book.cpp src/lob/reference_book.cpp)
target_include_directories(lob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob PRIVATE lobsim_options)
```

- `add_library(lob STATIC ...)` says: compile these two files into a **library** called `lob`. A **static library** is a bundle of compiled code (on disk, `build/release/liblob.a`) that is copied into any program that uses it.
- `target_include_directories(... include)` says where its headers are.
- `target_link_libraries(lob PRIVATE lobsim_options)` attaches the warning settings (below).

A program is made the same way:

```cmake
add_executable(lobsim_run apps/run_main.cpp)
target_link_libraries(lobsim_run PRIVATE sim lobsim_options)
```

`lobsim_run` is built from one file and **linked** with the `sim` library, which in turn says it needs `lob`, `orderlog`, `calibfile` and the JSON library. **Linking** is the step that joins all the compiled pieces into one program.

### Presets: three ways to build

`CMakePresets.json` defines three builds, each in its own folder:

| Preset | Folder | What it is for |
|---|---|---|
| `release` | `build/release` | Fast programs: compiled with `-O3` (optimise hard) and `-DNDEBUG` (switch off debugging checks) |
| `debug` | `build/debug` | Slower programs that check much more: no optimisation, and `NDEBUG` is not defined, so the engine runs its full self-check `validate()` after every message (Module 5) |
| `sanitize` | `build/sanitize` | Debug plus two "sanitizers" that catch memory mistakes and undefined behaviour while the program runs (Module 6) |

`cmake --preset release` **configures** (checks the compiler, downloads the three dependencies at fixed versions, writes the build commands). `cmake --build --preset release -j` **builds** (`-j` runs many compiler jobs at once). `ctest --preset release` **tests**.

### Warnings are errors

```cmake
add_library(lobsim_options INTERFACE)
target_compile_options(lobsim_options INTERFACE -Wall -Wextra -Wpedantic)
if(LOBSIM_WERROR)
  target_compile_options(lobsim_options INTERFACE -Werror)
endif()
```

`-Wall -Wextra -Wpedantic` turn on many warnings ("this is legal but probably a mistake"). `-Werror` turns every warning into an error, so code with a warning does not build at all. The project rule (SPEC.md section 8) is "zero warnings", and this makes the compiler enforce it.

## 3.5 Types: what a value is

Every value in C++ has a **type** that says how it is stored and what you may do with it.

| Type | Meaning | Used in lobsim for |
|---|---|---|
| `std::int64_t` | whole number, 64 bits, may be negative | `Price`, `Qty`, time in nanoseconds (`sim::TimeNs`) |
| `std::uint64_t` | whole number, 64 bits, never negative | `OrderId`, trade ids, random numbers, hashes |
| `std::uint32_t` | whole number, 32 bits, never negative | `OwnerId` |
| `std::uint8_t` | whole number, 0 to 255 | the storage of every enum |
| `double` | a decimal number, approximate | rates, probabilities, the fundamental value, statistics |
| `bool` | `true` or `false` | flags like `post_only` |

Two warnings that matter in lobsim:

- **Unsigned numbers wrap around.** For a `std::uint64_t`, 0 − 1 is 18,446,744,073,709,551,615, not −1. The random number generator relies on this on purpose (Module 7); everywhere else it would be a bug.
- **Signed overflow is undefined behaviour.** If a `std::int64_t` goes past its largest value, C++ makes no promise about what happens. lobsim's config checks keep every time and price far away from the limits (see `kFarFutureNs` in `include/sim/messages.hpp`).

`using Price = std::int64_t;` creates a second name for a type (an alias). `const` means "this value will not change after it is set". `constexpr` means "work this out while compiling", for constants like `inline constexpr lob::Price kReplayMaxPrice = 2'000'000'000;` (the `'` is just a digit separator).

## 3.6 Structs and classes

A **struct** bundles named fields into one value:

```cpp
struct Cancel {
  OwnerId owner{};
  OrderId order_id{};
  bool operator==(const Cancel&) const = default;
};
```

- The `{}` after each field means "start at the default value" (0 for numbers, `false` for `bool`).
- `bool operator==(const Cancel&) const = default;` asks the compiler to write the `==` comparison for you: two `Cancel`s are equal when every field is equal. The tests compare whole events this way: `EXPECT_EQ(h.last(), (Events{ack(4, 9), trade(...)}))`.
- You create one with `lob::Cancel c{7, 42};` (fields in order) and read a field with `c.order_id`.

A **class** is the same thing with **private** parts. `OrderBook` is a class: its `public:` member functions (`submit`, `cancel`, `best_bid`, ...) are the buttons anyone can press, and its `private:` data (the maps, the pool, the index) can only be touched by its own functions. That protects the book's rules: no outside code can put an order into the middle of a price level by hand.

Some member functions end in `const`, like `std::optional<Price> best_bid() const;`. That promises the function only reads the object and never changes it.

## 3.7 Enums: a fixed list of names

```cpp
enum class Side : std::uint8_t { Buy, Sell };
```

A `Side` is one of two names. Underneath, `Buy` is stored as 0 and `Sell` as 1 in one byte (`: std::uint8_t`). The word `class` makes it strict: you cannot compare a `Side` with a number by accident. You saw this error message while this course was being written:

```
error: no match for 'operator==' (operand types are 'lob::Side' and 'int')
    4 |   if (order.side == 1) return 1;
```

When you really want the number (to write it in `orders.log`, for instance), you say so with `static_cast<int>(side)`. lobsim writes enums as their numbers in `orders.log` and in the event hash.

## 3.8 Containers and other standard tools

### `std::vector`: a list that can grow

`lob::EventBuffer` is `std::vector<lob::Event>`. `out.push_back(e)` adds to the end, `out.size()` counts, `out[i]` reads the i-th item (from 0), `out.clear()` empties it but keeps the memory for reuse. The `ReferenceBook` keeps all its resting orders in one vector (Module 4).

### `std::optional`: a value, or nothing

```cpp
std::optional<Price> best_bid() const;
```

An empty book has no best bid. Instead of returning a made-up number like −1, the function returns "nothing" (`std::nullopt`). The caller must check: `if (auto b = book.best_bid()) use(*b);`. `*b` takes the value out; `b.value_or(0)` gives 0 when there is none. Using a missing value by mistake is a common bug, and the type makes it hard to forget.

### `std::variant`: exactly one of several types

```cpp
using Request = std::variant<NewOrder, Cancel, Modify>;
using Event = std::variant<Ack, Reject, Trade, Cancelled, Modified>;
```

A `Request` holds a `NewOrder` *or* a `Cancel` *or* a `Modify`, never two, and it knows which. Ways to look inside:

- `std::get_if<lob::Trade>(&ev)` gives a pointer to the `Trade` if that is what it holds, otherwise `nullptr`. You will see `if (const auto* t = std::get_if<lob::Trade>(&ev))` everywhere.
- `std::holds_alternative<lob::Ack>(ev)` just answers yes or no.
- `std::get<lob::NewOrder>(req)` takes it out, and throws an error if the variant holds something else.
- `std::visit(f, v)` calls `f` with whatever is inside.

### Lambdas: little functions written in place

```cpp
auto reject = [&](RejectReason reason) {
  out.push_back(Reject{req.owner, RequestKind::New, req.client_order_id, 0, reason});
};
```

This (from `OrderBook::do_submit`) makes a small function named `reject` inside another function. `[&]` lets it use the surrounding variables (`out`, `req`) directly. Later `return reject(RejectReason::InvalidQty);` uses it.

### The visitor pattern

Agents receive a `lob::Event` and need different code for each kind. The noise trader does it like this (`src/sim/noise_trader.cpp`, line 78):

```cpp
std::visit(
    [&](const auto& e) {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, lob::Ack>) {
        ...
      } else if constexpr (std::is_same_v<T, lob::Trade>) {
        ...
      }
    },
    event);
```

`const auto& e` means "whatever type is inside". `using T = std::decay_t<decltype(e)>;` names that type `T`. `if constexpr (std::is_same_v<T, lob::Ack>)` asks, while compiling, "is `T` the `Ack` type?". The compiler builds one version of the lambda per event type and keeps only the matching branch in each. You will see this shape in every agent; read it once here and you can read them all.

### Templates: one piece of code for many types

```cpp
template <class Book>
void apply(Book& book, const Request& req, EventBuffer& out) { ... }
```

`template <class Book>` means "`Book` can be any type that has the functions used inside". `apply(ref_book, ...)` and `apply(fast_book, ...)` are compiled into two versions, one per book. The fast book also uses templates to write its matching code once for both sides of the book (Module 5).

## 3.9 Passing things to functions

```cpp
void submit(const NewOrder& req, EventBuffer& out);
```

- `const NewOrder& req` passes a **reference** (the function looks at the caller's order directly, no copy is made) that is `const` (the function promises not to change it).
- `EventBuffer& out` is a reference without `const`: the function is *expected* to change it, by appending events. This is how lobsim returns events without building a new list per call (SPEC.md 3.7: "no allocation per call once the buffer has grown").

Values that are small, like a `Side` or a `Price`, are passed by plain copy.

## 3.10 When things go wrong: exceptions

```cpp
[[noreturn]] void fail(const std::string& what) { throw std::logic_error("OrderBook: " + what); }
```

`throw` stops the current work and travels up through the callers until some code `catch`es it. If nobody does, the program ends with a message. lobsim throws in situations that must never happen in a correct program, such as a book whose bid is above its ask (`std::logic_error`), and for bad input that a user can fix (`std::runtime_error`, for example a config with an unknown key). `noexcept` on a function promises it never throws.

Some checks run only in debug builds:

```cpp
void OrderBook::submit(const NewOrder& req, EventBuffer& out) {
  do_submit(req, out);
#ifndef NDEBUG
  validate();
#endif
}
```

The lines between `#ifndef NDEBUG` and `#endif` are only compiled when `NDEBUG` is *not* defined, which is the debug and sanitize builds. Release builds skip them for speed. This is the **preprocessor**, a step before compiling that handles `#include`, `#define`, `#ifdef` and `#if`. lobsim also uses it to switch on deliberately injected bugs in special test builds (Module 6).

## Lab 3.2: the tour

```sh
course/labs/build_lab.sh course/labs/lab03_cpp_tour.cpp
build/release/labs/lab03_cpp_tour
```

Open `course/labs/lab03_cpp_tour.cpp` and guess what each of the 8 parts prints before running. As printed when this course was written:

```
1. 0.1 + 0.2 == 0.3 ? no (it is 0.30000000000000004)
   10 + 20 ticks == 30 ticks ? yes
2. order: owner 7, side Sell, type Limit, tif GTC, price 10001, qty 5, post_only 0
3. Buy is stored as 0, Sell as 1; opposite(Sell) is Buy
4. copy == order ? yes; after changing qty: no
5. the request holds alternative number 1 (0 NewOrder, 1 Cancel, 2 Modify)
   it is a Cancel of order 42
6. a new order for 5 lots
6. a cancel of order 42
6. a modify to 3 lots at 10000
7. best bid of an empty book: none (nullopt)
   value_or(-1) gives -1
8. Ack{order_id=1, owner=7, client_order_id=0}
   best ask is now 10001
```

`build_lab.sh` is a small script that runs the compiler with lobsim's include folders and libraries; open it and read the compiler command it builds.

## Lab 3.3: break it on purpose

Make each change in a copy of `lab03_cpp_tour.cpp`, rebuild, read the message, then undo it.

1. In part 3, write `if (order.side == 1) std::printf("sell\n");`. (Compile error: an enum class is not a number.)
2. In part 2, write `order.price = "10001";`. (Compile error: text is not a `Price`.)
3. In part 5, after `req` holds a `Cancel`, write `std::printf("%lld\n", (long long)std::get<lob::NewOrder>(req).qty);`. This compiles. What happens when you run it?
4. In part 7, write `std::printf("%lld\n", (long long)*book.best_bid());` on the empty book. This also compiles. It is a bug: reading an empty `optional`. Unlike number 3, C++ does not promise any error message here. Module 6 shows how a sanitizer build catches mistakes like this.

<details><summary>What number 3 does</summary>

The program stops with `terminate called after throwing an instance of 'std::bad_variant_access'` and `what(): std::get: wrong index for variant` (the exact wording depends on the compiler). `std::get` checks which type is inside and throws when you ask for the wrong one.

</details>

## Lab 3.4: read the build

In `CMakeLists.txt` find:

1. Which source files make up the `sim` library?
2. The line that adds `-ffp-contract=off` to `sim`, and its comment. (Module 7 explains it.)
3. Where `lob_tests` is defined: which files are in it, and which library does it link for testing?
4. The comment "The simulator. It depends on lob; lob does not depend on it." Why can't `lob` link `sim`? (Look at what `sim` links.)

<details><summary>Answers</summary>

1. `src/sim/agents/calibrated_flow.cpp`, `as_market_maker.cpp`, `config.cpp`, `exchange.cpp`, `informed_trader.cpp`, `metrics.cpp`, `noise_trader.cpp`, `simulator.cpp` (all under `src/sim/`).
2. `target_compile_options(sim PUBLIC -ffp-contract=off)`, with the comment that floating-point results must not depend on the build type.
3. Eleven files from `tests/unit/` (`smoke_test.cpp`, `matching_test.cpp`, ...), linked with `lob` and `GTest::gtest_main`.
4. `sim` already links `lob`. If `lob` also linked `sim`, each would need the other first: a circle. Keeping `lob` at the bottom of the tree is also a design choice, so the engine can be tested and reused alone.

</details>

## Common questions

**Why are there so many different integer types?** Each says something: `Qty` may be negative inside calculations, an `OrderId` never is, an owner id fits in 32 bits, an enum in 8. The right type documents the meaning and catches mistakes.

**What does `std::` mean?** The namespace of the C++ standard library, the toolbox that comes with every compiler: `std::vector`, `std::optional`, `std::map`, `std::printf`.

**Why pass by `const&` instead of by value?** A `NewOrder` is about 48 bytes. Copying it for every call would cost time in the hot path. A reference costs nothing and `const` keeps it safe.

**What is "undefined behaviour"?** Things the C++ rules do not define, such as reading past the end of a vector or overflowing a signed integer. The program might crash, give wrong answers or appear to work. The sanitize build (Module 6) catches many of them while the tests run.

**Do I need to memorise all this?** No. Come back to this module when a piece of syntax stops you. Reading real code with this page open is how everyone learns it.

## Quiz

1. What is the difference between a header and a source file?
2. What does `cmake --build --preset release -j` produce, and where?
3. Why does `if (side == 0)` not compile when `side` is a `lob::Side`?
4. A function returns `std::optional<lob::Price>`. What must the caller do before using the price?
5. What does `std::get_if<lob::Trade>(&ev)` return when `ev` holds an `Ack`?
6. What does `-Werror` do?
7. Why does the engine take `EventBuffer& out` instead of returning a new vector?

<details><summary>Answers</summary>

1. A header declares what exists (types, function names and arguments); a source file defines how it works (function bodies).
2. Compiled libraries (`*.a`) and programs (`lobsim_run`, `lob_tests`, ...) in `build/release/`.
3. `enum class` values are not numbers; comparing with an `int` needs an explicit `static_cast`.
4. Check that it holds a value (`if (p)` or `p.has_value()`), then read it with `*p`, or use `p.value_or(default)`.
5. `nullptr`.
6. Makes every compiler warning an error, so the build fails until the warning is fixed.
7. The caller can reuse one buffer for every request (`clear()` keeps its memory), so after it has grown, handling a request allocates no new memory.

</details>

## What's next

Module 4 reads a whole matching engine: the `ReferenceBook`, written to be as simple as possible.
