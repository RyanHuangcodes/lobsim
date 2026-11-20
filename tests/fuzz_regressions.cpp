// Regression tests (upgrade 1, 3.4, 7): every regular file of fuzz/regressions/<kind>/ and then of
// fuzz/corpus/<kind>/ is run through run_diff (kind diff) or run_replay_parse (kind replay_parse).
// Both abort on a failure, so a failing file ends the process; the path is printed to stderr
// first so ctest's output names it.
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
#include "replay_parse_driver.hpp"

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

// The two suites below are filtered by name in ctest, and a gtest filter that matches nothing
// exits 0. These checks fail if the committed seeds are not found (a wrong source directory,
// a renamed suite), so the regression tests can never pass by running nothing.
bool has_file(const std::vector<Case>& cases, const std::string& file_name) {
  return std::any_of(cases.begin(), cases.end(),
                     [&](const Case& c) { return fs::path(c.path).filename().string() == file_name; });
}

TEST(FuzzCorpus, CommittedSeedsAreFound) {
  const std::vector<Case> diff = collect_cases("diff");
  for (const char* name :
       {"worked_example", "trade_at_maker_price", "fifo_one_level", "ioc_remainder", "market_empty_side",
        "fok_exact_fill", "fok_one_lot_short", "post_only_reject", "modify_reduce_keeps_priority",
        "modify_increase_loses_priority", "modify_to_crossing_price", "cancel_unknown_id", "cancel_wrong_owner",
        "stp_cancel_taking", "stp_cancel_resting", "fok_stp_cancel_taking", "fok_stp_cancel_resting",
        "every_reject_reason", "mixed_stream_64"}) {
    EXPECT_TRUE(has_file(diff, std::string(name) + ".bin")) << "missing diff seed " << name << ".bin";
  }
  const std::vector<Case> replay = collect_cases("replay_parse");
  for (const char* name : {"empty.log", "run_seed1.log", "run_seed2.log", "run_seed3.log"}) {
    EXPECT_TRUE(has_file(replay, name)) << "missing replay_parse seed " << name;
  }
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

class ReplayParseRegression : public ::testing::TestWithParam<Case> {};

TEST_P(ReplayParseRegression, ParsesOrRejectsCleanlyAndRoundTrips) {
  const Case& c = GetParam();
  std::fprintf(stderr, "run_replay_parse: %s\n", c.path.c_str());
  std::fflush(stderr);
  const std::vector<std::uint8_t> bytes = read_bytes(c.path);
  lob::fuzz::run_replay_parse(bytes.data(), bytes.size());
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(ReplayParseRegression);

INSTANTIATE_TEST_SUITE_P(ReplayParse, ReplayParseRegression, ::testing::ValuesIn(collect_cases("replay_parse")),
                         [](const ::testing::TestParamInfo<Case>& info) { return info.param.name; });

}  // namespace
