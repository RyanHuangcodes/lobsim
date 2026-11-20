// lobsim_fuzz_decode [--builder] <file>: prints every request of a fuzz input and the reference
// book's events for it, then runs the differential check on the same bytes. With --builder the
// input is a fuzz_diff_builder input (kind = b0 % 7) and the builder operations are decoded too.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "diff_driver.hpp"

int main(int argc, char** argv) {
  const bool builder = argc == 3 && std::strcmp(argv[1], "--builder") == 0;
  if (argc != 2 && !builder) {
    std::fprintf(stderr, "usage: lobsim_fuzz_decode [--builder] <file>\n");
    return 2;
  }
  const char* path = argv[argc - 1];
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "lobsim_fuzz_decode: cannot read %s\n", path);
    return 2;
  }
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    std::fprintf(stderr, "lobsim_fuzz_decode: error reading %s\n", path);
    return 2;
  }

  std::size_t count = 0;
  if (builder) {
    std::vector<std::vector<std::string>> outcomes;
    const std::vector<lob::fuzz::BuilderOp> ops = lob::fuzz::decode_builder(bytes.data(), bytes.size(), &outcomes);
    for (std::size_t i = 0; i < ops.size(); ++i) {
      std::printf("#%zu %s\n", i, lob::fuzz::op_to_string(ops[i]).c_str());
      for (const std::string& line : outcomes[i]) std::printf("    %s\n", line.c_str());
    }
    count = ops.size();
    std::fflush(stdout);
    lob::fuzz::run_diff_builder(bytes.data(), bytes.size());
  } else {
    std::vector<lob::EventBuffer> events;
    const std::vector<lob::Request> reqs = lob::fuzz::decode(bytes.data(), bytes.size(), &events);
    for (std::size_t i = 0; i < reqs.size(); ++i) {
      std::printf("#%zu %s\n", i, lob::fuzz::to_string(reqs[i]).c_str());
      for (const lob::Event& ev : events[i]) std::printf("    %s\n", lob::to_string(ev).c_str());
    }
    count = reqs.size();
    std::fflush(stdout);
    lob::fuzz::run_diff(bytes.data(), bytes.size());
  }
  std::printf("books agree: %zu %s\n", count, builder ? "operations" : "requests");
  return 0;
}
