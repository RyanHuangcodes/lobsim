// Microbenchmarks of single OrderBook operations (SPEC 7), with the book pre-filled to
// 1,000 and to 100,000 resting orders.
//
// Each benchmark times a batch of kBatch operations and then undoes the batch outside the
// timed region, so the book stays near its nominal size. Timing is paused once per batch,
// never once per operation: PauseTiming/ResumeTiming cost more than the operations do.
// The reported time is per operation.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace {

using namespace lob;

constexpr Price kMaxPrice = 1'000'000;
constexpr Price kMid = 500'000;
constexpr Qty kQty = 10;
constexpr std::size_t kBatch = 128;

class Rng {  // SplitMix64
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

 private:
  std::uint64_t state_;
};

NewOrder gtc(OwnerId owner, Side side, Price price, Qty qty) {
  return NewOrder{owner, 0, side, OrderType::Limit, TIF::GTC, price, qty, false, STP::CancelTaking};
}
NewOrder ioc(OwnerId owner, Side side, Price price, Qty qty) {
  return NewOrder{owner, 0, side, OrderType::Limit, TIF::IOC, price, qty, false, STP::CancelTaking};
}

// A book of `n` resting orders of kQty lots from owner 1, half bids and half asks, spread
// evenly over `levels` price levels per side around kMid (50 levels per side for 1,000
// orders, 500 for 100,000), so the best bid is kMid - 1 and the best ask kMid + 1.
struct Book {
  struct Resting {
    OrderId id;
    Side side;
    Price price;
  };

  explicit Book(std::int64_t n) : levels(n <= 1000 ? 50 : 500) {
    ev.reserve(1024);
    resting.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
      const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
      const Price level = (i / 2) % levels;
      const Price price = side == Side::Buy ? kMid - 1 - level : kMid + 1 + level;
      resting.push_back(Resting{submit(gtc(1, side, price, kQty)), side, price});
    }
    order.resize(resting.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    shuffle();
  }

  // Every order submitted here is valid, so ids are dense: the n-th submit gets id n.
  OrderId submit(const NewOrder& o) {
    ev.clear();
    book.submit(o, ev);
    return next_id++;
  }
  void cancel(OwnerId owner, OrderId id) {
    ev.clear();
    book.cancel(Cancel{owner, id}, ev);
  }
  void modify(OrderId id, Price price, Qty qty) {
    ev.clear();
    book.modify(Modify{1, id, price, qty}, ev);
  }

  void shuffle() {
    for (std::size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[rng.below(i)]);
    cursor = 0;
  }
  // The next kBatch distinct resting orders in random order (indices into `resting`).
  const std::size_t* pick() {
    if (cursor + kBatch > order.size()) shuffle();
    const std::size_t* p = order.data() + cursor;
    cursor += kBatch;
    return p;
  }

  OrderBook book{BookConfig{kMaxPrice}};
  EventBuffer ev;
  Price levels;
  std::vector<Resting> resting;
  std::vector<std::size_t> order;  // a random permutation of indices into `resting`
  std::size_t cursor{0};
  OrderId next_id{1};
  Rng rng{20261116};
};

// Add a non-crossing limit order to a price level that already exists.
void BM_AddExistingLevel(benchmark::State& state) {
  Book b(state.range(0));
  std::vector<NewOrder> orders;
  for (std::size_t i = 0; i < kBatch; ++i) {
    const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
    const auto level = static_cast<Price>(b.rng.below(static_cast<std::size_t>(b.levels)));
    orders.push_back(gtc(2, side, side == Side::Buy ? kMid - 1 - level : kMid + 1 + level, 1));
  }
  while (state.KeepRunningBatch(static_cast<benchmark::IterationCount>(kBatch))) {
    const OrderId first = b.next_id;
    for (const NewOrder& o : orders) {
      b.ev.clear();
      b.book.submit(o, b.ev);
      benchmark::DoNotOptimize(b.ev.data());
    }
    state.PauseTiming();
    b.next_id += kBatch;
    for (std::size_t i = 0; i < kBatch; ++i) b.cancel(2, first + i);
    state.ResumeTiming();
  }
}

// Add a non-crossing limit order at a price with no level yet (a std::map node is created).
void BM_AddNewLevel(benchmark::State& state) {
  Book b(state.range(0));
  std::vector<NewOrder> orders;
  for (std::size_t i = 0; i < kBatch; ++i) {
    const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
    const auto beyond = static_cast<Price>(b.levels + 1 + static_cast<Price>(i / 2));
    orders.push_back(gtc(2, side, side == Side::Buy ? kMid - 1 - beyond : kMid + 1 + beyond, 1));
  }
  while (state.KeepRunningBatch(static_cast<benchmark::IterationCount>(kBatch))) {
    const OrderId first = b.next_id;
    for (const NewOrder& o : orders) {
      b.ev.clear();
      b.book.submit(o, b.ev);
      benchmark::DoNotOptimize(b.ev.data());
    }
    state.PauseTiming();
    b.next_id += kBatch;
    for (std::size_t i = 0; i < kBatch; ++i) b.cancel(2, first + i);
    state.ResumeTiming();
  }
}

