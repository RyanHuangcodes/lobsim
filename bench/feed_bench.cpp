// Microbenchmarks of the binary feed (upgrade 6), part of micro_bench (Google Benchmark).
//
// One stream of 1,000,000 requests is generated and applied to the engine once, outside any timer:
// the same noise-trader-like flow as lobsim_bench (85% limit / 15% market orders, limit prices at a
// slowly drifting reference +- 1..10 ticks so some limit orders cross, sizes 1..10, every limit order
// cancelled after an exponential lifetime of mean 1,500 new orders, 10 owners). Then:
//   BM_FeedEncode        the engine events of every request -> ITCH bytes in memory (no engine time)
//   BM_FeedParse         parse the whole feed with a handler that does nothing
//   BM_FeedParseRebuild  parse the whole feed into a FeedBook (bounded by the book, not the parser)
//   BM_FeedEngineSame    the engine itself on the same requests, for comparison on the same machine
//   BM_MoldPacketize     the whole feed into MoldUDP64 packets of at most 1,400 bytes
//   BM_MoldRead          read those packets back in sequence order into a feed (gap and duplicate checks)
// Each iteration processes the whole stream from the start; the rates are per request, per ITCH
// message and per byte.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <span>
#include <vector>

#include "feed/decoder.hpp"
#include "feed/encoder.hpp"
#include "feed/feed_book.hpp"
#include "feed/moldudp64.hpp"
#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"

namespace {

using namespace lob;

constexpr std::size_t kRequests = 1'000'000;
constexpr Price kMaxPrice = 1'000'000;

// SplitMix64; no std:: distributions (the stream is the same with any standard library).
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) {
    const std::uint64_t threshold = (0 - n) % n;
    for (;;) {
      const std::uint64_t r = next();
      if (r >= threshold) return r % n;
    }
  }
  std::int64_t range(std::int64_t lo, std::int64_t hi) {
    return lo + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(hi - lo) + 1));
  }
  double uniform01() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

 private:
  std::uint64_t state_;
};

struct Recorded {
  std::vector<Request> requests;
  std::vector<Event> events;           // every request's events, back to back
  std::vector<std::size_t> first;      // requests.size() + 1 offsets into events
  std::vector<std::byte> feed;         // the whole feed, S O to S C
  std::uint64_t feed_messages{0};
};

const Recorded& recorded() {
  static const Recorded r = [] {
    Recorded out;
    Rng rng(42);
    struct Due {
      std::uint64_t at;
      OrderId id;
      OwnerId owner;
      bool operator>(const Due& o) const { return at != o.at ? at > o.at : id > o.id; }
    };
    std::priority_queue<Due, std::vector<Due>, std::greater<Due>> cancels;
    Price ref = 10'000;
    OrderId new_orders = 0;
    out.requests.reserve(kRequests);
    for (std::size_t i = 0; i < kRequests; ++i) {
      if (!cancels.empty() && cancels.top().at <= new_orders) {
        out.requests.push_back(Cancel{cancels.top().owner, cancels.top().id});
        cancels.pop();
        continue;
      }
      if (rng.below(100) < 2) ref = std::clamp<Price>(ref + (rng.below(2) == 0 ? 1 : -1), 100, kMaxPrice - 100);
      const OrderId id = ++new_orders;
      const auto owner = static_cast<OwnerId>(rng.range(1, 10));
      const Side side = rng.below(2) == 0 ? Side::Buy : Side::Sell;
      if (rng.uniform01() >= 0.85) {
        out.requests.push_back(NewOrder{owner, id, side, OrderType::Market, TIF::IOC, 0, rng.range(1, 10), false, STP::CancelTaking});
        continue;
      }
      const Price d = rng.range(1, 10);
      const Qty qty = rng.range(1, 10);
      const auto lifetime = static_cast<std::uint64_t>(-std::log(1.0 - rng.uniform01()) * 1500.0);
      cancels.push(Due{new_orders + 1 + lifetime, id, owner});
      out.requests.push_back(NewOrder{owner, id, side, OrderType::Limit, TIF::GTC, side == Side::Buy ? ref - d : ref + d,
                                      qty, false, STP::CancelTaking});
    }
    OrderBook book{BookConfig{kMaxPrice}};
    EventBuffer ev;
    out.first.push_back(0);
    for (const Request& req : out.requests) {
      ev.clear();
      apply(book, req, ev);
      out.events.insert(out.events.end(), ev.begin(), ev.end());
      out.first.push_back(out.events.size());
    }
    feed::Encoder enc(nullptr, kRequests, 64u << 20);
    enc.start(0);
    for (std::size_t k = 0; k < out.requests.size(); ++k) {
      enc.on_request(static_cast<std::int64_t>(k), out.requests[k],
                     std::span<const Event>(out.events.data() + out.first[k], out.first[k + 1] - out.first[k]));
    }
    enc.finish(static_cast<std::int64_t>(kRequests));
    out.feed.assign(enc.buffered().begin(), enc.buffered().end());
    out.feed_messages = enc.counts().messages();
    return out;
  }();
  return r;
}

