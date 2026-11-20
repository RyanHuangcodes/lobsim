// Regression test (upgrade 1, 3.4): every regular file of fuzz/regressions/diff/ and then of
// fuzz/corpus/diff/ is run through run_diff. run_diff aborts on a mismatch, so a failing file
// ends the process; the path is printed to stderr first so ctest's output names it.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>
#include <string>
#include <vector>

#include "diff_driver.hpp"

namespace {

namespace fs = std::filesystem;

struct Case {
  std::string name;  // gtest parameter name
  std::string path;
};

// Regular files of `dir` (non-recursive), not starting with '.', sorted by name. A missing
// directory is empty.
std::vector<fs::path> list_files(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return files;
  for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
    const std::string name = e.path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    if (e.is_regular_file(ec)) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end(),
            [](const fs::path& a, const fs::path& b) { return a.filename().string() < b.filename().string(); });
  return files;
}

std::string sanitize(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) c = '_';
  }
  return out;
}

std::vector<Case> collect_cases(const char* subdir) {
  const fs::path root = fs::path(LOBSIM_SOURCE_DIR) / "fuzz";
  std::vector<Case> cases;
  std::set<std::string> used;
  for (const auto& [prefix, group] : {std::pair<const char*, const char*>{"regressions_", "regressions"},
                                      std::pair<const char*, const char*>{"corpus_", "corpus"}}) {
    for (const fs::path& f : list_files(root / group / subdir)) {
      const std::string base = prefix + sanitize(f.filename().string());
      std::string name = base;
      for (int n = 2; !used.insert(name).second; ++n) name = base + "_" + std::to_string(n);
      cases.push_back({name, f.string()});
    }
  }
  return cases;
}

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in) << "cannot read " << path;
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

class DiffRegression : public ::testing::TestWithParam<Case> {};

TEST_P(DiffRegression, BooksAgree) {
  const Case& c = GetParam();
  std::fprintf(stderr, "run_diff: %s\n", c.path.c_str());
  std::fflush(stderr);
  const std::vector<std::uint8_t> bytes = read_bytes(c.path);
  lob::fuzz::run_diff(bytes.data(), bytes.size());
}

// Both directories may be empty (only a .gitkeep).
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(DiffRegression);

INSTANTIATE_TEST_SUITE_P(Diff, DiffRegression, ::testing::ValuesIn(collect_cases("diff")),
                         [](const ::testing::TestParamInfo<Case>& info) { return info.param.name; });

}  // namespace
