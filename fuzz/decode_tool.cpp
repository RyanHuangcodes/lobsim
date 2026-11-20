// lobsim_fuzz_decode <file>: prints every request of a fuzz input and the reference book's
// events for it, then runs the differential check on the same bytes.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "diff_driver.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: lobsim_fuzz_decode <file>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "lobsim_fuzz_decode: cannot read %s\n", argv[1]);
    return 2;
  }
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    std::fprintf(stderr, "lobsim_fuzz_decode: error reading %s\n", argv[1]);
    return 2;
  }

  std::vector<lob::EventBuffer> events;
  const std::vector<lob::Request> reqs = lob::fuzz::decode(bytes.data(), bytes.size(), &events);
  for (std::size_t i = 0; i < reqs.size(); ++i) {
    std::printf("#%zu %s\n", i, lob::fuzz::to_string(reqs[i]).c_str());
    for (const lob::Event& ev : events[i]) std::printf("    %s\n", lob::to_string(ev).c_str());
  }
  std::fflush(stdout);

  lob::fuzz::run_diff(bytes.data(), bytes.size());
  std::printf("books agree: %zu requests\n", reqs.size());
  return 0;
}
