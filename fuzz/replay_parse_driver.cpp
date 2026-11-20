#include "replay_parse_driver.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "sim/order_log.hpp"

namespace lob::fuzz {

namespace {

[[noreturn]] void fail(const char* what, const std::string& detail = {}) {
  std::fprintf(stderr, "lobsim fuzz: replay parse round trip failed: %s\n", what);
  if (!detail.empty()) std::fprintf(stderr, "  %s\n", detail.c_str());
  std::fflush(stderr);
  std::abort();
}

std::string describe(const sim::LoggedRequest& r) { return sim::format_request(r.time, r.request); }

[[noreturn]] void report_difference(const sim::OrderLog& a, const sim::OrderLog& b) {
  std::fprintf(stderr, "lobsim fuzz: replay parse round trip failed: the second parse differs\n");
  std::fprintf(stderr, "  max_price %lld -> %lld, requests %zu -> %zu\n", static_cast<long long>(a.max_price),
               static_cast<long long>(b.max_price), a.requests.size(), b.requests.size());
  for (std::size_t i = 0; i < a.requests.size() && i < b.requests.size(); ++i) {
    if (!(a.requests[i] == b.requests[i])) {
      std::fprintf(stderr, "  first difference at request %zu\n    first parse:  %s\n    second parse: %s\n", i,
                   describe(a.requests[i]).c_str(), describe(b.requests[i]).c_str());
      break;
    }
  }
  std::fflush(stderr);
  std::abort();
}

}  // namespace

void run_replay_parse(const std::uint8_t* data, std::size_t size) {
  const std::string_view contents(reinterpret_cast<const char*>(data), size);

  sim::OrderLog log;
  try {
    log = sim::parse_order_log(contents);
  } catch (const sim::ReplayError&) {
    return;  // a clean rejection
  }

  const std::string text = sim::format_order_log(log);
  sim::OrderLog again;
  try {
    again = sim::parse_order_log(text);
  } catch (const sim::ReplayError& e) {
    fail("the formatted log is rejected", std::string("input ") + std::to_string(size) + " bytes, formatted " +
                                             std::to_string(text.size()) + " bytes: " + e.what());
  }
  if (!(again == log)) report_difference(log, again);
  if (sim::format_order_log(again) != text) fail("formatting the second parse gives different text");
}

}  // namespace lob::fuzz
