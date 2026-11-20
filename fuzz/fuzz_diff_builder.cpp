// libFuzzer entry point: the book-builder operations, OrderBook against ReferenceBook.
#include <cstddef>
#include <cstdint>

#include "diff_driver.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  lob::fuzz::run_diff_builder(data, size);
  return 0;
}
