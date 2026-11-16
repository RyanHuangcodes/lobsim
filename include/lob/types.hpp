#pragma once

#include <cstdint>
#include <variant>

namespace lob {

using Price = std::int64_t;    // ticks, valid range [1, max_price]
using Qty = std::int64_t;      // lots, > 0
using OrderId = std::uint64_t; // assigned by engine, starts at 1, strictly increasing
using OwnerId = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class TIF : std::uint8_t { GTC, IOC, FOK };
enum class STP : std::uint8_t { CancelTaking, CancelResting };

struct NewOrder {
  OwnerId owner{};
  std::uint64_t client_order_id{};
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  TIF tif{TIF::GTC};
  Price price{};
  Qty qty{};
  bool post_only{false};
  STP stp{STP::CancelTaking};
  bool operator==(const NewOrder&) const = default;
};

struct Cancel {
  OwnerId owner{};
  OrderId order_id{};
  bool operator==(const Cancel&) const = default;
};

struct Modify {
  OwnerId owner{};
  OrderId order_id{};
  Price new_price{};
  Qty new_qty{};
  bool operator==(const Modify&) const = default;
};

using Request = std::variant<NewOrder, Cancel, Modify>;

struct BookConfig {
  Price max_price{1'000'000};
  bool operator==(const BookConfig&) const = default;
};

struct LevelView {
  Price price{};
  Qty total_qty{};
  bool operator==(const LevelView&) const = default;
};

struct OrderView {
  OrderId id{};
  OwnerId owner{};
  Side side{};
  Price price{};
  Qty remaining{};
  STP stp{};
  bool post_only{};
  bool operator==(const OrderView&) const = default;
};

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

inline const char* to_string(Side v) noexcept {
  switch (v) {
    case Side::Buy: return "Buy";
    case Side::Sell: return "Sell";
  }
  return "?";
}
inline const char* to_string(OrderType v) noexcept {
  switch (v) {
    case OrderType::Limit: return "Limit";
    case OrderType::Market: return "Market";
  }
  return "?";
}
inline const char* to_string(TIF v) noexcept {
  switch (v) {
    case TIF::GTC: return "GTC";
    case TIF::IOC: return "IOC";
    case TIF::FOK: return "FOK";
  }
  return "?";
}
inline const char* to_string(STP v) noexcept {
  switch (v) {
    case STP::CancelTaking: return "CancelTaking";
    case STP::CancelResting: return "CancelResting";
  }
  return "?";
}

} // namespace lob
