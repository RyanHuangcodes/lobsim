#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "calib/calibration.hpp"
#include "sim/agents/calibrated_flow.hpp"
#include "sim/config.hpp"
#include "sim/simulator.hpp"
#include "support/sim_support.hpp"

using namespace sim;
using nlohmann::json;

namespace {

const std::string kFixtures = std::string(LOBSIM_SOURCE_DIR) + "/tests/fixtures/calib/";

calib::Pmf pmf(std::vector<std::int64_t> v, std::vector<std::uint64_t> c) { return calib::Pmf{std::move(v), std::move(c)}; }

// A context with three owner ids (10, 11, 12) for the agent unit tests.
class MultiCtx final : public Context {
 public:
  explicit MultiCtx(std::uint64_t seed) : r(seed) {}
  TimeNs t{0};
  Xoshiro256StarStar r;
  std::vector<lob::Request> sent;
  std::vector<TimeNs> pending;

  TimeNs now() const override { return t; }
  void send(const lob::Request& q) override { sent.push_back(q); }
  void schedule_wakeup(TimeNs at) override { pending.push_back(at); }
  Xoshiro256StarStar& rng() override { return r; }
  lob::OwnerId self() const override { return 10; }
  std::uint32_t owner_count() const override { return 3; }
  lob::OwnerId owner_id(std::uint32_t k) const override { return 10 + k; }
  bool owns(lob::OwnerId o) const override { return o >= 10 && o <= 12; }

  void fire(Agent& a) {
    const auto it = std::min_element(pending.begin(), pending.end());
    t = *it;
    pending.erase(it);
    a.on_wakeup(*this);
  }
  const lob::NewOrder& last_new() const { return std::get<lob::NewOrder>(sent.back()); }
};

// A calibration with only sell limit orders (rate 3/s) unless changed by the caller; every
// lifetime bucket is `life`.
std::shared_ptr<calib::Calibration> make_cal() {
  auto c = std::make_shared<calib::Calibration>(calib::load(kFixtures + "flow_small.json"));
  c->rates.limit = {calib::Rate{0, 0.0}, calib::Rate{0, 3.0}};
  c->rates.aggressive = {calib::Rate{0, 0.0}, calib::Rate{0, 0.0}};
  for (int s = 0; s < 2; ++s)
    for (int b = 0; b < 3; ++b) c->placement[s][b] = pmf({0}, {1});
  c->limit_size = pmf({1}, {1});
  return c;
}

CalibratedFlowParams params(std::shared_ptr<calib::Calibration> c, lob::Price start = 500) {
  CalibratedFlowParams p;
  p.calibration = std::move(c);
  p.start_price = start;
  p.max_price = 1000;
  return p;
}

}  // namespace

// ---- the samplers -------------------------------------------------------------------------

TEST(CalibratedSampler, ThreeValuePmfMatchesItsFrequenciesWithinOnePercent) {
  const CumulativeTable table(pmf({-3, 2, 9}, {2, 3, 5}));  // 0.2, 0.3, 0.5
  EXPECT_EQ(table.total(), 10u);
  Xoshiro256StarStar rng(12345);
  std::map<std::int64_t, std::uint64_t> n;
  const int draws = 1'000'000;
  for (int i = 0; i < draws; ++i) ++n[table.sample(rng)];
  ASSERT_EQ(n.size(), 3u);
  const double want[] = {0.2, 0.3, 0.5};
  const std::int64_t at[] = {-3, 2, 9};
  for (int i = 0; i < 3; ++i) {
    const double got = static_cast<double>(n[at[i]]) / draws;
    EXPECT_NEAR(got, want[i], 0.01 * want[i]) << "value " << at[i];
  }
}

TEST(CalibratedSampler, CumulativeLookupBoundaries) {
  const CumulativeTable table(pmf({5, 6, 7}, {2, 1, 3}));  // draws 0,1 -> 5; 2 -> 6; 3,4,5 -> 7
  EXPECT_EQ(table.value_at(0), 5);
  EXPECT_EQ(table.value_at(1), 5);
  EXPECT_EQ(table.value_at(2), 6);
  EXPECT_EQ(table.value_at(3), 7);
  EXPECT_EQ(table.value_at(5), 7);
  EXPECT_THROW(table.value_at(6), std::out_of_range);
  Xoshiro256StarStar rng(1);
  EXPECT_THROW(CumulativeTable().sample(rng), std::logic_error);
}