// Cancel a resting order chosen at random from the whole book (its level stays).
void BM_Cancel(benchmark::State& state) {
  Book b(state.range(0));
  const std::size_t* picked = b.pick();
  while (state.KeepRunningBatch(static_cast<benchmark::IterationCount>(kBatch))) {
    for (std::size_t i = 0; i < kBatch; ++i) {
      b.ev.clear();
      b.book.cancel(Cancel{1, b.resting[picked[i]].id}, b.ev);
      benchmark::DoNotOptimize(b.ev.data());
    }
    state.PauseTiming();
    for (std::size_t i = 0; i < kBatch; ++i) {
      Book::Resting& r = b.resting[picked[i]];
      r.id = b.submit(gtc(1, r.side, r.price, kQty));
    }
    picked = b.pick();  // choosing (and reshuffling) is not part of the measurement
    state.ResumeTiming();
  }
}

// An aggressive order that trades within one level and fully fills exactly one maker.
void BM_MatchOneLevel(benchmark::State& state) {
  Book b(state.range(0));
  const NewOrder maker = gtc(3, Side::Sell, kMid + 1, kQty);
  const NewOrder taker = ioc(4, Side::Buy, kMid + 1, kQty);
  while (state.KeepRunningBatch(static_cast<benchmark::IterationCount>(kBatch))) {
    state.PauseTiming();
    for (std::size_t i = 0; i < kBatch; ++i) b.submit(maker);  // refill the best ask level
    state.ResumeTiming();
    for (std::size_t i = 0; i < kBatch; ++i) {
      b.ev.clear();
      b.book.submit(taker, b.ev);
      benchmark::DoNotOptimize(b.ev.data());
    }
    b.next_id += kBatch;
  }
}

// Modify a random resting order to a smaller quantity at the same price (keeps priority).
void BM_ModifyReduce(benchmark::State& state) {
  Book b(state.range(0));
  const std::size_t* picked = b.pick();
  while (state.KeepRunningBatch(static_cast<benchmark::IterationCount>(kBatch))) {
    for (std::size_t i = 0; i < kBatch; ++i) {
      const Book::Resting& r = b.resting[picked[i]];
      b.ev.clear();
      b.book.modify(Modify{1, r.id, r.price, kQty - 1}, b.ev);
      benchmark::DoNotOptimize(b.ev.data());
    }
    state.PauseTiming();
    for (std::size_t i = 0; i < kBatch; ++i) {
      const Book::Resting& r = b.resting[picked[i]];
      b.modify(r.id, r.price, kQty);  // back to full size (re-enters at the tail, same id)
    }
    picked = b.pick();
    state.ResumeTiming();
  }
}

// Read the best bid and the best ask.
void BM_BestPrices(benchmark::State& state) {
  Book b(state.range(0));
  for (auto _ : state) {
    auto bid = b.book.best_bid();
    auto ask = b.book.best_ask();
    benchmark::DoNotOptimize(bid);
    benchmark::DoNotOptimize(ask);
  }
}

// Calibration: what the batching pattern above costs by itself. Both run the same empty
// operation in batches of kBatch; the first also pauses and resumes the timer once per
// batch, as five of the benchmarks above do. The difference between the two is the timer
// overhead included in each per-operation time of those five benchmarks.
void BM_CalibrationEmptyBatchWithPause(benchmark::State& state) {
  std::uint64_t x = 0;
  while (state.KeepRunningBatch(static_cast<benchmark::IterationCount>(kBatch))) {
    for (std::size_t i = 0; i < kBatch; ++i) benchmark::DoNotOptimize(x += i);
    state.PauseTiming();
    state.ResumeTiming();
  }
}

void BM_CalibrationEmptyBatchWithoutPause(benchmark::State& state) {
  std::uint64_t x = 0;
  while (state.KeepRunningBatch(static_cast<benchmark::IterationCount>(kBatch))) {
    for (std::size_t i = 0; i < kBatch; ++i) benchmark::DoNotOptimize(x += i);
  }
}

BENCHMARK(BM_AddExistingLevel)->Arg(1000)->Arg(100000);
BENCHMARK(BM_AddNewLevel)->Arg(1000)->Arg(100000);
BENCHMARK(BM_Cancel)->Arg(1000)->Arg(100000);
BENCHMARK(BM_MatchOneLevel)->Arg(1000)->Arg(100000);
BENCHMARK(BM_ModifyReduce)->Arg(1000)->Arg(100000);
BENCHMARK(BM_BestPrices)->Arg(1000)->Arg(100000);
BENCHMARK(BM_CalibrationEmptyBatchWithPause);
BENCHMARK(BM_CalibrationEmptyBatchWithoutPause);

}  // namespace
