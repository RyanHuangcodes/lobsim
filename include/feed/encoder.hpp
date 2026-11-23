#pragma once

// The feed encoder (upgrade 6): turns the engine events of each request into ITCH 5.0 messages
// (FEED.md, DECISIONS D-306 to D-311). One pass over each request's events, a mirror of the
// resting orders indexed by order id, and one output buffer written through a raw cursor. No heap
// allocation per message once the buffer and the mirror have their size.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "feed/itch.hpp"
#include "lob/events.hpp"
#include "lob/types.hpp"

namespace feed {

// A value that does not fit its ITCH field (D-303 to D-305), or events that contradict the mirror.
// `request()` is the 1-based number of the request being encoded (0 for the start message).
class EncodeError : public std::runtime_error {
 public:
  EncodeError(std::uint64_t request, const std::string& what)
      : std::runtime_error("request " + std::to_string(request) + ": " + what), request_(request) {}
  std::uint64_t request() const noexcept { return request_; }

 private:
  std::uint64_t request_;
};

struct Counts {
  std::uint64_t S{0}, A{0}, E{0}, X{0}, D{0};
  std::uint64_t bytes{0};  // every byte written, length prefixes included
  std::uint64_t messages() const noexcept { return S + A + E + X + D; }
  bool operator==(const Counts&) const = default;
};

class Encoder {
 public:
  static constexpr std::size_t kDefaultBuffer = 1 << 20;
  static constexpr std::size_t kFlushBelow = 64;  // free bytes that trigger a flush (> kMaxFramed)

  // out: the file to write, or nullptr to keep everything in memory (read it with buffered()).
  // expected_orders: the largest order id expected, so the mirror never grows while encoding.
  explicit Encoder(std::FILE* out = nullptr, std::size_t expected_orders = 0,
                   std::size_t buffer_bytes = kDefaultBuffer);

  // System event O, before the first request. Times are simulated ns (9:30 is added, D-303).
  void start(std::int64_t sim_ns);
  // The messages of one request, from the events the engine produced for it, in order.
  void on_request(std::int64_t sim_ns, const lob::Request& req, std::span<const lob::Event> events);
  // System event C, then a flush when writing to a file.
  void finish(std::int64_t sim_ns);

  // Memory mode: the bytes written since the last clear().
  std::span<const std::byte> buffered() const noexcept { return {buf_.data(), pos_}; }
  void clear() noexcept { pos_ = 0; }
  // File mode: writes the buffered bytes with one fwrite. Throws std::runtime_error on a write error.
  void flush();

  const Counts& counts() const noexcept { return counts_; }
  std::uint64_t requests() const noexcept { return requests_; }

 private:
  struct Resting {
    lob::Price price{0};
    lob::Qty remaining{0};
    lob::Side side{lob::Side::Buy};
    bool live{false};
  };

  std::byte* room();  // a cursor with at least kFlushBelow free bytes
  Resting& slot(lob::OrderId id);
  bool live(lob::OrderId id) const noexcept { return id < mirror_.size() && mirror_[id].live; }
  std::uint64_t timestamp(std::int64_t sim_ns) const;
  std::uint32_t shares(lob::Qty q) const;
  std::uint32_t price(lob::Price p) const;

  void emit_s(char code);
  void emit_a(lob::OrderId id, lob::Side side, lob::Qty qty, lob::Price px);
  void emit_e(lob::OrderId maker, lob::Qty qty, std::uint64_t match);
  void emit_x(lob::OrderId id, lob::Qty qty);
  void emit_d(lob::OrderId id);

  std::FILE* out_;
  std::vector<std::byte> buf_;
  std::size_t pos_{0};
  std::vector<Resting> mirror_;
  Counts counts_;
  std::uint64_t requests_{0};
  std::uint64_t ts_{0};  // the current request's timestamp
};

}  // namespace feed