TEST(CalibratedSampler, TotalAboveInt64IsStillExact) {
  const std::uint64_t big = (std::uint64_t{1} << 63) + 5;  // total above INT64_MAX
  const CumulativeTable table(pmf({1, 2}, {big, 3}));
  Xoshiro256StarStar rng(9);
  int ones = 0;
  for (int i = 0; i < 1000; ++i) ones += table.sample(rng) == 1;
  EXPECT_GT(ones, 990);
}

TEST(CalibratedLifetime, InverseCdfOnAHandMadeCurve) {
  calib::Survival s;
  s.times_ns = {1000, 5000, 20000};
  s.survival = {0.9, 0.6, 0.3};
  // The smallest t with S(t) <= u.
  EXPECT_EQ(lifetime_from_survival(s, 0.99), 1000);
  EXPECT_EQ(lifetime_from_survival(s, 0.9), 1000);   // S(1000) = 0.9 <= 0.9
  EXPECT_EQ(lifetime_from_survival(s, 0.89), 5000);
  EXPECT_EQ(lifetime_from_survival(s, 0.6), 5000);
  EXPECT_EQ(lifetime_from_survival(s, 0.59), 20000);
  EXPECT_EQ(lifetime_from_survival(s, 0.3), 20000);
  EXPECT_FALSE(lifetime_from_survival(s, 0.29).has_value());  // u < S(t_max): never cancelled
  EXPECT_FALSE(lifetime_from_survival(s, 0.0).has_value());
  EXPECT_FALSE(lifetime_from_survival(calib::Survival{}, 0.5).has_value());
}

// ---- the agent ----------------------------------------------------------------------------

TEST(CalibratedFlow, OrdersCycleThroughTheOwnerIds) {
  auto cal = make_cal();
  CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(1));
  MultiCtx ctx(3);
  a.on_start(ctx);
  std::vector<lob::OwnerId> owners;
  for (int i = 0; i < 7; ++i) {
    ctx.fire(a);
    owners.push_back(ctx.last_new().owner);
  }
  EXPECT_EQ(owners, (std::vector<lob::OwnerId>{10, 11, 12, 10, 11, 12, 10}));
}

TEST(CalibratedFlow, EmptyBookUsesTheStartPriceAndIsCounted) {
  auto cal = make_cal();
  cal->placement[calib::kSell][0] = pmf({2}, {1});
  CalibratedFlowAgent a(params(cal, 500), Xoshiro256StarStar(1));
  MultiCtx ctx(3);
  a.on_start(ctx);
  ctx.fire(a);
  EXPECT_EQ(ctx.last_new().side, lob::Side::Sell);
  EXPECT_EQ(ctx.last_new().price, 502);  // start price + Delta, bucket "1"
  EXPECT_EQ(a.counters().fallback_empty, 1u);
  EXPECT_EQ(a.counters().fallback_one_sided, 0u);

  auto cal2 = make_cal();
  cal2->rates.limit = {calib::Rate{0, 3.0}, calib::Rate{0, 0.0}};  // buys only
  cal2->placement[calib::kBuy][0] = pmf({2}, {1});
  CalibratedFlowAgent b(params(cal2, 500), Xoshiro256StarStar(1));
  MultiCtx c2(3);
  b.on_start(c2);
  c2.fire(b);
  EXPECT_EQ(c2.last_new().side, lob::Side::Buy);
  EXPECT_EQ(c2.last_new().price, 498);
}

