// lobsim_replay: feed an orders.log into a fresh OrderBook and print the engine event hash.
//
//   lobsim_replay --log <path>
//
// The hash must equal engine_event_hash in the summary.json of the run that wrote the log.
#include <cinttypes>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>

#include "sim/replay.hpp"

int main(int argc, char** argv) {
  if (argc != 3 || std::string(argv[1]) != "--log") {
    std::fprintf(stderr, "usage: lobsim_replay --log <path>\n");
    return 2;
  }
  std::ifstream in(argv[2], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "lobsim_replay: cannot read %s\n", argv[2]);
    return 1;
  }
  try {
    const sim::ReplayResult r = sim::replay_log(in);
    std::printf("requests             %" PRIu64 "\n", r.requests);
    std::printf("engine events        %" PRIu64 "\n", r.events);
    std::printf("trades               %" PRIu64 "\n", r.trades);
    std::printf("engine event hash    %016" PRIx64 "\n", r.event_hash);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_replay: %s: %s\n", argv[2], e.what());
    return 1;
  }
  return 0;
}
