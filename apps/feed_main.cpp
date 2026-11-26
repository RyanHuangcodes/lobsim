// lobsim_feed: the binary market-data feed (upgrade 6, FEED.md).
//
//   lobsim_feed encode --run <sim output dir> --out <feed file>
//       orders.log -> engine -> ITCH 5.0 file
//   lobsim_feed decode --feed <feed file> [--depth N]
//       parse, rebuild the book from the feed alone, print the counts and the top N levels (default 5)
//   lobsim_feed check --run <sim output dir>
//       replay orders.log through the engine, encode every request in memory, parse it straight back
//       into a book built from the feed, and check after every request that the two books agree
//       (best prices and the top 10 levels per side), every 10,000 requests and at the end that their
//       digests agree, that the executions equal the engine's trades, and that re-encoding the parsed
//       messages gives the same bytes. Writes no file.
//   lobsim_feed packetize --feed <feed file> --out <packet file> [--session NAME] [--max-packet N]
//                         [--drop-packet K]
//       group the messages into MoldUDP64 packets of at most N bytes (default 1,400, header included),
//       sequence numbers from 1, then an end-of-session packet. --drop-packet leaves out the K-th
//       message packet, to simulate a loss.
//   lobsim_feed depacketize --packets <packet file> --out <feed file>
//       read the packets in sequence order, skip duplicates, report every gap, and write the feed
//       back out. Exit code 2 if a message is missing or the session never ended.
//
// Every subcommand prints one summary line, "feed: S <n> A <n> E <n> X <n> D <n> bytes <n> digest <hex>".
// Exit codes, as for the LOBSTER tools: 0 done (every check passed), 1 bad arguments or bad input
// (unreadable or malformed file, a value the feed cannot hold), 2 the first mismatch (named on stderr).
#include <algorithm>
#include <charconv>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "feed/decoder.hpp"
#include "feed/encoder.hpp"
#include "feed/feed_book.hpp"
#include "feed/itch.hpp"
#include "feed/moldudp64.hpp"
#include "lob/apply.hpp"
#include "lob/events.hpp"
#include "lob/order_book.hpp"
#include "sim/order_log.hpp"

namespace {

constexpr std::size_t kCompareLevels = 10;
constexpr std::uint64_t kDigestEvery = 10'000;

int usage(const std::string& why) {
  std::fprintf(stderr, "lobsim_feed: %s\n", why.c_str());
  std::fprintf(stderr,
               "usage: lobsim_feed encode --run <sim output dir> --out <feed file>\n"
               "       lobsim_feed decode --feed <feed file> [--depth N]\n"
               "       lobsim_feed check --run <sim output dir>\n"
               "       lobsim_feed packetize --feed <feed file> --out <packet file> [--session NAME]\n"
               "                             [--max-packet N] [--drop-packet K]\n"
               "       lobsim_feed depacketize --packets <packet file> --out <feed file>\n");
  return 1;
}

// A mismatch: exit code 2.
struct Mismatch {
  std::string what;
};

// Bad input: exit code 1.
struct BadInput {
  std::string what;
};

sim::OrderLog read_log(const std::string& run_dir) {
  const std::filesystem::path path = std::filesystem::path(run_dir) / "orders.log";
  std::ifstream in(path, std::ios::binary);
  if (!in) throw BadInput{"cannot read " + path.string()};
  std::ostringstream text;
  text << in.rdbuf();
  try {
    return sim::parse_order_log(text.str());
  } catch (const sim::ReplayError& e) {
    throw BadInput{path.string() + ": " + e.what()};
  }
}

std::vector<std::byte> read_file(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) throw BadInput{"cannot read " + path};
  std::vector<std::byte> bytes;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    bytes.resize(static_cast<std::size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
      std::fclose(f);
      throw BadInput{"cannot read " + path};
    }
  }
  std::fclose(f);
  return bytes;
}

std::string summary_line(const feed::ParseResult& r, std::uint64_t bytes, std::uint64_t digest) {
  char line[256];
  std::snprintf(line, sizeof line,
                "feed: S %" PRIu64 " A %" PRIu64 " E %" PRIu64 " X %" PRIu64 " D %" PRIu64 " bytes %" PRIu64
                " digest %016" PRIx64,
                r.by_type['S'], r.by_type['A'], r.by_type['E'], r.by_type['X'], r.by_type['D'], bytes, digest);
  return line;
}

