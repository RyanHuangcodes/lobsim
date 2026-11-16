#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "lob/types.hpp"

namespace lob {

enum class RejectReason : std::uint8_t {
  InvalidQty, InvalidPrice, InvalidFlags, UnknownOrder, NotOwner, NoChange, PostOnlyWouldCross
};
enum class CancelReason : std::uint8_t { User, IOCRemainder, NoLiquidity, FOKUnfillable, SelfTrade };
enum class RequestKind : std::uint8_t { New, Cancel, Modify };

struct Ack {
  OrderId order_id{};
  OwnerId owner{};
  std::uint64_t client_order_id{};
  bool operator==(const Ack&) const = default;
};
struct Reject {
  OwnerId owner{};
  RequestKind request_kind{};
  std::uint64_t client_order_id{};
  OrderId order_id{};
  RejectReason reason{};
  bool operator==(const Reject&) const = default;
};
struct Trade {
  std::uint64_t trade_id{};
  Price price{};
  Qty qty{};
  OrderId maker_order_id{};
  OrderId taker_order_id{};
  OwnerId maker_owner{};
  OwnerId taker_owner{};
  Side aggressor_side{};
  bool operator==(const Trade&) const = default;
};
struct Cancelled {
  OrderId order_id{};
  OwnerId owner{};
  Qty qty_cancelled{};
  CancelReason reason{};
  bool operator==(const Cancelled&) const = default;
};
struct Modified {
  OrderId order_id{};
  OwnerId owner{};
  Price new_price{};
  Qty new_qty{};
  bool priority_kept{};
  bool operator==(const Modified&) const = default;
};

// The alternative order is the tag order used by the event hash (D-015).
using Event = std::variant<Ack, Reject, Trade, Cancelled, Modified>;
using EventBuffer = std::vector<Event>;

inline const char* to_string(RejectReason v) noexcept {
  switch (v) {
    case RejectReason::InvalidQty: return "InvalidQty";
    case RejectReason::InvalidPrice: return "InvalidPrice";
    case RejectReason::InvalidFlags: return "InvalidFlags";
    case RejectReason::UnknownOrder: return "UnknownOrder";
    case RejectReason::NotOwner: return "NotOwner";
    case RejectReason::NoChange: return "NoChange";
    case RejectReason::PostOnlyWouldCross: return "PostOnlyWouldCross";
  }
  return "?";
}
inline const char* to_string(CancelReason v) noexcept {
  switch (v) {
    case CancelReason::User: return "User";
    case CancelReason::IOCRemainder: return "IOCRemainder";
    case CancelReason::NoLiquidity: return "NoLiquidity";
    case CancelReason::FOKUnfillable: return "FOKUnfillable";
    case CancelReason::SelfTrade: return "SelfTrade";
  }
  return "?";
}
inline const char* to_string(RequestKind v) noexcept {
  switch (v) {
    case RequestKind::New: return "New";
    case RequestKind::Cancel: return "Cancel";
    case RequestKind::Modify: return "Modify";
  }
  return "?";
}

inline std::string to_string(const Event& ev) {
  using std::to_string;
  struct Visitor {
    std::string operator()(const Ack& e) const {
      return "Ack{order_id=" + to_string(e.order_id) + ", owner=" + to_string(e.owner) +
             ", client_order_id=" + to_string(e.client_order_id) + "}";
    }
    std::string operator()(const Reject& e) const {
      return std::string("Reject{owner=") + to_string(e.owner) +
             ", request_kind=" + lob::to_string(e.request_kind) +
             ", client_order_id=" + to_string(e.client_order_id) +
             ", order_id=" + to_string(e.order_id) + ", reason=" + lob::to_string(e.reason) + "}";
    }
    std::string operator()(const Trade& e) const {
      return "Trade{trade_id=" + to_string(e.trade_id) + ", price=" + to_string(e.price) +
             ", qty=" + to_string(e.qty) + ", maker_order_id=" + to_string(e.maker_order_id) +
             ", taker_order_id=" + to_string(e.taker_order_id) +
             ", maker_owner=" + to_string(e.maker_owner) +
             ", taker_owner=" + to_string(e.taker_owner) +
             ", aggressor_side=" + lob::to_string(e.aggressor_side) + "}";
    }
    std::string operator()(const Cancelled& e) const {
      return "Cancelled{order_id=" + to_string(e.order_id) + ", owner=" + to_string(e.owner) +
             ", qty_cancelled=" + to_string(e.qty_cancelled) +
             ", reason=" + lob::to_string(e.reason) + "}";
    }
    std::string operator()(const Modified& e) const {
      return "Modified{order_id=" + to_string(e.order_id) + ", owner=" + to_string(e.owner) +
             ", new_price=" + to_string(e.new_price) + ", new_qty=" + to_string(e.new_qty) +
             ", priority_kept=" + (e.priority_kept ? "true" : "false") + "}";
    }
  };
  return std::visit(Visitor{}, ev);
}

// FNV-1a 64-bit over the fixed little-endian encoding of D-015.
class EventHasher {
 public:
  void add_bytes(const void* data, std::size_t n) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
      h_ ^= p[i];
      h_ *= kPrime;
    }
  }

  void add(const Event& ev) noexcept {
    std::visit([this](const auto& e) { encode(e); }, ev);
  }

  void add(const EventBuffer& buf) noexcept {
    for (const Event& ev : buf) add(ev);
  }

  std::uint64_t value() const noexcept { return h_; }

 private:
  static constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
  static constexpr std::uint64_t kPrime = 0x100000001b3ULL;

  void u8(std::uint8_t v) noexcept { add_bytes(&v, 1); }
  void u32(std::uint32_t v) noexcept {
    unsigned char b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>(v >> (8 * i));
    add_bytes(b, 4);
  }
  void u64(std::uint64_t v) noexcept {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(v >> (8 * i));
    add_bytes(b, 8);
  }
  void i64(std::int64_t v) noexcept { u64(static_cast<std::uint64_t>(v)); }
  template <class E>
  void en(E v) noexcept { u8(static_cast<std::uint8_t>(v)); }

  void encode(const Ack& e) noexcept {
    u8(0); u64(e.order_id); u32(e.owner); u64(e.client_order_id);
  }
  void encode(const Reject& e) noexcept {
    u8(1); u32(e.owner); en(e.request_kind); u64(e.client_order_id); u64(e.order_id); en(e.reason);
  }
  void encode(const Trade& e) noexcept {
    u8(2); u64(e.trade_id); i64(e.price); i64(e.qty); u64(e.maker_order_id);
    u64(e.taker_order_id); u32(e.maker_owner); u32(e.taker_owner); en(e.aggressor_side);
  }
  void encode(const Cancelled& e) noexcept {
    u8(3); u64(e.order_id); u32(e.owner); i64(e.qty_cancelled); en(e.reason);
  }
  void encode(const Modified& e) noexcept {
    u8(4); u64(e.order_id); u32(e.owner); i64(e.new_price); i64(e.new_qty);
    u8(e.priority_kept ? 1 : 0);
  }

  std::uint64_t h_{kOffset};
};

} // namespace lob
