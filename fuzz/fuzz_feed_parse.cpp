// libFuzzer entry point: the input is the bytes of an ITCH feed file (upgrade 6).
#include <cstddef>
#include <cstdint>

#include "feed_parse_driver.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lob::fuzz::run_feed_parse(data, size);
  return 0;
}