TEST(CalibratedFlow, OneSidedBookFallbacks) {
  // Sells only. Bid present, ask absent: the sell has no own-side best, so the reference is
  // bid + 1 and the bucket is "1" (placement PMF {2} gives 103).
  {
    auto cal = make_cal();
    cal->placement[calib::kSell][0] = pmf({2}, {1});
    CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(1));
    MultiCtx ctx(3);
    a.on_start(ctx);
    a.on_market_data(ctx, BBO{100, 5, 0, 0});
    ctx.fire(a);
    EXPECT_EQ(ctx.last_new().price, 103);
    EXPECT_EQ(a.counters().fallback_one_sided, 1u);
    EXPECT_EQ(a.counters().fallback_empty, 0u);
  }
  // Ask present, bid absent: the sell has its own best, reference 120, bucket "3+" (PMF {4}).
  {
    auto cal = make_cal();
    cal->placement[calib::kSell][2] = pmf({4}, {1});
    CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(1));
    MultiCtx ctx(3);
    a.on_start(ctx);
    a.on_market_data(ctx, BBO{0, 0, 120, 5});
    ctx.fire(a);
    EXPECT_EQ(ctx.last_new().price, 124);
    EXPECT_EQ(a.counters().fallback_one_sided, 1u);
  }
  // Buys only, ask present, bid absent: reference ask - 1 = 119, bucket "1", Delta 3 -> 116.
  {
    auto cal = make_cal();
    cal->rates.limit = {calib::Rate{0, 3.0}, calib::Rate{0, 0.0}};
    cal->placement[calib::kBuy][0] = pmf({3}, {1});
    CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(1));
    MultiCtx ctx(3);
    a.on_start(ctx);
    a.on_market_data(ctx, BBO{0, 0, 120, 5});
    ctx.fire(a);
    EXPECT_EQ(ctx.last_new().price, 116);
  }
  // A two-sided book is no fallback and uses the spread bucket: s = 2 -> bucket "2".
  {
    auto cal = make_cal();
    cal->placement[calib::kSell][1] = pmf({-1}, {1});
    CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(1));
    MultiCtx ctx(3);
    a.on_start(ctx);
    a.on_market_data(ctx, BBO{100, 5, 102, 5});
    ctx.fire(a);
    EXPECT_EQ(ctx.last_new().price, 101);  // ask - 1: inside the spread, not crossing
    EXPECT_EQ(a.counters().fallback_one_sided + a.counters().fallback_empty, 0u);
  }
}

TEST(CalibratedFlow, CrossingPlacementsAreRedrawn) {
  auto cal = make_cal();
  cal->rates.limit = {calib::Rate{0, 3.0}, calib::Rate{0, 0.0}};
  cal->placement[calib::kBuy][2] = pmf({-20, 0}, {1, 1});  // half the draws would cross (s = 10)
  CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(1));
  MultiCtx ctx(3);
  a.on_start(ctx);
  a.on_market_data(ctx, BBO{100, 5, 110, 5});
  for (int i = 0; i < 200; ++i) {
    ctx.fire(a);
    EXPECT_EQ(ctx.last_new().price, 100) << "the only draw that does not cross is Delta = 0";
  }
  EXPECT_GT(a.counters().placement_redraws, 50u);
  EXPECT_EQ(a.counters().placement_clamped, 0u);
}

TEST(CalibratedFlow, ClampsToTheCrossingBoundAfter100Redraws) {
  auto cal = make_cal();
  cal->rates.limit = {calib::Rate{0, 3.0}, calib::Rate{0, 0.0}};
  cal->placement[calib::kBuy][2] = pmf({-20}, {1});  // always crosses
  CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(1));
  MultiCtx ctx(3);
  a.on_start(ctx);
  a.on_market_data(ctx, BBO{100, 5, 110, 5});
  for (int i = 0; i < 5; ++i) {
    ctx.fire(a);
    EXPECT_EQ(ctx.last_new().price, 109) << "bid + (s - 1): the best price that does not cross";
  }
  EXPECT_EQ(a.counters().placement_clamped, 5u);
  EXPECT_EQ(a.counters().placement_redraws, 500u);
}

