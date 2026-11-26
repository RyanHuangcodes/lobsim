#pragma once

// MoldUDP64 packets around the ITCH feed (upgrade 6, phase F7). File and memory only: no sockets.
//
// A MoldUDP64 packet is a 20-byte header (session: 10 bytes, sequence number of the first message:
// 8 bytes, message count: 2 bytes, all big-endian), followed by `count` message blocks, each a 2-byte
// big-endian length and the message. Those blocks are exactly the framed messages of a feed file, so
// packetizing only groups them. A count of 0 is a heartbeat and 0xFFFF marks the end of the session;
// both carry no messages, and their sequence number is the next one the sender will use.
//
// UDP keeps packet boundaries, a file does not, so a packet file stores each packet behind its own
// 2-byte big-endian length (FEED.md). The reader follows the sequence numbers: a jump forward is a
// gap (lost packets), a number already delivered is a duplicate (a retransmission) and is skipped.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "feed/itch.hpp"

namespace feed::mold {

inline constexpr std::size_t kHeader = 20;
inline constexpr std::size_t kMaxPacket = 1400;  // default packet size limit, header included
inline constexpr std::uint16_t kHeartbeat = 0;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;
inline constexpr std::uint16_t kMaxCount = 0xFFFE;  // the largest count of a packet that carries messages

using Session = std::array<char, 10>;

// 1 to 10 printable ASCII characters, padded on the right with spaces; nullopt otherwise.
inline std::optional<Session> make_session(std::string_view name) {
  if (name.empty() || name.size() > 10) return std::nullopt;
  Session s;
  s.fill(' ');
  for (std::size_t i = 0; i < name.size(); ++i) {
    if (name[i] < 0x21 || name[i] > 0x7e) return std::nullopt;
    s[i] = name[i];
  }
  return s;
}

inline std::string to_string(const Session& s) {
  std::string out(s.begin(), s.end());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

struct PacketHeader {
  Session session{};
  std::uint64_t sequence{0};
  std::uint16_t count{0};
  bool operator==(const PacketHeader&) const = default;
};

inline void write_header(std::byte* out, const PacketHeader& h) noexcept {
  std::memcpy(out, h.session.data(), 10);
  be::store(out + 10, h.sequence);
  be::store(out + 18, h.count);
}

inline PacketHeader read_header(const std::byte* in) noexcept {
  PacketHeader h;
  std::memcpy(h.session.data(), in, 10);
  h.sequence = be::load<std::uint64_t>(in + 10);
  h.count = be::load<std::uint16_t>(in + 18);
  return h;
}

// The feed cannot be packetized: its framing is broken, or a message does not fit in a packet.
class PacketizeError : public std::runtime_error {
 public:
  PacketizeError(std::size_t offset, const std::string& what)
      : std::runtime_error("byte offset " + std::to_string(offset) + ": " + what), offset_(offset) {}
  std::size_t offset() const noexcept { return offset_; }

 private:
  std::size_t offset_;
};

struct PacketizeOptions {
  Session session = *make_session("LOBSIM");
  std::size_t max_packet{kMaxPacket};  // header included; at most 65,535 (the packet length prefix)
  std::uint64_t first_sequence{1};
  std::uint64_t drop_packet{0};  // 1-based number of a message packet to leave out (a loss, for tests); 0 = none
};

struct PacketizeStats {
  std::uint64_t packets{0};   // message packets written, the end-of-session packet not included
  std::uint64_t messages{0};
  std::uint64_t dropped_messages{0};
  std::uint64_t bytes{0};     // the packet file
};

// Groups the framed messages of `feed` (any ITCH file framed with 2-byte lengths) into packets of at
// most `max_packet` bytes, in order, sequence numbers counting from `first_sequence`, and ends the
// session with an end-of-session packet. Returns the packet file. Throws std::invalid_argument for a
// bad option and PacketizeError for a broken feed.
inline std::vector<std::byte> packetize(std::span<const std::byte> feed, const PacketizeOptions& opt,
                                        PacketizeStats* stats = nullptr) {
  if (opt.max_packet < kHeader + kPrefix + 1 || opt.max_packet > 0xFFFF) {
    throw std::invalid_argument("the packet size limit must be between " + std::to_string(kHeader + kPrefix + 1) +
                                " and 65535 bytes");
  }
  if (opt.first_sequence == 0) throw std::invalid_argument("MoldUDP64 sequence numbers start at 1 or more");
  PacketizeStats st;
  std::vector<std::byte> out;
  out.reserve(feed.size() + feed.size() / 40 + 64);
  std::uint64_t seq = opt.first_sequence;  // of the next message
  std::size_t start = 0;                   // packet start in `out`, after its length prefix
  std::uint16_t count = 0;
  std::uint64_t packet_no = 0;

  const auto open_packet = [&] {
    out.resize(out.size() + kPrefix + kHeader);
    start = out.size() - kHeader;
    count = 0;
  };
  const auto close_packet = [&](std::uint64_t first_seq, std::uint16_t n) {
    const std::size_t len = out.size() - start;
    be::store(out.data() + start - kPrefix, static_cast<std::uint16_t>(len));
    write_header(out.data() + start, PacketHeader{opt.session, first_seq, n});
  };
  const auto finish_message_packet = [&] {
    if (count == 0) return;
    ++packet_no;
    const std::uint64_t first = seq - count;
    if (packet_no == opt.drop_packet) {
      st.dropped_messages += count;
      out.resize(start - kPrefix);
    } else {
      close_packet(first, count);
      ++st.packets;
    }
    count = 0;
  };

  std::size_t at = 0;
  while (at < feed.size()) {
    if (feed.size() - at < kPrefix) throw PacketizeError(at, "truncated length prefix");
    const std::size_t len = be::load<std::uint16_t>(feed.data() + at);
    if (len == 0) throw PacketizeError(at, "message of length 0");
    if (feed.size() - at - kPrefix < len) throw PacketizeError(at, "truncated message body");
    const std::size_t block = kPrefix + len;
    if (kHeader + block > opt.max_packet) throw PacketizeError(at, "a message does not fit in one packet");
    if (count != 0 && (out.size() - start + block > opt.max_packet || count == kMaxCount)) finish_message_packet();
    if (count == 0) open_packet();
    out.insert(out.end(), feed.begin() + static_cast<std::ptrdiff_t>(at),
               feed.begin() + static_cast<std::ptrdiff_t>(at + block));
    ++count;
    ++seq;
    ++st.messages;
    at += block;
  }
  finish_message_packet();
  open_packet();
  close_packet(seq, kEndOfSession);  // its sequence number is the next one the sender would use
  st.bytes = out.size();
  if (stats != nullptr) *stats = st;
  return out;
}

enum class ReadError : std::uint8_t {
  None,
  TruncatedPacketLength,  // fewer than 2 bytes left for a packet length
  TruncatedPacket,        // the packet length promises more bytes than are left
  ShortPacket,            // a packet shorter than its 20-byte header
  MessageOverrun,         // a message block runs past the end of its packet
  CountMismatch,          // bytes left in a packet after its `count` messages
  SessionChanged,         // a packet of another session
  AfterEndOfSession,      // a packet after the end-of-session packet
  Stopped,                // the handler returned false
};

inline const char* to_string(ReadError e) noexcept {
  switch (e) {
    case ReadError::None: return "none";
    case ReadError::TruncatedPacketLength: return "truncated packet length";
    case ReadError::TruncatedPacket: return "truncated packet";
    case ReadError::ShortPacket: return "packet shorter than the 20-byte header";
    case ReadError::MessageOverrun: return "message block runs past the end of its packet";
    case ReadError::CountMismatch: return "bytes left in the packet after its message count";
    case ReadError::SessionChanged: return "packet of another session";
    case ReadError::AfterEndOfSession: return "packet after the end of the session";
    case ReadError::Stopped: return "stopped by the handler";
  }
  return "?";
}

// Sequence numbers first..last (inclusive) never arrived.
struct Gap {
  std::uint64_t first{0};
  std::uint64_t last{0};
  bool operator==(const Gap&) const = default;
};

struct ReadResult {
  ReadError error{ReadError::None};
  std::size_t offset{0};  // byte offset of the failing packet's length prefix, or the file size
  Session session{};
  std::uint64_t packets{0};     // every packet, heartbeats and the end of session included
  std::uint64_t heartbeats{0};
  std::uint64_t messages{0};    // delivered to the handler
  std::uint64_t duplicates{0};  // messages with a sequence number already delivered: skipped
  std::uint64_t next_sequence{1};
  bool ended{false};            // the end-of-session packet was read
  std::vector<Gap> gaps;
  bool ok() const noexcept { return error == ReadError::None; }
  bool complete() const noexcept { return ok() && ended && gaps.empty(); }
  std::uint64_t missing() const noexcept {
    std::uint64_t n = 0;
    for (const Gap& g : gaps) n += g.last - g.first + 1;
    return n;
  }
};

// Reads a packet file and delivers each message, in sequence order, once, to
// h.on_message(std::uint64_t sequence, std::span<const std::byte> framed) -> bool. The framed
// span is the 2-byte length and the message, as in a feed file. Sequence numbers are expected from
// `first_sequence`; every jump forward, including one shown by a heartbeat or the end-of-session
// packet, is recorded as a gap. Never reads past the buffer.
template <class Handler>
ReadResult read_packets(std::span<const std::byte> file, Handler& h, std::uint64_t first_sequence = 1) {
  ReadResult r;
  r.next_sequence = first_sequence;
  const std::byte* const p = file.data();
  const std::size_t n = file.size();
  std::size_t at = 0;
  bool have_session = false;
  const auto skip_to = [&r](std::uint64_t seq) {
    if (seq > r.next_sequence) {
      r.gaps.push_back(Gap{r.next_sequence, seq - 1});
      r.next_sequence = seq;
    }
  };
  while (at < n) {
    r.offset = at;
    if (n - at < kPrefix) {
      r.error = ReadError::TruncatedPacketLength;
      return r;
    }
    const std::size_t len = be::load<std::uint16_t>(p + at);
    if (n - at - kPrefix < len) {
      r.error = ReadError::TruncatedPacket;
      return r;
    }
    if (len < kHeader) {
      r.error = ReadError::ShortPacket;
      return r;
    }
    if (r.ended) {
      r.error = ReadError::AfterEndOfSession;
      return r;
    }
    const std::byte* const pkt = p + at + kPrefix;
    const PacketHeader hd = read_header(pkt);
    if (!have_session) {
      r.session = hd.session;
      have_session = true;
    } else if (hd.session != r.session) {
      r.error = ReadError::SessionChanged;
      return r;
    }
    ++r.packets;
    if (hd.count == kHeartbeat || hd.count == kEndOfSession) {
      if (len != kHeader) {
        r.error = ReadError::CountMismatch;
        return r;
      }
      if (hd.count == kHeartbeat) ++r.heartbeats;
      else r.ended = true;
      skip_to(hd.sequence);
      at += kPrefix + len;
      continue;
    }
    // Check the whole packet's framing before delivering any of it.
    std::size_t in = kHeader;
    for (std::uint16_t i = 0; i < hd.count; ++i) {
      if (len - in < kPrefix || len - in - kPrefix < be::load<std::uint16_t>(pkt + in)) {
        r.error = ReadError::MessageOverrun;
        return r;
      }
      in += kPrefix + be::load<std::uint16_t>(pkt + in);
    }
    if (in != len) {
      r.error = ReadError::CountMismatch;
      return r;
    }
    in = kHeader;
    for (std::uint16_t i = 0; i < hd.count; ++i) {
      const std::size_t block = kPrefix + be::load<std::uint16_t>(pkt + in);
      const std::uint64_t seq = hd.sequence + i;
      if (seq < r.next_sequence) {
        ++r.duplicates;
      } else {
        skip_to(seq);
        ++r.messages;
        r.next_sequence = seq + 1;
        if (!h.on_message(seq, std::span<const std::byte>(pkt + in, block))) {
          r.error = ReadError::Stopped;
          return r;
        }
      }
      in += block;
    }
    at += kPrefix + len;
  }
  r.offset = at;
  return r;
}

// A handler that puts the delivered messages back together as a feed file.
struct Reassembler {
  std::vector<std::byte> feed;
  bool on_message(std::uint64_t, std::span<const std::byte> framed) {
    feed.insert(feed.end(), framed.begin(), framed.end());
    return true;
  }
};

}  // namespace feed::mold