std::string summary_line(const feed::Counts& c, std::uint64_t digest) {
  feed::ParseResult r;
  r.by_type['S'] = c.S;
  r.by_type['A'] = c.A;
  r.by_type['E'] = c.E;
  r.by_type['X'] = c.X;
  r.by_type['D'] = c.D;
  return summary_line(r, c.bytes, digest);
}

[[noreturn]] void rethrow_encode_error(const feed::EncodeError& e) {
  if (e.kind() == feed::EncodeError::Kind::Overflow) throw BadInput{std::string("encode: ") + e.what()};
  throw Mismatch{std::string("encode: ") + e.what()};
}

// ---------------------------------------------------------------- encode

int encode(const std::string& run_dir, const std::string& out_path) {
  const sim::OrderLog log = read_log(run_dir);
  std::FILE* out = std::fopen(out_path.c_str(), "wb");
  if (out == nullptr) throw BadInput{"cannot write " + out_path};
  lob::OrderBook engine(lob::BookConfig{log.max_price});
  feed::Encoder enc(out, log.requests.size());
  lob::EventBuffer events;
  try {
    enc.start(0);
    for (const sim::LoggedRequest& r : log.requests) {
      events.clear();
      lob::apply(engine, r.request, events);
      enc.on_request(r.time, r.request, events);
    }
    enc.finish(log.requests.empty() ? 0 : log.requests.back().time);
  } catch (const feed::EncodeError& e) {
    std::fclose(out);
    rethrow_encode_error(e);
  } catch (const std::runtime_error& e) {
    std::fclose(out);
    throw BadInput{e.what()};
  }
  if (std::fclose(out) != 0) throw BadInput{"error writing " + out_path};
  const feed::Counts& c = enc.counts();
  std::error_code ec;
  const auto log_bytes = std::filesystem::file_size(std::filesystem::path(run_dir) / "orders.log", ec);
  const double n = static_cast<double>(log.requests.size());
  std::printf("requests             %zu\n", log.requests.size());
  std::printf("feed bytes/request   %.2f (orders.log: %.2f)\n", n > 0 ? static_cast<double>(c.bytes) / n : 0.0,
              n > 0 && !ec ? static_cast<double>(log_bytes) / n : 0.0);
  std::printf("wrote                %s\n", out_path.c_str());
  std::printf("%s\n", summary_line(c, feed::book_digest(engine)).c_str());
  return 0;
}

// ---------------------------------------------------------------- decode

std::string level_text(const std::optional<lob::LevelView>& l) {
  if (!l) return "-";
  return std::to_string(l->total_qty) + " @ " + std::to_string(l->price);
}

int decode(const std::string& path, std::size_t depth) {
  const std::vector<std::byte> bytes = read_file(path);
  feed::FeedBook fb;
  const feed::ParseResult r = feed::parse(bytes, fb);
  if (!r.ok() && r.error != feed::ParseError::Stopped) {
    throw BadInput{path + ": " + feed::to_string(r.error) + " at byte offset " + std::to_string(r.offset)};
  }
  if (!r.ok() || !fb.finish()) throw Mismatch{path + ": " + fb.error()};
  std::printf("messages             %" PRIu64 " (%" PRIu64 " of types this feed does not use, skipped)\n", r.messages,
              r.unknown);
  std::printf("resting orders       %zu\n", fb.book().resting_orders());
  const std::vector<lob::LevelView> bids = fb.book().depth(lob::Side::Buy, depth);
  const std::vector<lob::LevelView> asks = fb.book().depth(lob::Side::Sell, depth);
  std::printf("%-5s %-20s %-20s\n", "level", "bid (lots @ ticks)", "ask (lots @ ticks)");
  for (std::size_t i = 0; i < depth && (i < bids.size() || i < asks.size()); ++i) {
    const auto b = i < bids.size() ? std::optional(bids[i]) : std::nullopt;
    const auto a = i < asks.size() ? std::optional(asks[i]) : std::nullopt;
    std::printf("%-5zu %-20s %-20s\n", i + 1, level_text(b).c_str(), level_text(a).c_str());
  }
  std::printf("%s\n", summary_line(r, bytes.size(), feed::book_digest(fb.book())).c_str());
  return 0;
}

// ---------------------------------------------------------------- check