void rates(benchmark::State& state, const Recorded& r) {
  const auto it = static_cast<double>(state.iterations());
  state.counters["requests/s"] = benchmark::Counter(it * static_cast<double>(r.requests.size()), benchmark::Counter::kIsRate);
  state.counters["itch_msgs/s"] = benchmark::Counter(it * static_cast<double>(r.feed_messages), benchmark::Counter::kIsRate);
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(r.feed.size()));
}

void BM_FeedEncode(benchmark::State& state) {
  const Recorded& r = recorded();
  std::optional<feed::Encoder> enc;  // memory mode: drained after every request, as `check` does
  for (auto _ : state) {
    state.PauseTiming();
    enc.emplace(nullptr, kRequests);  // a fresh mirror for each pass over the stream
    state.ResumeTiming();
    enc->start(0);
    for (std::size_t k = 0; k < r.requests.size(); ++k) {
      enc->clear();
      enc->on_request(static_cast<std::int64_t>(k), r.requests[k],
                      std::span<const Event>(r.events.data() + r.first[k], r.first[k + 1] - r.first[k]));
      benchmark::DoNotOptimize(enc->buffered().data());
    }
    enc->finish(static_cast<std::int64_t>(kRequests));
    benchmark::ClobberMemory();
  }
  rates(state, r);
}

// Sums a field of every message so the parse cannot be optimised away.
struct SumHandler {
  std::uint64_t sum{0};
  bool on(const feed::MsgS& m) { return sum += m.p.timestamp, true; }
  bool on(const feed::MsgA& m) { return sum += m.shares, true; }
  bool on(const feed::MsgE& m) { return sum += m.shares, true; }
  bool on(const feed::MsgX& m) { return sum += m.shares, true; }
  bool on(const feed::MsgD& m) { return sum += m.ref, true; }
  bool on_unknown(std::uint8_t, std::span<const std::byte>) { return true; }
};

void BM_FeedParse(benchmark::State& state) {
  const Recorded& r = recorded();
  for (auto _ : state) {
    SumHandler h;
    feed::ParseResult res = feed::parse(r.feed, h);
    benchmark::DoNotOptimize(h.sum);
    benchmark::DoNotOptimize(res.messages);
  }
  rates(state, r);
}

void BM_FeedParseRebuild(benchmark::State& state) {
  const Recorded& r = recorded();
  for (auto _ : state) {
    feed::FeedBook book(kRequests);
    const feed::ParseResult res = feed::parse(r.feed, book);
    if (!res.ok()) state.SkipWithError("the feed did not rebuild");
    benchmark::DoNotOptimize(book.book().best_bid());
  }
  rates(state, r);
}

void BM_FeedEngineSame(benchmark::State& state) {
  const Recorded& r = recorded();
  EventBuffer ev;
  ev.reserve(4096);
  for (auto _ : state) {
    OrderBook book{BookConfig{kMaxPrice}};
    for (const Request& req : r.requests) {
      ev.clear();
      apply(book, req, ev);
      benchmark::DoNotOptimize(ev.data());
    }
  }
  rates(state, r);
}

void BM_MoldPacketize(benchmark::State& state) {
  const Recorded& r = recorded();
  for (auto _ : state) {
    std::vector<std::byte> packets = feed::mold::packetize(r.feed, feed::mold::PacketizeOptions{});
    benchmark::DoNotOptimize(packets.data());
  }
  rates(state, r);
}

void BM_MoldRead(benchmark::State& state) {
  const Recorded& r = recorded();
  const std::vector<std::byte> packets = feed::mold::packetize(r.feed, feed::mold::PacketizeOptions{});
  for (auto _ : state) {
    feed::mold::Reassembler back;
    back.feed.reserve(r.feed.size());
    feed::mold::ReadResult res = feed::mold::read_packets(packets, back);
    if (!res.complete()) state.SkipWithError("the packets did not read back");
    benchmark::DoNotOptimize(back.feed.data());
  }
  rates(state, r);
}

BENCHMARK(BM_FeedEncode)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_FeedParse)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_FeedParseRebuild)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_FeedEngineSame)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MoldPacketize)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MoldRead)->Unit(benchmark::kMillisecond);

}  // namespace