namespace {

// Fires wakeups until `n` limit orders have been sent, acknowledging each one at once with
// the next order id (and checking every cancel on the way against the owner that placed it).
// Returns the owner of each order by id.
std::map<lob::OrderId, lob::OwnerId> drive(CalibratedFlowAgent& a, MultiCtx& ctx, int n) {
  std::map<lob::OrderId, lob::OwnerId> placed_by;
  lob::OrderId next_id = 1;
  while (static_cast<int>(placed_by.size()) < n) {
    const std::size_t before = ctx.sent.size();
    ctx.fire(a);
    if (ctx.sent.size() == before) continue;  // a cancel timer whose order was already gone
    if (const auto* c = std::get_if<lob::Cancel>(&ctx.sent.back())) {
      EXPECT_EQ(c->owner, placed_by.at(c->order_id));
      continue;
    }
    const auto& o = ctx.last_new();
    placed_by[next_id] = o.owner;
    a.on_private(ctx, lob::Ack{next_id, o.owner, o.client_order_id});
    ++next_id;
  }
  return placed_by;
}

}  // namespace

TEST(CalibratedFlow, NoCancelTailLeavesAboutFortyPercentOfOrdersAlone) {
  auto cal = make_cal();
  for (auto& l : cal->lifetime) {
    l = calib::Survival{};
    l.times_ns = {1'000'000'000};
    l.survival = {0.4};  // the curve ends at 0.4
  }
  CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(5));
  MultiCtx ctx(3);
  a.on_start(ctx);
  const int n = 20000;
  drive(a, ctx, n);
  const auto& k = a.counters();
  EXPECT_EQ(k.cancels_scheduled + k.no_cancel_draws, static_cast<std::uint64_t>(n));
  EXPECT_NEAR(static_cast<double>(k.no_cancel_draws) / n, 0.4, 0.02);
}

TEST(CalibratedFlow, CancelCarriesTheOwnerThatPlacedTheOrder) {
  auto cal = make_cal();
  for (auto& l : cal->lifetime) {
    l = calib::Survival{};
    l.times_ns = {1'000'000'000};
    l.survival = {0.0};  // every order is cancelled after 1 s
  }
  CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(5));
  MultiCtx ctx(3);
  a.on_start(ctx);
  const auto placed_by = drive(a, ctx, 60);  // checks each cancel's owner as it is sent
  EXPECT_EQ(a.counters().cancels_scheduled, 60u);
  EXPECT_GT(a.counters().cancels_sent, 10u);
  EXPECT_NE(placed_by.at(1), placed_by.at(2));
}

TEST(CalibratedFlow, MarketOrdersAreIocAndSizedFromTheirPmf) {
  auto cal = make_cal();
  cal->rates.limit = {calib::Rate{0, 0.0}, calib::Rate{0, 0.0}};
  cal->rates.aggressive = {calib::Rate{0, 0.0}, calib::Rate{0, 2.0}};
  cal->aggressive_size = pmf({7}, {1});
  CalibratedFlowAgent a(params(cal), Xoshiro256StarStar(1));
  MultiCtx ctx(3);
  a.on_start(ctx);
  ASSERT_EQ(ctx.pending.size(), 1u) << "a clock with rate 0 is never started";
  ctx.fire(a);
  const auto& o = ctx.last_new();
  EXPECT_EQ(o.type, lob::OrderType::Market);
  EXPECT_EQ(o.tif, lob::TIF::IOC);
  EXPECT_EQ(o.side, lob::Side::Sell);
  EXPECT_EQ(o.qty, 7);
  EXPECT_EQ(a.counters().markets_sent, 1u);
}

// ---- the kernel: several owner ids for one agent --------------------------------------------

namespace {

class OwnerProbe final : public Agent {
 public:
  using Step = std::pair<TimeNs, std::function<lob::Request(Context&)>>;
  explicit OwnerProbe(std::vector<Step> steps) : steps_(std::move(steps)) {}
  void on_start(Context& ctx) override {
    for (const auto& s : steps_) ctx.schedule_wakeup(s.first);
  }
  void on_wakeup(Context& ctx) override { ctx.send(steps_[next_++].second(ctx)); }
  void on_market_data(Context&, const MarketData&) override {}
  void on_private(Context&, const lob::Event& e) override { events.push_back(e); }
  std::vector<lob::Event> events;

 private:
  std::vector<Step> steps_;
  std::size_t next_{0};
};

struct RequestOwners final : Observer {
  std::vector<lob::Request> requests;
  void on_request(TimeNs, const lob::Request& r) override { requests.push_back(r); }
};

lob::OwnerId owner_of(const lob::Request& r) {
  return std::visit([](const auto& x) { return x.owner; }, r);
}

}  // namespace

