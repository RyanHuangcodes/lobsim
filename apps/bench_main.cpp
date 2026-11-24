// lobsim_bench: throughput and per-message service time of the matching engine (SPEC 7).
//
// What is measured: the time one thread spends inside the engine for each message of a
// seeded, noise-trader-like stream, with the next message sent only after the previous one
// returned (a closed loop). This is in-process service time. It is not end-to-end latency
// under load: there is no network, no queue and no concurrent traffic, and a closed loop
// hides queueing delay (coordinated omission).
//
//   lobsim_bench [--messages N] [--seed S] [--ref-messages M]
#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <queue>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "lob/reference_book.hpp"
#include "lob/types.hpp"

#ifndef LOBSIM_BUILD_TYPE
#define LOBSIM_BUILD_TYPE "unknown"
#endif
#ifndef LOBSIM_CXX_FLAGS
#define LOBSIM_CXX_FLAGS "unknown"
#endif

// ---------------------------------------------------------------- allocation counter
//
// Global operator new/delete are replaced in this binary only. Allocations are counted
// while `counting` is true, which the harness sets only around engine calls.
namespace alloc_count {

struct SizeCount {
  std::size_t size;
  std::uint64_t count;
};

bool counting = false;
std::uint64_t allocs = 0;
std::uint64_t frees = 0;
std::uint64_t bytes = 0;
std::array<SizeCount, 64> by_size{};
std::size_t distinct = 0;
std::uint64_t other_sizes = 0;  // allocations whose size did not fit in by_size

void reset() {
  allocs = frees = bytes = other_sizes = 0;
  distinct = 0;
}

void record(std::size_t n) noexcept {
  ++allocs;
  bytes += n;
  for (std::size_t i = 0; i < distinct; ++i) {
    if (by_size[i].size == n) {
      ++by_size[i].count;
      return;
    }
  }
  if (distinct < by_size.size()) by_size[distinct++] = SizeCount{n, 1};
  else ++other_sizes;
}

void* allocate(std::size_t n) {
  void* p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr) throw std::bad_alloc();
  if (counting) record(n);
  return p;
}

void* allocate_aligned(std::size_t n, std::size_t align) {
  if (align < sizeof(void*)) align = sizeof(void*);
  void* p = nullptr;
  if (posix_memalign(&p, align, n == 0 ? align : n) != 0) throw std::bad_alloc();
  if (counting) record(n);
  return p;
}

void release(void* p) noexcept {
  if (p == nullptr) return;
  if (counting) ++frees;
  std::free(p);
}

}  // namespace alloc_count

