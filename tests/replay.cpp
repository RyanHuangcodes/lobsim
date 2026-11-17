// The order log reproduces the run: parsing the text and replaying the requests into a
// fresh OrderBook gives the engine event hash the exchange recorded (SPEC 5.4, 5.7).
#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "lob/apply.hpp"
#include "lob/order_book.hpp"
#include "sim/order_log.hpp"
#include "support/sim_support.hpp"

using namespace sim;

namespace {

struct Replayed {
  std::uint64_t hash;
  std::uint64_t requests;
  std::uint64_t events;
  TimeNs last_time;
};

Replayed replay(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  EXPECT_TRUE(std::getline(in, line));
  const auto max_price = parse_header(line);
  EXPECT_TRUE(max_price);
  lob::OrderBook book(lob::BookConfig{max_price.value_or(1)});
  lob::EventHasher hasher;
  lob::EventBuffer events;
  Replayed r{0, 0, 0, 0};
  while (std::getline(in, line)) {
    const auto parsed = parse_request(line);
    EXPECT_TRUE(parsed) << line;
    if (!parsed) continue;
    EXPECT_GE(parsed->first, r.last_time) << "log is in arrival order";
    r.last_time = parsed->first;
    events.clear();
    lob::apply(book, parsed->second, events);
    hasher.add(events);
    r.events += events.size();
    ++r.requests;
  }
  r.hash = hasher.value();
  return r;
}

}  // namespace

TEST(Replay, LogParsedFromTextReproducesTheEngineHash) {
  for (std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    std::ostringstream log;
    Simulator s(simtest::noise_config(10, seed, 60.0), SimulatorOptions{&log});
    s.run();
    const Replayed r = replay(log.str());
    EXPECT_EQ(r.hash, s.exchange().event_hash()) << "seed " << seed;
    EXPECT_EQ(r.requests, s.exchange().requests());
    EXPECT_EQ(r.events, s.exchange().events());
    EXPECT_GT(s.exchange().trades(), 0u);
  }
}

TEST(Replay, ReplayIsSensitiveToTheLog) {
  std::ostringstream log;
  Simulator s(simtest::noise_config(10, 4, 20.0), SimulatorOptions{&log});
  s.run();
  std::string text = log.str();
  // Drop the last request line: the hash must change (or the replay is blind).
  text.pop_back();
  text.erase(text.rfind('\n') + 1);
  EXPECT_NE(replay(text).hash, s.exchange().event_hash());
}