// The handler of the check: applies each message to the FeedBook, re-encodes it, and records the
// executions for the trade tape.
struct CheckHandler {
  feed::FeedBook& book;
  feed::Reencoder re;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> executions;  // (match number, shares)

  template <class Msg>
  bool on(const Msg& m) {
    re.on(m);
    if constexpr (std::is_same_v<Msg, feed::MsgE>) executions.emplace_back(m.match, m.shares);
    return book.on(m);
  }
  bool on_unknown(std::uint8_t type, std::span<const std::byte> body) {
    re.on_unknown(type, body);
    return book.on_unknown(type, body);
  }
};

std::string levels_text(const std::vector<lob::LevelView>& v) {
  std::string s;
  for (const lob::LevelView& l : v) s += (s.empty() ? "" : " ") + std::to_string(l.total_qty) + "@" + std::to_string(l.price);
  return s.empty() ? "(empty)" : s;
}

std::string price_text(const std::optional<lob::Price>& p) { return p ? std::to_string(*p) : "none"; }

// Parses one request's bytes into the handler and checks the round trip. `where` names the request.
void feed_chunk(CheckHandler& h, std::span<const std::byte> chunk, const std::string& where) {
  h.re.out.clear();
  h.executions.clear();
  const feed::ParseResult r = feed::parse(chunk, h);
  if (!r.ok()) {
    if (r.error == feed::ParseError::Stopped) throw Mismatch{where + ": the book rebuilt from the feed failed: " + h.book.error()};
    throw Mismatch{where + ": the encoder wrote bytes that do not parse: " + feed::to_string(r.error)};
  }
  if (!std::equal(h.re.out.begin(), h.re.out.end(), chunk.begin(), chunk.end())) {
    throw Mismatch{where + ": re-encoding the parsed messages does not give the same bytes"};
  }
}

int check(const std::string& run_dir) {
  const sim::OrderLog log = read_log(run_dir);
  const std::size_t n = log.requests.size();
  lob::OrderBook engine(lob::BookConfig{log.max_price});
  feed::Encoder enc(nullptr, n);
  feed::FeedBook fb(n);
  CheckHandler h{fb, {}, {}};
  h.re.out.reserve(1 << 12);
  lob::EventBuffer events;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> trades;  // the engine's (trade_id, qty)
  std::vector<lob::LevelView> ours, theirs;  // the top levels of both books, reused for every request
  std::uint64_t trade_count = 0, trade_shares = 0, digests = 0;

  try {
    enc.start(0);
    feed_chunk(h, enc.buffered(), "the start of the feed");
    for (std::size_t k = 0; k < n; ++k) {
      const sim::LoggedRequest& r = log.requests[k];
      const std::string where_k = "request " + std::to_string(k + 1);
      events.clear();
      lob::apply(engine, r.request, events);
      trades.clear();
      for (const lob::Event& ev : events) {
        if (const auto* t = std::get_if<lob::Trade>(&ev)) {
          trades.emplace_back(t->trade_id, static_cast<std::uint64_t>(t->qty));
          ++trade_count;
          trade_shares += static_cast<std::uint64_t>(t->qty);
        }
      }
      enc.clear();
      enc.on_request(r.time, r.request, events);
      feed_chunk(h, enc.buffered(), where_k);

      // 3. The trade tape: one E per trade, in order, with match number = trade_id and the same size.
      if (h.executions != trades) {
        throw Mismatch{where_k + ": the feed carries " + std::to_string(h.executions.size()) +
                       " executions, the engine made " + std::to_string(trades.size()) + " trades (or their ids or sizes differ)"};
      }
      // 1. Best prices and the top 10 levels of both sides.
      if (fb.book().best_bid() != engine.best_bid() || fb.book().best_ask() != engine.best_ask()) {
        throw Mismatch{where_k + ": best bid/ask from the feed " + price_text(fb.book().best_bid()) + "/" +
                       price_text(fb.book().best_ask()) + ", engine " + price_text(engine.best_bid()) + "/" +
                       price_text(engine.best_ask())};
      }
      for (const lob::Side side : {lob::Side::Buy, lob::Side::Sell}) {
        fb.book().depth(side, kCompareLevels, ours);
        engine.depth(side, kCompareLevels, theirs);
        if (ours != theirs) {
          throw Mismatch{where_k + ": the " + (side == lob::Side::Buy ? "bid" : "ask") + " side differs: feed " +
                         levels_text(ours) + ", engine " + levels_text(theirs)};
        }
      }
      // 2. The full book, every 10,000 requests.
      if ((k + 1) % kDigestEvery == 0) {
        ++digests;
        if (feed::book_digest(fb.book()) != feed::book_digest(engine)) {
          throw Mismatch{where_k + ": the full book digest differs"};
        }
      }
    }
    enc.clear();
    enc.finish(n == 0 ? 0 : log.requests.back().time);
    feed_chunk(h, enc.buffered(), "the end of the feed");
  } catch (const feed::EncodeError& e) {
    rethrow_encode_error(e);
  }
  if (!fb.finish()) throw Mismatch{"the end of the feed: " + fb.error()};
  const std::uint64_t digest = feed::book_digest(engine);
  ++digests;
  if (feed::book_digest(fb.book()) != digest) throw Mismatch{"the end of the feed: the full book digest differs"};
  const feed::Counts& c = enc.counts();
  if (c.E != trade_count || fb.executed_shares() != trade_shares) {
    throw Mismatch{"the trade tape: " + std::to_string(c.E) + " executions for " + std::to_string(fb.executed_shares()) +
                   " shares, the engine made " + std::to_string(trade_count) + " trades for " + std::to_string(trade_shares)};
  }
  std::printf("requests             %zu, every one checked (best prices and top %zu levels per side)\n", n, kCompareLevels);
  std::printf("full book digests    %" PRIu64 " compared, all equal\n", digests);
  std::printf("trade tape           %" PRIu64 " executions, %" PRIu64 " shares, match numbers equal the trade ids\n",
              trade_count, trade_shares);
  std::printf("round trip           parse then re-encode is byte-identical (%" PRIu64 " bytes)\n", c.bytes);
  std::printf("%s\n", summary_line(c, digest).c_str());
  return 0;
}

