#include "diff_driver.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <optional>
#include <variant>

#include "lob/apply.hpp"
#include "lob/order_book.hpp"
#include "lob/reference_book.hpp"

namespace lob::fuzz {

Request decode_record(const std::uint8_t* rec, std::uint64_t index,
                      const std::vector<OrderId>& known_ids) {
  const unsigned kind = rec[0] % 3;
  const OwnerId owner = 1 + rec[1] % 3;
  const unsigned flags = rec[2];

  const auto p16 = static_cast<std::int16_t>(static_cast<std::uint16_t>(rec[3] | (rec[4] << 8)));
  const bool raw_price = (flags & 0x20) != 0;
  const Price price =
      raw_price ? static_cast<Price>(p16) : 95 + static_cast<Price>(static_cast<std::uint16_t>(p16) % 11);

  const bool raw_qty = (flags & 0x40) != 0;
  const Qty qty = raw_qty ? static_cast<Qty>(static_cast<std::int8_t>(rec[5])) : 1 + rec[5] % 10;

  const std::uint64_t t = static_cast<std::uint64_t>(rec[6] | (rec[7] << 8));
  const OrderId target = (t % 8 == 7 || known_ids.empty()) ? t : known_ids[t % known_ids.size()];

  if (kind == 1) return Cancel{owner, target};
  if (kind == 2) return Modify{owner, target, price, qty};

  NewOrder n;
  n.owner = owner;
  n.client_order_id = index;
  n.side = (flags & 0x01) ? Side::Sell : Side::Buy;
  n.post_only = (flags & 0x02) != 0;
  n.stp = (flags & 0x04) ? STP::CancelResting : STP::CancelTaking;
  switch ((flags >> 3) & 3) {
    case 0: n.type = OrderType::Limit; n.tif = TIF::GTC; break;
    case 1: n.type = OrderType::Limit; n.tif = TIF::IOC; break;
    case 2: n.type = OrderType::Limit; n.tif = TIF::FOK; break;
    default:
      n.type = OrderType::Market;
      n.tif = (flags & 0x80) ? TIF::GTC : TIF::IOC;  // bad_tif: a Market order with GTC is invalid
      break;
  }
  n.price = price;
  n.qty = qty;
  return n;
}

namespace {

void collect_acks(const EventBuffer& buf, std::vector<OrderId>& ids) {
  for (const Event& ev : buf) {
    if (const auto* a = std::get_if<Ack>(&ev)) ids.push_back(a->order_id);
  }
}

OrderId target_of(const Request& r) {
  if (const auto* c = std::get_if<Cancel>(&r)) return c->order_id;
  if (const auto* m = std::get_if<Modify>(&r)) return m->order_id;
  return 0;
}

bool has_target(const Request& r) { return !std::holds_alternative<NewOrder>(r); }

void print_events(const char* title, const EventBuffer& buf) {
  std::fprintf(stderr, "  %s (%zu events)\n", title, buf.size());
  for (const Event& ev : buf) std::fprintf(stderr, "    %s\n", lob::to_string(ev).c_str());
}

void print_depth(const char* title, const std::vector<LevelView>& d) {
  std::fprintf(stderr, "  %s:", title);
  for (const LevelView& l : d) {
    std::fprintf(stderr, " %lld x %lld", static_cast<long long>(l.price), static_cast<long long>(l.total_qty));
  }
  std::fprintf(stderr, "\n");
}

[[noreturn]] void fail(const char* what, std::uint64_t index, const Request& req, const EventBuffer& fast,
                       const EventBuffer& ref, const char* detail = nullptr) {
  std::fprintf(stderr, "lobsim fuzz: %s\n", what);
  if (detail) std::fprintf(stderr, "  %s\n", detail);
  std::fprintf(stderr, "  request #%llu: %s\n", static_cast<unsigned long long>(index),
               to_string(req).c_str());
  print_events("OrderBook", fast);
  print_events("ReferenceBook", ref);
  std::fflush(stderr);
  std::abort();
}

template <class Book>
std::optional<std::string> try_apply(Book& book, const Request& req, EventBuffer& out) {
  try {
    lob::apply(book, req, out);
  } catch (const std::exception& e) {
    return std::string(e.what());
  } catch (...) {
    return std::string("non-standard exception");
  }
  return std::nullopt;
}

template <class Book>
std::optional<std::string> try_validate(const Book& book) {
  try {
    book.validate();
  } catch (const std::exception& e) {
    return std::string(e.what());
  } catch (...) {
    return std::string("non-standard exception");
  }
  return std::nullopt;
}

}  // namespace

std::vector<Request> decode(const std::uint8_t* data, std::size_t size,
                            std::vector<EventBuffer>* reference_events) {
  std::vector<Request> out;
  if (reference_events) reference_events->clear();
  ReferenceBook ref(BookConfig{kFuzzMaxPrice});
  std::vector<OrderId> known_ids;
  const std::size_t n = size / kRecordSize;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    Request req = decode_record(data + i * kRecordSize, i, known_ids);
    EventBuffer buf;
    // A throwing reference book (a failed validate() in a non-NDEBUG build) leaves buf partial;
    // the ids seen so far are still used so that a crash file always decodes.
    (void)try_apply(ref, req, buf);
    collect_acks(buf, known_ids);
    out.push_back(std::move(req));
    if (reference_events) reference_events->push_back(std::move(buf));
  }
  return out;
}