TEST(KernelOwners, AMarketOrderTradesAgainstTheAgentsOwnRestingOrderUnderAnotherOwnerId) {
  Simulator s(simtest::empty_config(1.0));
  // Agent 0: an ordinary one (owner 1). Agent 1: three owner ids.
  auto plain = std::make_unique<OwnerProbe>(std::vector<OwnerProbe::Step>{
      {50, [](Context&) -> lob::Request {
         // It asks for another agent's extra owner id: the kernel must replace it with its own.
         return lob::NewOrder{kExtraOwnerBase, 1, lob::Side::Buy, lob::OrderType::Limit, lob::TIF::GTC, 90, 1, false,
                              lob::STP::CancelTaking};
       }}});
  auto multi = std::make_unique<OwnerProbe>(std::vector<OwnerProbe::Step>{
      {100, [](Context& c) -> lob::Request {
         return lob::NewOrder{c.owner_id(1), 1, lob::Side::Sell, lob::OrderType::Limit, lob::TIF::GTC, 100, 5, false,
                              lob::STP::CancelTaking};
       }},
      {200, [](Context& c) -> lob::Request {
         return lob::NewOrder{c.owner_id(2), 2, lob::Side::Buy, lob::OrderType::Market, lob::TIF::IOC, 0, 3, false,
                              lob::STP::CancelTaking};
       }},
      {300, [](Context& c) -> lob::Request { return lob::Cancel{c.owner_id(1), 2}; }},  // the sell has id 2
      {400, [](Context&) -> lob::Request {
         return lob::NewOrder{12345, 3, lob::Side::Sell, lob::OrderType::Limit, lob::TIF::GTC, 105, 1, false,
                              lob::STP::CancelTaking};  // not its id: becomes its own
       }}});
  OwnerProbe* mp = multi.get();
  OwnerProbe* pp = plain.get();
  const LatencySpec zero{0, 0, 0, 0};
  const AgentIndex i0 = s.add_agent(std::move(plain), zero, StreamRole::Test, 0);
  const AgentIndex i1 = s.add_agent(std::move(multi), zero, StreamRole::Test, 1);
  s.set_owner_count(i1, 3);
  RequestOwners obs;
  s.add_observer(&obs);
  s.run();

  // The ordinary agent keeps owner 1 whatever it asks for.
  EXPECT_EQ(i0, 0u);
  ASSERT_EQ(obs.requests.size(), 5u);
  EXPECT_EQ(owner_of(obs.requests[0]), 1u);
  // The multi-owner agent's requests carry the ids it chose; the foreign one became owner 2.
  EXPECT_EQ(owner_of(obs.requests[1]), kExtraOwnerBase);
  EXPECT_EQ(owner_of(obs.requests[2]), kExtraOwnerBase + 1);
  EXPECT_EQ(owner_of(obs.requests[3]), kExtraOwnerBase);  // the cancel by the order's owner
  EXPECT_EQ(owner_of(obs.requests[4]), 2u);

  int trades = 0, cancelled = 0;
  for (const lob::Event& e : mp->events) {
    if (const auto* t = std::get_if<lob::Trade>(&e)) {
      ++trades;
      EXPECT_EQ(t->maker_owner, kExtraOwnerBase);
      EXPECT_EQ(t->taker_owner, kExtraOwnerBase + 1);
      EXPECT_EQ(t->qty, 3);
    } else if (const auto* c = std::get_if<lob::Cancelled>(&e)) {
      if (c->reason == lob::CancelReason::User) {
        ++cancelled;
        EXPECT_EQ(c->owner, kExtraOwnerBase);
        EXPECT_EQ(c->qty_cancelled, 2);
      }
    }
  }
  EXPECT_EQ(trades, 1) << "one trade between two owner ids of one agent is delivered once";
  EXPECT_EQ(cancelled, 1);
  for (const lob::Event& e : pp->events) EXPECT_FALSE(std::holds_alternative<lob::Trade>(e));
  EXPECT_EQ(s.exchange().trades(), 1u);
}

