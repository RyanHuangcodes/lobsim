// libFuzzer entry point: the input is the contents of an orders.log.
#include <cstddef>
#include <cstdint>

#include "replay_parse_driver.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lob::fuzz::run_replay_parse(data, size);
  return 0;
}