// ---------------------------------------------------------------- MoldUDP64 packets (F7)

void write_file(const std::string& path, std::span<const std::byte> bytes) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) throw BadInput{"cannot write " + path};
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  if (std::fclose(f) != 0 || !ok) throw BadInput{"error writing " + path};
}

int packetize(const std::string& feed_path, const std::string& out_path, const feed::mold::PacketizeOptions& opt) {
  const std::vector<std::byte> feed_bytes = read_file(feed_path);
  feed::mold::PacketizeStats st;
  std::vector<std::byte> packets;
  try {
    packets = feed::mold::packetize(feed_bytes, opt, &st);
  } catch (const feed::mold::PacketizeError& e) {
    throw BadInput{feed_path + ": " + e.what()};
  }
  write_file(out_path, packets);
  const double per = st.packets > 0 ? static_cast<double>(st.messages - st.dropped_messages) / static_cast<double>(st.packets) : 0.0;
  std::printf("messages             %" PRIu64 " in %" PRIu64 " packets (%.1f per packet, at most %zu bytes each)\n",
              st.messages - st.dropped_messages, st.packets, per, opt.max_packet);
  if (opt.drop_packet != 0) {
    std::printf("dropped              packet %" PRIu64 ": %" PRIu64 " messages (a simulated loss)\n", opt.drop_packet,
                st.dropped_messages);
  }
  std::printf("bytes                %" PRIu64 " (feed %zu; packet overhead %.2f%%)\n", st.bytes, feed_bytes.size(),
              feed_bytes.empty() ? 0.0 : 100.0 * (static_cast<double>(st.bytes) / static_cast<double>(feed_bytes.size()) - 1.0));
  std::printf("session              %s, end-of-session packet at sequence %" PRIu64 "\n",
              feed::mold::to_string(opt.session).c_str(), opt.first_sequence + st.messages);
  std::printf("wrote                %s\n", out_path.c_str());
  return 0;
}