void* operator new(std::size_t n) { return alloc_count::allocate(n); }
void* operator new[](std::size_t n) { return alloc_count::allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return alloc_count::allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return alloc_count::allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new(std::size_t n, std::align_val_t a) {
  return alloc_count::allocate_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return alloc_count::allocate_aligned(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  try {
    return alloc_count::allocate_aligned(n, static_cast<std::size_t>(a));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  try {
    return alloc_count::allocate_aligned(n, static_cast<std::size_t>(a));
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p) noexcept { alloc_count::release(p); }
void operator delete[](void* p) noexcept { alloc_count::release(p); }
void operator delete(void* p, std::size_t) noexcept { alloc_count::release(p); }
void operator delete[](void* p, std::size_t) noexcept { alloc_count::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { alloc_count::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { alloc_count::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { alloc_count::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { alloc_count::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { alloc_count::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { alloc_count::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { alloc_count::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { alloc_count::release(p); }

namespace {

using namespace lob;

// ---------------------------------------------------------------- clock

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct TimerCost {
  double min_pair_ns;     // cheapest back-to-back pair of clock reads
  double median_pair_ns;  // typical back-to-back pair
  double mean_read_ns;    // cost of one clock read, from a long run of reads
  double step_ns;         // smallest non-zero difference between two reads (resolution)
};

TimerCost measure_timer() {
  constexpr std::size_t n = 1'000'000;
  std::vector<std::uint64_t> d(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint64_t a = now_ns();
    const std::uint64_t b = now_ns();
    d[i] = b - a;
  }
  std::uint64_t step = 0;
  for (std::uint64_t v : d)
    if (v != 0 && (step == 0 || v < step)) step = v;
  std::sort(d.begin(), d.end());
  const std::uint64_t t0 = now_ns();
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < n; ++i) sum += now_ns();
  const std::uint64_t t1 = now_ns();
  volatile std::uint64_t sink = sum;  // the reads above must not be optimised away
  (void)sink;
  return TimerCost{static_cast<double>(d.front()), static_cast<double>(d[n / 2]),
                   static_cast<double>(t1 - t0) / static_cast<double>(n), static_cast<double>(step)};
}

// ---------------------------------------------------------------- message stream

// SplitMix64 with the few helpers the stream needs. No std:: distributions: a seed gives
// the same stream with any standard library.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) {  // uniform in [0, n), rejection sampling
    const std::uint64_t threshold = (0 - n) % n;
    for (;;) {
      const std::uint64_t r = next();
      if (r >= threshold) return r % n;
    }
  }
  std::int64_t range(std::int64_t lo, std::int64_t hi) {  // uniform in [lo, hi]
    return lo + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(hi - lo) + 1));
  }
  double uniform01() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  double exponential(double mean) { return -std::log(1.0 - uniform01()) * mean; }

 private:
  std::uint64_t state_;
};

// A noise-trader-like flow (compare SPEC 5.6): 85% limit and 15% market orders, limit buys
// at ref - d and sells at ref + d with d uniform in [1, 10], sizes uniform in [1, 10], a
// reference price that drifts slowly so some limit orders cross, and every limit order
// cancelled after an exponential lifetime measured in new-order arrivals. A cancel is sent
// even if its order was filled in the meantime (it then gets UnknownOrder), as a real
// trader's would be. Every new order is valid, so order ids are dense and predictable
// (the n-th new order gets id n) and the stream needs no feedback from the book.
class Stream {
 public:
  static constexpr Price kMaxPrice = 1'000'000;
  static constexpr double kLimitShare = 0.85;
  static constexpr double kLifetimeMeanNewOrders = 1500.0;  // ~1,275 limit orders alive
  static constexpr OwnerId kOwners = 10;

  explicit Stream(std::uint64_t seed) : rng_(seed) {}

  void fill(std::vector<Request>& out, std::size_t n) {
    out.clear();
    for (std::size_t i = 0; i < n; ++i) out.push_back(next());
  }

  OrderId last_new_order_id() const { return new_orders_; }

 private:
  struct Due {
    std::uint64_t at;  // cancel once this many new orders have been sent
    OrderId id;
    OwnerId owner;
    bool operator>(const Due& o) const { return at != o.at ? at > o.at : id > o.id; }
  };

  Request next() {
    if (!cancels_.empty() && cancels_.top().at <= new_orders_) {
      const Due d = cancels_.top();
      cancels_.pop();
      return Cancel{d.owner, d.id};
    }
    if (rng_.below(100) < 2) ref_ = std::clamp<Price>(ref_ + (rng_.below(2) == 0 ? 1 : -1), 100, kMaxPrice - 100);
    const OrderId id = ++new_orders_;
    const auto owner = static_cast<OwnerId>(rng_.range(1, kOwners));
    const Side side = rng_.below(2) == 0 ? Side::Buy : Side::Sell;
    const bool is_limit = rng_.uniform01() < kLimitShare;
    if (!is_limit) {
      const Qty qty = rng_.range(1, 10);
      return NewOrder{owner, id, side, OrderType::Market, TIF::IOC, 0, qty, false, STP::CancelTaking};
    }
    const Price d = rng_.range(1, 10);
    const Qty qty = rng_.range(1, 10);
    const Price price = side == Side::Buy ? ref_ - d : ref_ + d;
    const auto lifetime = static_cast<std::uint64_t>(rng_.exponential(kLifetimeMeanNewOrders));
    cancels_.push(Due{new_orders_ + 1 + lifetime, id, owner});
    return NewOrder{owner, id, side, OrderType::Limit, TIF::GTC, price, qty, false, STP::CancelTaking};
  }

  Rng rng_;
  Price ref_{10'000};
  OrderId new_orders_{0};
  std::priority_queue<Due, std::vector<Due>, std::greater<Due>> cancels_;
};

// ---------------------------------------------------------------- one timed pass

struct AllocTotals {
  std::uint64_t allocs{0}, frees{0}, bytes{0}, other_sizes{0};
  std::vector<alloc_count::SizeCount> by_size;
};

struct PassResult {
  std::uint64_t messages{0};
  std::uint64_t engine_ns{0};
  std::uint64_t events{0};
  std::vector<float> ns_per_message;  // one sample per timed batch
  AllocTotals allocs;
  // Filled only when the batch size is 1 (events are inspected after each timed call).
  std::uint64_t hash{0};
  std::array<std::uint64_t, 5> event_counts{};  // by Event alternative
  std::uint64_t unknown_order_rejects{0};
  std::uint64_t id_mismatches{0};
  std::size_t resting_at_end{0};
  std::size_t bid_levels{0}, ask_levels{0};
};

constexpr std::size_t kBlock = 4096;  // messages generated at a time, outside any timing

// Feeds `messages` messages of the stream to a fresh `Book`, timing the engine calls in
// batches of `batch` messages (1 = one timer pair per message). Generation is never timed.
template <class Book>
PassResult run_pass(std::uint64_t seed, std::uint64_t messages, std::size_t batch) {
  PassResult r;
  r.ns_per_message.reserve(static_cast<std::size_t>(messages / batch + 1));
  // Each message is at most one new order, so `messages` bounds the ids: the id index is sized up
  // front and no message pays for growing it (D-260). ReferenceBook ignores the field.
  Book book{BookConfig{Stream::kMaxPrice, static_cast<std::size_t>(messages)}};
  Stream stream(seed);
  std::vector<Request> block;
  block.reserve(kBlock);
  EventBuffer ev;
  ev.reserve(4096);  // never grows during the run: a message cannot produce this many events
  EventHasher hasher;
  OrderId expected_id = 0;

  alloc_count::reset();
  std::uint64_t done = 0;
  while (done < messages) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, messages - done));
    stream.fill(block, n);
    for (std::size_t at = 0; at < n; at += batch) {
      const std::size_t end = std::min(n, at + batch);
      std::uint64_t events = 0;
      alloc_count::counting = true;
      const std::uint64_t t0 = now_ns();
      for (std::size_t k = at; k < end; ++k) {
        ev.clear();
        apply(book, block[k], ev);
        events += ev.size();
      }
      const std::uint64_t t1 = now_ns();
      alloc_count::counting = false;
      r.engine_ns += t1 - t0;
      r.events += events;
      r.ns_per_message.push_back(static_cast<float>(static_cast<double>(t1 - t0) / static_cast<double>(end - at)));
      if (batch == 1) {
        hasher.add(ev);
        for (const Event& e : ev) {
          ++r.event_counts[e.index()];
          if (const auto* rej = std::get_if<Reject>(&e))
            if (rej->reason == RejectReason::UnknownOrder) ++r.unknown_order_rejects;
        }
        if (std::holds_alternative<NewOrder>(block[at])) {
          ++expected_id;
          const auto* ack = ev.empty() ? nullptr : std::get_if<Ack>(&ev.front());
          if (ack == nullptr || ack->order_id != expected_id) ++r.id_mismatches;
        }
      }
    }
    done += n;
  }
  r.messages = messages;
  r.hash = hasher.value();
  r.allocs.allocs = alloc_count::allocs;
  r.allocs.frees = alloc_count::frees;
  r.allocs.bytes = alloc_count::bytes;
  r.allocs.other_sizes = alloc_count::other_sizes;
  r.allocs.by_size.assign(alloc_count::by_size.begin(),
                          alloc_count::by_size.begin() + static_cast<std::ptrdiff_t>(alloc_count::distinct));
  r.resting_at_end = book.resting_orders();
  r.bid_levels = book.depth(Side::Buy, static_cast<std::size_t>(-1)).size();
  r.ask_levels = book.depth(Side::Sell, static_cast<std::size_t>(-1)).size();
  return r;
}

struct Percentiles {
  double p50, p90, p99, p999, max;
};

Percentiles percentiles(std::vector<float> v) {
  if (v.empty()) return Percentiles{0, 0, 0, 0, 0};
  std::sort(v.begin(), v.end());
  auto at = [&](double p) {  // nearest rank
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size())));
    return static_cast<double>(v[std::min(v.size() - 1, rank == 0 ? 0 : rank - 1)]);
  };
  return Percentiles{at(0.50), at(0.90), at(0.99), at(0.999), static_cast<double>(v.back())};
}

double msgs_per_sec(const PassResult& r) {
  return r.engine_ns == 0 ? 0.0 : static_cast<double>(r.messages) * 1e9 / static_cast<double>(r.engine_ns);
}
double mean_ns(const PassResult& r) {
  return r.messages == 0 ? 0.0 : static_cast<double>(r.engine_ns) / static_cast<double>(r.messages);
}

// ---------------------------------------------------------------- allocation audit

struct AuditRow {
  const char* operation;
  std::uint64_t ops, allocs, frees;
};

// Counts allocations per kind of operation on a warm book: 20 levels of 50 orders per
// side, the pool and the id index already grown past everything the audit will use.
std::vector<AuditRow> allocation_audit() {
  OrderBook book{BookConfig{Stream::kMaxPrice}};
  EventBuffer ev;
  ev.reserve(4096);
  OrderId next_id = 1;
  auto submit = [&](const NewOrder& o) {
    ev.clear();
    book.submit(o, ev);
    return next_id++;
  };
  auto gtc = [](OwnerId owner, Side side, Price price, Qty qty) {
    return NewOrder{owner, 0, side, OrderType::Limit, TIF::GTC, price, qty, false, STP::CancelTaking};
  };
  auto ioc = [](OwnerId owner, Side side, Price price, Qty qty) {
    return NewOrder{owner, 0, side, OrderType::Limit, TIF::IOC, price, qty, false, STP::CancelTaking};
  };

  std::vector<OrderId> asks;
  for (int level = 0; level < 20; ++level)
    for (int k = 0; k < 50; ++k) {
      submit(gtc(1, Side::Buy, 9'990 - level, 10));
      asks.push_back(submit(gtc(1, Side::Sell, 10'010 + level, 10)));
    }
  // Warm-up: grow the pool and the id index well past what the audit uses, then free it.
  std::vector<OrderId> warm;
  for (int i = 0; i < 7'000; ++i) warm.push_back(submit(gtc(1, Side::Buy, 9'000, 1)));
  for (OrderId id : warm) {
    ev.clear();
    book.cancel(Cancel{1, id}, ev);
  }

  std::vector<AuditRow> rows;
  auto counted = [&](const char* name, std::uint64_t ops, const std::function<void()>& body) {
    alloc_count::reset();
    alloc_count::counting = true;
    body();
    alloc_count::counting = false;
    rows.push_back(AuditRow{name, ops, alloc_count::allocs, alloc_count::frees});
  };
  // `setup` runs uncounted before each counted `op`.
  auto counted_each = [&](const char* name, int ops, const std::function<void(int)>& setup,
                          const std::function<void(int)>& op) {
    std::uint64_t allocs = 0, frees = 0;
    for (int i = 0; i < ops; ++i) {
      setup(i);
      alloc_count::reset();
      alloc_count::counting = true;
      op(i);
      alloc_count::counting = false;
      allocs += alloc_count::allocs;
      frees += alloc_count::frees;
    }
    rows.push_back(AuditRow{name, static_cast<std::uint64_t>(ops), allocs, frees});
  };

  std::vector<OrderId> in_existing, in_new;
  in_existing.reserve(500);  // so the counted sections contain engine allocations only
  in_new.reserve(500);
  counted("add, existing price level", 500, [&] {
    for (int i = 0; i < 500; ++i) in_existing.push_back(submit(gtc(2, Side::Buy, 9'990 - i % 20, 1)));
  });
  counted("add, new price level", 500, [&] {
    for (int i = 0; i < 500; ++i) in_new.push_back(submit(gtc(2, Side::Buy, 9'900 - i, 1)));
  });
  counted("cancel, level stays", 500, [&] {
    for (OrderId id : in_existing) {
      ev.clear();
      book.cancel(Cancel{2, id}, ev);
    }
  });
  counted("cancel, level becomes empty", 500, [&] {
    for (OrderId id : in_new) {
      ev.clear();
      book.cancel(Cancel{2, id}, ev);
    }
  });
  counted("match within one level, level stays", 400, [&] {
    for (int i = 0; i < 400; ++i) submit(ioc(3, Side::Sell, 9'990, 1));
  });
  counted_each(
      "match that empties a level", 200, [&](int) { submit(gtc(2, Side::Sell, 10'005, 5)); },
      [&](int) { submit(ioc(3, Side::Buy, 10'005, 5)); });
  counted("modify, same price, smaller qty", 500, [&] {
    for (int i = 0; i < 500; ++i) {
      ev.clear();
      book.modify(Modify{1, asks[static_cast<std::size_t>(i)], 10'010 + i / 50, 9}, ev);
    }
  });
  counted("modify to a new price level", 200, [&] {
    for (int i = 0; i < 200; ++i) {
      ev.clear();
      book.modify(Modify{1, asks[static_cast<std::size_t>(500 + i)], 11'000 + i, 9}, ev);
    }
  });
  return rows;
}

// ---------------------------------------------------------------- environment

std::string cpu_model() {
#if defined(__APPLE__)
  char buf[256];
  std::size_t len = sizeof(buf);
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return std::string(buf);
#else
  if (std::FILE* f = std::fopen("/proc/cpuinfo", "r")) {
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      if (std::strncmp(line, "model name", 10) == 0) {
        std::fclose(f);
        const char* colon = std::strchr(line, ':');
        std::string s = colon != nullptr ? colon + 1 : line;
        while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        return s;
      }
    }
    std::fclose(f);
  }
#endif
  return "unknown CPU";
}

std::string compiler() {
#if defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("gcc ") + __VERSION__;
#else
  return "unknown compiler";
#endif
}

bool parse_u64(const char* s, std::uint64_t& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || *end != '\0') return false;
  out = v;
  return true;
}

int usage() {
  std::fprintf(stderr, "usage: lobsim_bench [--messages N] [--seed S] [--ref-messages M]\n");
  return 2;
}

void print_percentiles(const char* label, const Percentiles& p) {
  std::printf("  %-44s p50 %8.0f   p90 %8.0f   p99 %8.0f   p99.9 %8.0f   max %10.0f  ns\n", label, p.p50, p.p90,
              p.p99, p.p999, p.max);
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t messages = 10'000'000, seed = 42, ref_messages = 100'000;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](std::uint64_t& out) { return i + 1 < argc && parse_u64(argv[++i], out); };
    if (a == "--messages") {
      if (!value(messages)) return usage();
    } else if (a == "--seed") {
      if (!value(seed)) return usage();
    } else if (a == "--ref-messages") {
      if (!value(ref_messages)) return usage();
    } else {
      return usage();
    }
  }
  if (messages == 0) return usage();
  ref_messages = std::min(ref_messages, messages);

  const std::string env = cpu_model() + "; " + compiler() + "; " + LOBSIM_BUILD_TYPE + "; flags: " + LOBSIM_CXX_FLAGS;
  std::printf("lobsim_bench\n  machine and build: %s\n", env.c_str());
  std::printf("  OrderBook id index pre-sized for %" PRIu64 " order ids (BookConfig::expected_order_ids = the message count)\n",
              messages);
#ifndef NDEBUG
  std::printf("  WARNING: NDEBUG is not defined. This is not an optimised build (and the book validates itself\n"
              "  after every message). The numbers below are NOT benchmark results.\n");
#endif
  std::printf("  stream: seed %" PRIu64 ", %" PRIu64 " messages, %.0f%% limit / %.0f%% market new orders, every limit\n"
              "  order cancelled after an exponential lifetime (mean %.0f new orders), %u owners\n\n",
              seed, messages, Stream::kLimitShare * 100, (1 - Stream::kLimitShare) * 100,
              Stream::kLifetimeMeanNewOrders, Stream::kOwners);

  const TimerCost timer = measure_timer();

  // Three passes over the same stream, each on a fresh OrderBook.
  const PassResult thr = run_pass<OrderBook>(seed, messages, kBlock);  // throughput: one timer pair per block
  const PassResult b32 = run_pass<OrderBook>(seed, messages, 32);      // batches of 32
  const PassResult one = run_pass<OrderBook>(seed, messages, 1);       // one timer pair per message
  const Percentiles p_one = percentiles(one.ns_per_message);
  const Percentiles p_b32 = percentiles(b32.ns_per_message);

  // ReferenceBook on the first ref_messages of the same stream, and OrderBook on the same
  // prefix for the hash comparison.
  const PassResult ref_thr = run_pass<ReferenceBook>(seed, ref_messages, kBlock);
  const PassResult ref_one = run_pass<ReferenceBook>(seed, ref_messages, 1);
  const PassResult fast_prefix = run_pass<OrderBook>(seed, ref_messages, 1);
  const PassResult fast_prefix_thr = run_pass<OrderBook>(seed, ref_messages, kBlock);
  const Percentiles p_ref = percentiles(ref_one.ns_per_message);

  std::printf("OrderBook, %" PRIu64 " messages\n", messages);
  std::printf("  throughput (timed per block of %zu)          %.2f million msgs/s   (mean %.1f ns per message)\n",
              kBlock, msgs_per_sec(thr) / 1e6, mean_ns(thr));
  print_percentiles("service time, one timer pair per message", p_one);
  print_percentiles("service time, mean of each batch of 32", p_b32);
  std::printf("  mean of the per-message samples               %.1f ns (compare with the block-timed mean above)\n",
              mean_ns(one));
  std::printf("  event hash %016" PRIx64 "; events: %" PRIu64 " Ack, %" PRIu64 " Reject (%" PRIu64
              " UnknownOrder), %" PRIu64 " Trade, %" PRIu64 " Cancelled\n",
              one.hash, one.event_counts[0], one.event_counts[1], one.unknown_order_rejects, one.event_counts[2],
              one.event_counts[3]);
  std::printf("  book at the end: %zu resting orders on %zu bid and %zu ask levels\n\n", one.resting_at_end,
              one.bid_levels, one.ask_levels);

  // The timer matters if its step or the cost of a pair of reads is not small next to the
  // time of one message (taken from the block-timed pass, which the timer barely affects).
  const double per_message_ns = mean_ns(thr);
  const bool timer_significant = timer.median_pair_ns > 0.1 * per_message_ns || timer.step_ns > 0.1 * per_message_ns;
  std::printf("timer (std::chrono::steady_clock)\n");
  std::printf("  back-to-back pair of reads: min %.0f ns, median %.0f ns; one read costs about %.1f ns; smallest\n"
              "  non-zero step %.0f ns\n",
              timer.min_pair_ns, timer.median_pair_ns, timer.mean_read_ns, timer.step_ns);
  if (!timer_significant) {
    std::printf("  This is small next to the block-timed mean of %.1f ns per message, so the per-message samples can\n"
                "  be read directly.\n\n",
                per_message_ns);
  } else {
    std::printf("  This is SIGNIFICANT next to the block-timed mean of %.1f ns per message.\n", per_message_ns);
    if (per_message_ns < timer.step_ns)
      std::printf("  One message takes less than one timer step, so a per-message sample is a whole number of steps\n"
                  "  (often 0) and never the true value.\n");
    else
      std::printf("  A per-message sample is rounded to the timer step and includes part of the timer's own cost.\n");
    std::printf("  Use the block-timed throughput for the average cost and the batch-of-32 row for the body of the\n"
                "  distribution. The per-message row is the one to read for the far tail and the maximum, where\n"
                "  the timer no longer matters.\n\n");
  }

  std::printf("ReferenceBook, first %" PRIu64 " messages of the same stream\n", ref_messages);
  std::printf("  throughput (timed per block of %zu)          %.3f million msgs/s   (mean %.1f ns per message)\n",
              kBlock, msgs_per_sec(ref_thr) / 1e6, mean_ns(ref_thr));
  print_percentiles("service time, one timer pair per message", p_ref);
  std::printf("  event hash %016" PRIx64 "; OrderBook on the same %" PRIu64 " messages: %016" PRIx64 "\n\n", ref_one.hash,
              ref_messages, fast_prefix.hash);

  const double allocs_per_msg = static_cast<double>(thr.allocs.allocs) / static_cast<double>(messages);
  std::printf("heap allocations inside OrderBook calls, %" PRIu64 " messages (global operator new counted)\n", messages);
  std::printf("  %" PRIu64 " allocations (%.4f per message), %" PRIu64 " deallocations, %.1f MB requested in total\n",
              thr.allocs.allocs, allocs_per_msg, thr.allocs.frees, static_cast<double>(thr.allocs.bytes) / 1e6);
  std::vector<alloc_count::SizeCount> sizes = thr.allocs.by_size;
  std::sort(sizes.begin(), sizes.end(),
            [](const alloc_count::SizeCount& a, const alloc_count::SizeCount& b) { return a.size < b.size; });
  std::uint64_t small_allocs = 0;
  std::size_t small_size = 0;
  for (const auto& s : sizes) {
    if (s.count > small_allocs) {
      small_allocs = s.count;
      small_size = s.size;
    }
    std::printf("    %10zu bytes x %" PRIu64 "\n", s.size, s.count);
  }
  if (thr.allocs.other_sizes != 0) std::printf("    (other sizes) x %" PRIu64 "\n", thr.allocs.other_sizes);
  if (small_allocs != 0)
    std::printf("  The %zu-byte allocations are std::map nodes, one per new price level. The larger ones are growth\n"
                "  of the order pool (a chunk of orders plus its free list); the id index was sized before the run.\n",
                small_size);
  std::printf("\n");

  const std::vector<AuditRow> audit = allocation_audit();
  std::printf("allocation audit on a warm book (pool and id index already grown)\n");
  for (const AuditRow& row : audit)
    std::printf("  %-38s %4" PRIu64 " operations: %4" PRIu64 " allocations, %4" PRIu64 " deallocations\n", row.operation,
                row.ops, row.allocs, row.frees);
  std::printf("\n");

  // ---- checks: fail loudly rather than print numbers from a broken run
  bool ok = true;
  if (thr.events != one.events || thr.events != b32.events) {
    std::printf("FAIL: the passes saw different event counts (%" PRIu64 ", %" PRIu64 ", %" PRIu64 ")\n", thr.events,
                b32.events, one.events);
    ok = false;
  }
  if (one.id_mismatches != 0) {
    std::printf("FAIL: %" PRIu64 " new orders did not get the predicted order id\n", one.id_mismatches);
    ok = false;
  }
  if (ref_one.hash != fast_prefix.hash) {
    std::printf("FAIL: ReferenceBook and OrderBook event hashes differ on the first %" PRIu64 " messages\n",
                ref_messages);
    ok = false;
  }

  std::printf("Markdown summary\n\n");
  std::printf("%s\n\n", env.c_str());
  std::printf("| Measurement | Result |\n|---|---|\n");
  std::printf("| OrderBook throughput, %" PRIu64 " messages (timed per block of %zu) | %.2f million msgs/s (mean %.1f ns per message) |\n",
              messages, kBlock, msgs_per_sec(thr) / 1e6, mean_ns(thr));
  std::printf("| OrderBook service time, one timer pair per message | p50 %.0f ns, p90 %.0f ns, p99 %.0f ns, p99.9 %.0f ns, max %.0f ns |\n",
              p_one.p50, p_one.p90, p_one.p99, p_one.p999, p_one.max);
  std::printf("| OrderBook service time, mean of each batch of 32 messages | p50 %.0f ns, p90 %.0f ns, p99 %.0f ns, p99.9 %.0f ns, max %.0f ns |\n",
              p_b32.p50, p_b32.p90, p_b32.p99, p_b32.p999, p_b32.max);
  std::printf("| Timer: back-to-back pair of clock reads | min %.0f ns, median %.0f ns, smallest step %.0f ns |\n",
              timer.min_pair_ns, timer.median_pair_ns, timer.step_ns);
  std::printf("| Heap allocations inside OrderBook calls | %" PRIu64 " in %" PRIu64 " messages (%.4f per message) |\n",
              thr.allocs.allocs, messages, allocs_per_msg);
  std::printf("| ReferenceBook throughput, first %" PRIu64 " messages | %.3f million msgs/s (mean %.0f ns per message) |\n",
              ref_messages, msgs_per_sec(ref_thr) / 1e6, mean_ns(ref_thr));
  std::printf("| ReferenceBook service time, one timer pair per message | p50 %.0f ns, p90 %.0f ns, p99 %.0f ns, p99.9 %.0f ns, max %.0f ns |\n",
              p_ref.p50, p_ref.p90, p_ref.p99, p_ref.p999, p_ref.max);
  std::printf("| OrderBook / ReferenceBook throughput on the same %" PRIu64 " messages | %.0fx |\n", ref_messages,
              msgs_per_sec(fast_prefix_thr) / std::max(1.0, msgs_per_sec(ref_thr)));
  std::printf("| Event hash, OrderBook vs ReferenceBook, first %" PRIu64 " messages | %s (%016" PRIx64 ") |\n", ref_messages,
              ref_one.hash == fast_prefix.hash ? "identical" : "DIFFERENT", fast_prefix.hash);
  std::printf("\nThese are in-process service times measured in a closed loop on one thread. They are not end-to-end\n"
              "latency under load.\n");
  return ok ? 0 : 1;
}
