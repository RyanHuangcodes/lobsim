// libFuzzer entry point: the input is a text of LOBSTER message and orderbook lines.
#include <cstddef>
#include <cstdint>

#include "lobster_parse_driver.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lob::fuzz::run_lobster_parse(data, size);
  return 0;
}