TEST(KernelOwners, ACancelByAnotherOfTheAgentsOwnersIsRefused) {
  Simulator s(simtest::empty_config(1.0));
  auto multi = std::make_unique<OwnerProbe>(std::vector<OwnerProbe::Step>{
      {100, [](Context& c) -> lob::Request {
         return lob::NewOrder{c.owner_id(1), 1, lob::Side::Sell, lob::OrderType::Limit, lob::TIF::GTC, 100, 5, false,
                              lob::STP::CancelTaking};
       }},
      {200, [](Context& c) -> lob::Request { return lob::Cancel{c.owner_id(2), 1}; }}});
  OwnerProbe* mp = multi.get();
  const AgentIndex i = s.add_agent(std::move(multi), LatencySpec{0, 0, 0, 0}, StreamRole::Test, 0);
  s.set_owner_count(i, 3);
  s.run();
  bool rejected = false;
  for (const lob::Event& e : mp->events)
    if (const auto* r = std::get_if<lob::Reject>(&e)) rejected = r->reason == lob::RejectReason::NotOwner;
  EXPECT_TRUE(rejected);
  EXPECT_EQ(s.exchange().book().resting_orders(), 1u);
}

TEST(KernelOwners, SetOwnerCountRules) {
  Simulator s(simtest::empty_config(1.0));
  const AgentIndex i = s.add_agent(std::make_unique<OwnerProbe>(std::vector<OwnerProbe::Step>{}), LatencySpec{}, StreamRole::Test, 0);
  EXPECT_THROW(s.set_owner_count(i + 1, 2), std::logic_error);
  EXPECT_THROW(s.set_owner_count(i, 0), std::logic_error);
  s.set_owner_count(i, 4);
  EXPECT_THROW(s.set_owner_count(i, 4), std::logic_error);
}

// ---- the config -----------------------------------------------------------------------------

namespace {

json run_json(const std::string& file = "flow_small.json") {
  json j = json::parse(std::ifstream(kFixtures + "flow_small_run.json"));
  j["background"]["file"] = file;
  return j;
}

void expect_config_error(const json& j, const std::string& what, const std::string& base = kFixtures) {
  try {
    (void)parse_config(j, base);
    ADD_FAILURE() << "no error for " << what;
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find(what), std::string::npos) << e.what();
  }
}

}  // namespace

TEST(CalibratedConfig, DefaultsAndRelativeFileAreResolvedAgainstTheConfigDirectory) {
  json j = run_json();
  j["background"].erase("owner_count");
  j["background"].erase("latency_us");
  const SimConfig c = parse_config(j, kFixtures);
  ASSERT_TRUE(c.background);
  EXPECT_EQ(c.background->owner_count, 50u);
  EXPECT_EQ(c.background->latency, LatencySpec{});
  EXPECT_FALSE(c.background->start_price.has_value());
  EXPECT_EQ(c.background->file, kFixtures + "flow_small.json");
  ASSERT_TRUE(c.background->calibration);
  EXPECT_EQ(c.background->calibration->median_mid_ticks, 1000);
  EXPECT_EQ(c.background->calibration->source, "hand-made");
}

