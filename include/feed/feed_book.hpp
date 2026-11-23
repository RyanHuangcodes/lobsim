#pragma once

// The order book rebuilt from the feed alone (upgrade 6). Each message is applied to a
// lob::OrderBook through the book-builder operations (insert_passive, reduce, remove), with no
// matching. A message that does not fit the book (an unknown reference, an A that would cross, an
// execution larger than the order) stops the rebuild and names the message. Use it as the
// handler of feed::parse.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "feed/itch.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace feed {

class FeedBook {
 public:
  // The largest price a Price(4) field can carry, in ticks.
  static constexpr lob::Price kMaxTicks = std::numeric_limits<std::uint32_t>::max() / 100;
  // References index a vector (D-302); a reference this large is refused instead of allocated.
  static constexpr std::uint64_t kMaxRef = std::uint64_t{1} << 26;

  explicit FeedBook(std::size_t expected_refs = 0);
  FeedBook(const FeedBook&) = delete;
  FeedBook& operator=(const FeedBook&) = delete;

  bool on(const MsgS& m);
  bool on(const MsgA& m);
  bool on(const MsgE& m);
  bool on(const MsgX& m);
  bool on(const MsgD& m);
  bool on_unknown(std::uint8_t type, std::span<const std::byte> body);
  // After the last message: the feed must have ended with S C. Returns false (and sets error()) if not.
  bool finish();

  const lob::OrderBook& book() const noexcept { return book_; }
  bool failed() const noexcept { return !error_.empty(); }
  // "message N (type T): what"; empty while nothing failed.
  const std::string& error() const noexcept { return error_; }
  // 1-based index of the last message seen (the failing one after a failure).
  std::uint64_t messages() const noexcept { return messages_; }
  std::uint64_t executed_shares() const noexcept { return executed_shares_; }

 private:
  bool enter(char type);  // counts the message and checks its place between S O and S C
  bool fail(char type, const std::string& what);
  lob::OrderId lookup(std::uint64_t ref) const noexcept {
    return ref < ref_to_book_.size() ? ref_to_book_[ref] : 0;
  }

  lob::OrderBook book_;
  std::vector<lob::OrderId> ref_to_book_;  // 0 when the reference is not resting
  std::uint64_t messages_{0};
  std::uint64_t executed_shares_{0};
  bool open_{false};
  bool closed_{false};
  std::string error_;
};

// FNV-1a 64 over both sides (bids then asks, best price first): per side the level count, then per
// level its price, total quantity and order count, then each order's remaining quantity in FIFO
// order. Order ids are left out, because two books built from the same flow number their orders
// differently; the FIFO order still captures time priority.
std::uint64_t book_digest(const lob::OrderBook& book);

}  // namespace feed
