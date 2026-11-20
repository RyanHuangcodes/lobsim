// libFuzzer entry point: the shared differential driver, OrderBook against ReferenceBook.
#include <cstddef>
#include <cstdint>

#include "diff_driver.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lob::fuzz::run_diff(data, size);
  return 0;
}