void run_diff(const std::uint8_t* data, std::size_t size) {
  OrderBook fast(BookConfig{kFuzzMaxPrice});
  ReferenceBook ref(BookConfig{kFuzzMaxPrice});
  std::vector<OrderId> known_ids;
  const std::size_t n = size / kRecordSize;
  constexpr std::size_t kAll = std::numeric_limits<std::size_t>::max();

  for (std::size_t i = 0; i < n; ++i) {
    const Request req = decode_record(data + i * kRecordSize, i, known_ids);
    EventBuffer fast_ev, ref_ev;

    if (auto e = try_apply(fast, req, fast_ev)) {
      const std::string d = "OrderBook threw: " + *e;
      (void)try_apply(ref, req, ref_ev);
      fail("OrderBook threw while applying the request", i, req, fast_ev, ref_ev, d.c_str());
    }
    if (auto e = try_apply(ref, req, ref_ev)) {
      const std::string d = "ReferenceBook threw: " + *e;
      fail("ReferenceBook threw while applying the request", i, req, fast_ev, ref_ev, d.c_str());
    }
    if (fast_ev != ref_ev) fail("event mismatch", i, req, fast_ev, ref_ev);

    if (auto e = try_validate(fast)) {
      const std::string d = "OrderBook::validate: " + *e;
      fail("OrderBook validate() failed", i, req, fast_ev, ref_ev, d.c_str());
    }
    if (auto e = try_validate(ref)) {
      const std::string d = "ReferenceBook::validate: " + *e;
      fail("ReferenceBook validate() failed", i, req, fast_ev, ref_ev, d.c_str());
    }

    for (const Side side : {Side::Buy, Side::Sell}) {
      const std::vector<LevelView> df = fast.depth(side, kAll);
      const std::vector<LevelView> dr = ref.depth(side, kAll);
      if (df != dr) {
        std::fprintf(stderr, "lobsim fuzz: depth mismatch on the %s side\n", lob::to_string(side));
        print_depth("OrderBook depth", df);
        print_depth("ReferenceBook depth", dr);
        fail("depth mismatch", i, req, fast_ev, ref_ev);
      }
    }
    if (fast.resting_orders() != ref.resting_orders()) {
      const std::string d = "resting_orders: OrderBook " + std::to_string(fast.resting_orders()) +
                            ", ReferenceBook " + std::to_string(ref.resting_orders());
      fail("resting_orders mismatch", i, req, fast_ev, ref_ev, d.c_str());
    }
    if (has_target(req)) {
      const OrderId id = target_of(req);
      if (fast.order(id) != ref.order(id)) {
        const std::string d = "order(" + std::to_string(id) + ") differs between the books";
        fail("order() mismatch", i, req, fast_ev, ref_ev, d.c_str());
      }
    }

    collect_acks(fast_ev, known_ids);
  }
}

std::string to_string(const Request& r) {
  struct Visitor {
    std::string operator()(const NewOrder& o) const {
      return "New owner=" + std::to_string(o.owner) + " side=" + lob::to_string(o.side) +
             " type=" + lob::to_string(o.type) + " tif=" + lob::to_string(o.tif) +
             " price=" + std::to_string(o.price) + " qty=" + std::to_string(o.qty) +
             " post_only=" + (o.post_only ? "1" : "0") + " stp=" + lob::to_string(o.stp);
    }
    std::string operator()(const Cancel& c) const {
      return "Cancel owner=" + std::to_string(c.owner) + " id=" + std::to_string(c.order_id);
    }
    std::string operator()(const Modify& m) const {
      return "Modify owner=" + std::to_string(m.owner) + " id=" + std::to_string(m.order_id) +
             " price=" + std::to_string(m.new_price) + " qty=" + std::to_string(m.new_qty);
    }
  };
  return std::visit(Visitor{}, r);
}

}  // namespace lob::fuzz