int depacketize(const std::string& packets_path, const std::string& out_path) {
  const std::vector<std::byte> file = read_file(packets_path);
  feed::mold::Reassembler re;
  const feed::mold::ReadResult r = feed::mold::read_packets(file, re);
  if (!r.ok()) {
    throw BadInput{packets_path + ": " + feed::mold::to_string(r.error) + " at byte offset " + std::to_string(r.offset)};
  }
  write_file(out_path, re.feed);
  std::printf("session              %s\n", feed::mold::to_string(r.session).c_str());
  std::printf("packets              %" PRIu64 " (%" PRIu64 " heartbeats)%s\n", r.packets, r.heartbeats,
              r.ended ? ", ended by an end-of-session packet" : ", NO end-of-session packet");
  std::printf("messages             %" PRIu64 " delivered, %" PRIu64 " duplicates skipped, %" PRIu64 " missing\n",
              r.messages, r.duplicates, r.missing());
  for (const feed::mold::Gap& g : r.gaps) {
    std::printf("gap                  sequence numbers %" PRIu64 " to %" PRIu64 " (%" PRIu64 " messages)\n", g.first,
                g.last, g.last - g.first + 1);
  }
  std::printf("wrote                %s (%zu bytes)\n", out_path.c_str(), re.feed.size());
  if (!r.complete()) {
    throw Mismatch{packets_path + ": " + (r.gaps.empty() ? std::string("the session never ended")
                                                         : std::to_string(r.missing()) + " messages are missing")};
  }
  return 0;
}

std::optional<std::size_t> parse_size(std::string_view s) {
  std::size_t v = 0;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage("a subcommand is required");
  const std::string cmd = argv[1];
  std::string run_dir, out_path, feed_path, packets_path;
  std::size_t depth = 5;
  feed::mold::PacketizeOptions mold_opt;
  for (int i = 2; i < argc; i += 2) {
    const std::string a = argv[i];
    if (i + 1 >= argc) return usage(a + " needs a value");
    const std::string v = argv[i + 1];
    if (a == "--run" && (cmd == "encode" || cmd == "check")) {
      run_dir = v;
    } else if (a == "--out" && (cmd == "encode" || cmd == "packetize" || cmd == "depacketize")) {
      out_path = v;
    } else if (a == "--feed" && (cmd == "decode" || cmd == "packetize")) {
      feed_path = v;
    } else if (a == "--packets" && cmd == "depacketize") {
      packets_path = v;
    } else if (a == "--session" && cmd == "packetize") {
      const auto s = feed::mold::make_session(v);
      if (!s) return usage("--session must be 1 to 10 printable characters without spaces");
      mold_opt.session = *s;
    } else if (a == "--max-packet" && cmd == "packetize") {
      const auto m = parse_size(v);
      if (!m || *m < feed::mold::kHeader + feed::kPrefix + 1 || *m > 0xFFFF) {
        return usage("--max-packet must be between 23 and 65535");
      }
      mold_opt.max_packet = *m;
    } else if (a == "--drop-packet" && cmd == "packetize") {
      const auto k = parse_size(v);
      if (!k || *k == 0) return usage("--drop-packet must be a positive integer");
      mold_opt.drop_packet = *k;
    } else if (a == "--depth" && cmd == "decode") {
      const auto d = parse_size(v);
      if (!d || *d == 0) return usage("--depth must be a positive integer");
      depth = *d;
    } else {
      return usage("unknown argument " + a + " for " + cmd);
    }
  }
  try {
    if (cmd == "encode") {
      if (run_dir.empty() || out_path.empty()) return usage("encode needs --run and --out");
      return encode(run_dir, out_path);
    }
    if (cmd == "decode") {
      if (feed_path.empty()) return usage("decode needs --feed");
      return decode(feed_path, depth);
    }
    if (cmd == "check") {
      if (run_dir.empty()) return usage("check needs --run");
      return check(run_dir);
    }
    if (cmd == "packetize") {
      if (feed_path.empty() || out_path.empty()) return usage("packetize needs --feed and --out");
      return packetize(feed_path, out_path, mold_opt);
    }
    if (cmd == "depacketize") {
      if (packets_path.empty() || out_path.empty()) return usage("depacketize needs --packets and --out");
      return depacketize(packets_path, out_path);
    }
    return usage("unknown subcommand " + cmd);
  } catch (const Mismatch& m) {
    std::fprintf(stderr, "lobsim_feed %s: MISMATCH: %s\n", cmd.c_str(), m.what.c_str());
    return 2;
  } catch (const BadInput& b) {
    std::fprintf(stderr, "lobsim_feed %s: %s\n", cmd.c_str(), b.what.c_str());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lobsim_feed %s: %s\n", cmd.c_str(), e.what());
    return 1;
  }
}
