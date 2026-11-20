// run_replay_parse (upgrade 1, F5): returns on valid logs and on clean rejections.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "replay_parse_driver.hpp"
#include "sim/order_log.hpp"

namespace {

void run(const std::string& text) {
  lob::fuzz::run_replay_parse(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

TEST(FuzzReplayParse, EverySeedReturns) {
  const std::filesystem::path dir = std::filesystem::path(LOBSIM_SOURCE_DIR) / "fuzz" / "corpus" / "replay_parse";
  int files = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (!e.is_regular_file() || e.path().filename().string()[0] == '.') continue;
    std::ifstream in(e.path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    run(text);
    // A format change must not turn the seeds into rejection-only inputs: the run_seed logs
    // parse, and only the empty file is rejected.
    if (e.path().filename() == "empty.log") {
      EXPECT_THROW(sim::parse_order_log(text), sim::ReplayError);
    } else {
      EXPECT_NO_THROW(sim::parse_order_log(text)) << e.path();
    }
    ++files;
  }
  EXPECT_GE(files, 4);
}

TEST(FuzzReplayParse, EmptyInputAndNullDataAreCleanRejections) {
  lob::fuzz::run_replay_parse(nullptr, 0);
  run("");
}

TEST(FuzzReplayParse, MalformedLogsAreCleanRejections) {
  run("not a log\n");
  run("# lobsim orders.log v1 max_price=\n");
  run("# lobsim orders.log v1 max_price=1000\n5 N 1 0 0 0 0 100 5 0\n");        // too few fields
  run("# lobsim orders.log v1 max_price=1000\n5 X 1 2\n");                       // unknown kind
  run("# lobsim orders.log v1 max_price=1000\n9 C 1 2\n5 C 1 2\n");             // time goes backwards
  run("# lobsim orders.log v1 max_price=1000\n5 C 1 99999999999999999999999\n");  // overflow
  std::string nul = "# lobsim orders.log v1 max_price=1000\n5 C 1 2";  // embedded NUL
  nul += '\0';
  nul += '\n';
  run(nul);
  run("# lobsim orders.log v1 max_price=1000\n5 C 1 2");                           // no final newline
}

TEST(FuzzReplayParse, LogWithEveryRequestKindRoundTrips) {
  lob::NewOrder n;
  n.owner = 3;
  n.client_order_id = 9;
  n.side = lob::Side::Sell;
  n.type = lob::OrderType::Limit;
  n.tif = lob::TIF::FOK;
  n.price = 10'005;
  n.qty = 7;
  n.post_only = false;
  n.stp = lob::STP::CancelResting;
  lob::NewOrder market;
  market.owner = 1;
  market.client_order_id = 10;
  market.type = lob::OrderType::Market;
  market.tif = lob::TIF::IOC;
  market.qty = 2;

  sim::OrderLog log;
  log.max_price = 1'000'000;
  log.requests = {{10, n}, {20, market}, {20, lob::Cancel{3, 1}}, {35, lob::Modify{3, 1, 10'006, 4}}};
  const std::string text = sim::format_order_log(log);
  EXPECT_EQ(sim::parse_order_log(text), log);
  run(text);
}

}  // namespace