TEST(CalibratedConfig, LoadConfigResolvesFromAnyWorkingDirectory) {
  const auto saved = std::filesystem::current_path();
  std::filesystem::current_path(std::filesystem::temp_directory_path());
  SimConfig c;
  try {
    c = load_config(kFixtures + "flow_small_run.json");
  } catch (...) {
    std::filesystem::current_path(saved);
    throw;
  }
  std::filesystem::current_path(saved);
  ASSERT_TRUE(c.background);
  EXPECT_EQ(c.background->owner_count, 50u);
  EXPECT_EQ(c.background->latency.entry, 100'000);
}

TEST(CalibratedConfig, AbsoluteFileAndStartPrice) {
  json j = run_json(kFixtures + "flow_small.json");
  j["background"]["start_price"] = 777;
  j["background"]["owner_count"] = 3;
  const SimConfig c = parse_config(j, "/nonexistent");
  EXPECT_EQ(c.background->start_price, 777);
  EXPECT_EQ(c.background->owner_count, 3u);
}

TEST(CalibratedConfig, Errors) {
  {
    json j = run_json();
    j["background"]["bogus"] = 1;
    expect_config_error(j, "background.bogus");
  }
  {
    json j = run_json();
    j["background"]["model"] = "noise";
    expect_config_error(j, "background.model");
  }
  {
    json j = run_json();
    j["background"].erase("file");
    expect_config_error(j, "background.file");
  }
  {
    json j = run_json("no_such_file.json");
    expect_config_error(j, "no_such_file.json");
  }
  {
    json j = run_json();
    j["background"]["owner_count"] = 0;
    expect_config_error(j, "background.owner_count");
  }
  {
    json j = run_json();
    j["background"]["start_price"] = 0;
    expect_config_error(j, "background.start_price");
  }
  {
    json j = run_json();
    j["background"]["latency_us"].erase("jitter");
    expect_config_error(j, "background.latency_us.jitter");
  }
  {
    json j = run_json();
    j["background"]["latency_us"]["entry"] = -1;
    expect_config_error(j, "background.latency_us.entry");
  }
}

TEST(CalibratedConfig, EmptyPmfsTheAgentNeedsAreAClearError) {
  expect_config_error(run_json("flow_empty_limit_size.json"), "limit_size is empty");
  expect_config_error(run_json("flow_empty_placement_side.json"), "placement[sell]: all three spread buckets are empty");
}

// One empty bucket (flow_empty_placement.json: sell, spread 1) loads, and the agent draws from the nearest
// non-empty bucket of that side (bucket "2" is empty: it draws from "1").
TEST(CalibratedConfig, OneEmptyPlacementBucketFallsBackToTheNearestNonEmptyOne) {
  const SimConfig c = parse_config(run_json("flow_empty_placement.json"), kFixtures);
  ASSERT_TRUE(c.background.has_value());
  const calib::Calibration& cal = *c.background->calibration;
  ASSERT_TRUE(cal.placement[calib::kSell][1].empty());
  Simulator s(c);
  s.run();
  const auto& agent = dynamic_cast<const CalibratedFlowAgent&>(s.agent(*s.background_index()));
  EXPECT_GT(agent.counters().limits_sent, 0u);
  EXPECT_GT(agent.counters().placement_bucket_fallback, 0u);
}

TEST(CalibratedConfig, AbsentSectionLeavesTheConfigUntouched) {
  const SimConfig c = simtest::default_config();
  EXPECT_FALSE(c.background.has_value());
  EXPECT_FALSE(to_json(c).contains("background"));
  EXPECT_EQ(to_json(parse_config(to_json(c))), to_json(c));
}

TEST(CalibratedConfig, EffectiveConfigShowsTheSection) {
  const SimConfig c = parse_config(run_json(), kFixtures);
  const json j = to_json(c);
  ASSERT_TRUE(j.contains("background"));
  EXPECT_EQ(j["background"]["model"], "calibrated");
  EXPECT_EQ(j["background"]["owner_count"], 50);
  EXPECT_FALSE(j["background"].contains("start_price"));
}

// ---- in a whole run -------------------------------------------------------------------------

TEST(CalibratedRun, DeterministicAndAtTheConfiguredRates) {
  SimConfig cfg = parse_config(run_json(), kFixtures);
  cfg.duration_s = 20;
  finalize(cfg);
  const auto r1 = simtest::run(cfg);
  const auto r2 = simtest::run(cfg);
  EXPECT_EQ(r1.hash, r2.hash);
  EXPECT_EQ(r1.log, r2.log);

  Simulator s(cfg);
  ASSERT_TRUE(s.background_index().has_value());
  EXPECT_EQ(*s.background_index(), 0u);
  s.run();
  const auto& a = dynamic_cast<const CalibratedFlowAgent&>(s.agent(*s.background_index()));
  const auto& k = a.counters();
  EXPECT_NEAR(static_cast<double>(k.limits_sent), 8.0 * 20, 0.3 * 8.0 * 20);
  EXPECT_NEAR(static_cast<double>(k.markets_sent), 1.0 * 20, 0.6 * 20);
  EXPECT_GT(k.cancels_scheduled, 0u);
  EXPECT_GT(k.no_cancel_draws, 0u);
  EXPECT_GT(s.exchange().book().resting_orders(), 0u);
}
