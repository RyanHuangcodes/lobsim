// lobsim_fingerprint: the market fingerprint of one LOBSTER message/orderbook pair.
//   lobsim_fingerprint --messages F --orderbook F [--tick-units N] [--from-s S] [--to-s S]
//                      [--bootstrap] [--resamples N] [--block-s N] [--seed N] [--halves] [--out FILE]
// Prints a JSON document: the fingerprint, and with --bootstrap the 95% interval of every scalar,
// and with --halves the distance between the two halves of the window with its bootstrap spread.
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "calib/fingerprint.hpp"

namespace {
int usage(const std::string& why) {
  std::fprintf(stderr, "lobsim_fingerprint: %s\nusage: lobsim_fingerprint --messages F --orderbook F [--tick-units N] "
                       "[--from-s S] [--to-s S] [--bootstrap] [--resamples N] [--block-s N] [--seed N] [--halves] [--out FILE]\n",
               why.c_str());
  return 1;
}
bool to_int(const std::string& v, std::int64_t& out) {
  const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
  return !v.empty() && ec == std::errc{} && p == v.data() + v.size();
}
}  // namespace

int main(int argc, char** argv) {
  std::string mp, bp, out_path;
  std::int64_t tick = 1;
  calib::FingerprintWindow w;
  calib::BootstrapOptions bo;
  bool boot = false, halves = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--bootstrap") { boot = true; continue; }
    if (a == "--halves") { halves = true; continue; }
    if (i + 1 >= argc) return usage(a + " needs a value");
    const std::string v = argv[++i];
    std::int64_t n = 0;
    if (a == "--messages") mp = v;
    else if (a == "--orderbook") bp = v;
    else if (a == "--out") out_path = v;
    else if (!to_int(v, n) || n < 0) return usage(a + " needs a non-negative integer");
    else if (a == "--tick-units") tick = n;
    else if (a == "--from-s") w.from_ns = n * 1'000'000'000;
    else if (a == "--to-s") w.to_ns = n * 1'000'000'000;
    else if (a == "--resamples") bo.resamples = static_cast<int>(n);
    else if (a == "--block-s") bo.block_seconds = static_cast<int>(n);
    else if (a == "--seed") bo.seed = static_cast<std::uint64_t>(n);
    else return usage("unknown argument " + a);
  }
  if (mp.empty() || bp.empty() || tick < 1) return usage("--messages, --orderbook and --tick-units >= 1 are required");
  try {
    const calib::Prepared p = calib::Prepared::from_files(mp, bp, tick, w);
    nlohmann::json j;
    j["fingerprint"] = calib::to_json(p.fingerprint());
    if (boot) j["bootstrap"] = calib::to_json(p.bootstrap(bo));
    if (halves) j["halves"] = calib::to_json(p.halves(bo));
    const std::string text = j.dump(2) + "\n";
    if (out_path.empty()) {
      std::cout << text;
    } else {
      std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
      f << text;
      if (!f) return usage("cannot write " + out_path);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_fingerprint: %s\n", e.what());
    return 1;
  }
  return 0;
}
