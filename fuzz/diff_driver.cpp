#include "diff_driver.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

#include "lob/apply.hpp"
#include "lob/order_book.hpp"
#include "lob/reference_book.hpp"

namespace lob::fuzz {

namespace {

// The fields of a record that every kind shares (3.1).
struct Fields {
  OwnerId owner;
  unsigned flags;
  Price price;
  Qty qty;
  OrderId target;
};

Fields decode_fields(const std::uint8_t* rec, const std::vector<OrderId>& known_ids) {
  Fields f;
  f.owner = 1 + rec[1] % 3;
  f.flags = rec[2];

  const auto p16 = static_cast<std::int16_t>(static_cast<std::uint16_t>(rec[3] | (rec[4] << 8)));
  const bool raw_price = (f.flags & 0x20) != 0;
  f.price =
      raw_price ? static_cast<Price>(p16) : 95 + static_cast<Price>(static_cast<std::uint16_t>(p16) % 11);

  const bool raw_qty = (f.flags & 0x40) != 0;
  f.qty = raw_qty ? static_cast<Qty>(static_cast<std::int8_t>(rec[5])) : 1 + rec[5] % 10;

  const std::uint64_t t = static_cast<std::uint64_t>(rec[6] | (rec[7] << 8));
  f.target = (t % 8 == 7 || known_ids.empty()) ? t : known_ids[t % known_ids.size()];
  return f;
}

// Kinds 0-2, shared by fuzz_diff and fuzz_diff_builder.
Request make_request(unsigned kind, const Fields& f, std::uint64_t index) {
  if (kind == 1) return Cancel{f.owner, f.target};
  if (kind == 2) return Modify{f.owner, f.target, f.price, f.qty};

  NewOrder n;
  n.owner = f.owner;
  n.client_order_id = index;
  n.side = (f.flags & 0x01) ? Side::Sell : Side::Buy;
  n.post_only = (f.flags & 0x02) != 0;
  n.stp = (f.flags & 0x04) ? STP::CancelResting : STP::CancelTaking;
  switch ((f.flags >> 3) & 3) {
    case 0: n.type = OrderType::Limit; n.tif = TIF::GTC; break;
    case 1: n.type = OrderType::Limit; n.tif = TIF::IOC; break;
    case 2: n.type = OrderType::Limit; n.tif = TIF::FOK; break;
    default:
      n.type = OrderType::Market;
      n.tif = (f.flags & 0x80) ? TIF::GTC : TIF::IOC;  // bad_tif: a Market order with GTC is invalid
      break;
  }
  n.price = f.price;
  n.qty = f.qty;
  return n;
}

}  // namespace

Request decode_record(const std::uint8_t* rec, std::uint64_t index,
                      const std::vector<OrderId>& known_ids) {
  return make_request(rec[0] % 3, decode_fields(rec, known_ids), index);
}

BuilderOp decode_builder_record(const std::uint8_t* rec, std::uint64_t index,
                                const std::vector<OrderId>& known_ids) {
  const unsigned kind = rec[0] % 7;
  const Fields f = decode_fields(rec, known_ids);
  const Side side = (f.flags & 0x01) ? Side::Sell : Side::Buy;
  switch (kind) {
    case 3: return InsertPassive{side, f.price, f.qty, f.owner};
    case 4: return Reduce{f.target, f.qty};
    case 5: return Remove{f.target};
    case 6: return PreviewMatch{side, f.price, f.qty};
    default: break;
  }
  return std::visit([](const auto& r) { return BuilderOp{r}; }, make_request(kind, f, index));
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

[[noreturn]] void fail(const char* what, std::uint64_t index, const std::string& req_text,
                       const EventBuffer& fast, const EventBuffer& ref, const char* detail = nullptr) {
  std::fprintf(stderr, "lobsim fuzz: %s\n", what);
  if (detail) std::fprintf(stderr, "  %s\n", detail);
  std::fprintf(stderr, "  request #%llu: %s\n", static_cast<unsigned long long>(index),
               req_text.c_str());
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
    const std::string req_text = to_string(req);
    EventBuffer fast_ev, ref_ev;

    if (auto e = try_apply(fast, req, fast_ev)) {
      const std::string d = "OrderBook threw: " + *e;
      (void)try_apply(ref, req, ref_ev);
      fail("OrderBook threw while applying the request", i, req_text, fast_ev, ref_ev, d.c_str());
    }
    if (auto e = try_apply(ref, req, ref_ev)) {
      const std::string d = "ReferenceBook threw: " + *e;
      fail("ReferenceBook threw while applying the request", i, req_text, fast_ev, ref_ev, d.c_str());
    }
    if (fast_ev != ref_ev) fail("event mismatch", i, req_text, fast_ev, ref_ev);

    if (auto e = try_validate(fast)) {
      const std::string d = "OrderBook::validate: " + *e;
      fail("OrderBook validate() failed", i, req_text, fast_ev, ref_ev, d.c_str());
    }
    if (auto e = try_validate(ref)) {
      const std::string d = "ReferenceBook::validate: " + *e;
      fail("ReferenceBook validate() failed", i, req_text, fast_ev, ref_ev, d.c_str());
    }

    for (const Side side : {Side::Buy, Side::Sell}) {
      const std::vector<LevelView> df = fast.depth(side, kAll);
      const std::vector<LevelView> dr = ref.depth(side, kAll);
      if (df != dr) {
        std::fprintf(stderr, "lobsim fuzz: depth mismatch on the %s side\n", lob::to_string(side));
        print_depth("OrderBook depth", df);
        print_depth("ReferenceBook depth", dr);
        fail("depth mismatch", i, req_text, fast_ev, ref_ev);
      }
    }
    if (fast.resting_orders() != ref.resting_orders()) {
      const std::string d = "resting_orders: OrderBook " + std::to_string(fast.resting_orders()) +
                            ", ReferenceBook " + std::to_string(ref.resting_orders());
      fail("resting_orders mismatch", i, req_text, fast_ev, ref_ev, d.c_str());
    }
    if (has_target(req)) {
      const OrderId id = target_of(req);
      if (fast.order(id) != ref.order(id)) {
        const std::string d = "order(" + std::to_string(id) + ") differs between the books";
        fail("order() mismatch", i, req_text, fast_ev, ref_ev, d.c_str());
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

namespace {

// What one builder operation returned, besides its events.
struct OpResult {
  Status status{Status::Ok};
  OrderId id{0};
  std::vector<Fill> fills;
  bool operator==(const OpResult&) const = default;
};

template <class Book>
OpResult apply_op(Book& book, const BuilderOp& op, EventBuffer& ev) {
  struct Dispatch {
    Book& book;
    EventBuffer& ev;
    OpResult& r;
    void operator()(const NewOrder& x) const { book.submit(x, ev); }
    void operator()(const Cancel& x) const { book.cancel(x, ev); }
    void operator()(const Modify& x) const { book.modify(x, ev); }
    void operator()(const InsertPassive& x) const {
      const Result<OrderId> res = book.insert_passive(x.side, x.price, x.qty, x.owner);
      r.status = res.status;
      r.id = res.value;
    }
    void operator()(const Reduce& x) const { r.status = book.reduce(x.order_id, x.qty); }
    void operator()(const Remove& x) const { r.status = book.remove(x.order_id); }
    void operator()(const PreviewMatch& x) const { r.fills = book.preview_match(x.taker_side, x.limit, x.qty); }
  };
  OpResult r;
  std::visit(Dispatch{book, ev, r}, op);
  return r;
}

template <class Book>
std::optional<std::string> try_apply_op(Book& book, const BuilderOp& op, EventBuffer& ev, OpResult& r) {
  try {
    r = apply_op(book, op, ev);
  } catch (const std::exception& e) {
    return std::string(e.what());
  } catch (...) {
    return std::string("non-standard exception");
  }
  return std::nullopt;
}

std::string describe(const OpResult& r) {
  std::string s = "status=" + std::string(lob::to_string(r.status)) + " id=" + std::to_string(r.id) + " fills=[";
  for (std::size_t i = 0; i < r.fills.size(); ++i) {
    if (i != 0) s += ", ";
    s += std::to_string(r.fills[i].maker_id) + "@" + std::to_string(r.fills[i].price) + "x" +
         std::to_string(r.fills[i].qty);
  }
  return s + "]";
}

std::string describe(const std::vector<OrderView>& orders) {
  std::string s;
  for (const OrderView& o : orders) {
    s += " [id=" + std::to_string(o.id) + " owner=" + std::to_string(o.owner) + " rem=" + std::to_string(o.remaining) + "]";
  }
  return s.empty() ? " (none)" : s;
}

bool has_target(const BuilderOp& op) {
  return std::holds_alternative<Cancel>(op) || std::holds_alternative<Modify>(op) ||
         std::holds_alternative<Reduce>(op) || std::holds_alternative<Remove>(op);
}

OrderId target_of(const BuilderOp& op) {
  if (const auto* c = std::get_if<Cancel>(&op)) return c->order_id;
  if (const auto* m = std::get_if<Modify>(&op)) return m->order_id;
  if (const auto* r = std::get_if<Reduce>(&op)) return r->order_id;
  if (const auto* r = std::get_if<Remove>(&op)) return r->order_id;
  return 0;
}

// The (side, price) levels whose queues the operation can change, found before it runs.
std::vector<std::pair<Side, Price>> touched_levels(const ReferenceBook& ref, const BuilderOp& op) {
  std::vector<std::pair<Side, Price>> levels;
  if (const auto* n = std::get_if<NewOrder>(&op)) {
    levels.emplace_back(n->side, n->price);
  } else if (const auto* i = std::get_if<InsertPassive>(&op)) {
    levels.emplace_back(i->side, i->price);
  } else if (const auto* p = std::get_if<PreviewMatch>(&op)) {
    if (const auto best = ref.best_level(opposite(p->taker_side))) levels.emplace_back(opposite(p->taker_side), best->price);
  } else if (has_target(op)) {
    if (const auto o = ref.order(target_of(op))) {
      levels.emplace_back(o->side, o->price);
      if (const auto* m = std::get_if<Modify>(&op)) levels.emplace_back(o->side, m->new_price);
    }
  }
  return levels;
}

}  // namespace

std::vector<BuilderOp> decode_builder(const std::uint8_t* data, std::size_t size,
                                      std::vector<std::vector<std::string>>* reference_outcomes) {
  std::vector<BuilderOp> out;
  if (reference_outcomes) reference_outcomes->clear();
  ReferenceBook ref(BookConfig{kFuzzMaxPrice});
  std::vector<OrderId> known_ids;
  const std::size_t n = size / kRecordSize;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    BuilderOp op = decode_builder_record(data + i * kRecordSize, i, known_ids);
    EventBuffer buf;
    OpResult res;
    // As in decode(): a throwing reference book leaves partial results; a crash file still decodes.
    (void)try_apply_op(ref, op, buf, res);
    collect_acks(buf, known_ids);
    if (res.status == Status::Ok && res.id != 0) known_ids.push_back(res.id);
    if (reference_outcomes) {
      std::vector<std::string> lines;
      for (const Event& ev : buf) lines.push_back(lob::to_string(ev));
      if (!std::holds_alternative<NewOrder>(op) && !std::holds_alternative<Cancel>(op) &&
          !std::holds_alternative<Modify>(op))
        lines.push_back("-> " + describe(res));
      reference_outcomes->push_back(std::move(lines));
    }
    out.push_back(std::move(op));
  }
  return out;
}

void run_diff_builder(const std::uint8_t* data, std::size_t size) {
  OrderBook fast(BookConfig{kFuzzMaxPrice});
  ReferenceBook ref(BookConfig{kFuzzMaxPrice});
  std::vector<OrderId> known_ids;
  const std::size_t n = size / kRecordSize;
  constexpr std::size_t kAll = std::numeric_limits<std::size_t>::max();

  for (std::size_t i = 0; i < n; ++i) {
    const BuilderOp op = decode_builder_record(data + i * kRecordSize, i, known_ids);
    const std::string op_text = op_to_string(op);
    const auto levels = touched_levels(ref, op);  // before the op changes anything
    EventBuffer fast_ev, ref_ev;
    OpResult fast_res, ref_res;

    if (auto e = try_apply_op(fast, op, fast_ev, fast_res)) {
      const std::string d = "OrderBook threw: " + *e;
      OpResult ignored;
      (void)try_apply_op(ref, op, ref_ev, ignored);
      fail("OrderBook threw while applying the operation", i, op_text, fast_ev, ref_ev, d.c_str());
    }
    if (auto e = try_apply_op(ref, op, ref_ev, ref_res)) {
      const std::string d = "ReferenceBook threw: " + *e;
      fail("ReferenceBook threw while applying the operation", i, op_text, fast_ev, ref_ev, d.c_str());
    }
    if (fast_res != ref_res) {
      const std::string d = "OrderBook returned " + describe(fast_res) + "; ReferenceBook returned " + describe(ref_res);
      fail("return value mismatch", i, op_text, fast_ev, ref_ev, d.c_str());
    }
    if (fast_ev != ref_ev) fail("event mismatch", i, op_text, fast_ev, ref_ev);

    if (auto e = try_validate(fast)) {
      const std::string d = "OrderBook::validate: " + *e;
      fail("OrderBook validate() failed", i, op_text, fast_ev, ref_ev, d.c_str());
    }
    if (auto e = try_validate(ref)) {
      const std::string d = "ReferenceBook::validate: " + *e;
      fail("ReferenceBook validate() failed", i, op_text, fast_ev, ref_ev, d.c_str());
    }

    for (const Side side : {Side::Buy, Side::Sell}) {
      const std::vector<LevelView> df = fast.depth(side, kAll);
      const std::vector<LevelView> dr = ref.depth(side, kAll);
      if (df != dr) {
        std::fprintf(stderr, "lobsim fuzz: depth mismatch on the %s side\n", lob::to_string(side));
        print_depth("OrderBook depth", df);
        print_depth("ReferenceBook depth", dr);
        fail("depth mismatch", i, op_text, fast_ev, ref_ev);
      }
    }
    if (fast.resting_orders() != ref.resting_orders()) {
      const std::string d = "resting_orders: OrderBook " + std::to_string(fast.resting_orders()) +
                            ", ReferenceBook " + std::to_string(ref.resting_orders());
      fail("resting_orders mismatch", i, op_text, fast_ev, ref_ev, d.c_str());
    }
    if (has_target(op)) {
      const OrderId id = target_of(op);
      if (fast.order(id) != ref.order(id)) {
        const std::string d = "order(" + std::to_string(id) + ") differs between the books";
        fail("order() mismatch", i, op_text, fast_ev, ref_ev, d.c_str());
      }
    }
    for (const auto& [side, price] : levels) {
      const std::vector<OrderView> lf = fast.level_orders(side, price);
      const std::vector<OrderView> lr = ref.level_orders(side, price);
      if (lf != lr) {
        const std::string d = std::string("level_orders(") + lob::to_string(side) + ", " + std::to_string(price) +
                              "): OrderBook" + describe(lf) + "; ReferenceBook" + describe(lr);
        fail("level_orders mismatch", i, op_text, fast_ev, ref_ev, d.c_str());
      }
    }

    collect_acks(fast_ev, known_ids);
    if (fast_res.status == Status::Ok && fast_res.id != 0) known_ids.push_back(fast_res.id);
  }
}

std::string op_to_string(const BuilderOp& op) {
  struct Visitor {
    std::string operator()(const NewOrder& x) const { return to_string(Request{x}); }
    std::string operator()(const Cancel& x) const { return to_string(Request{x}); }
    std::string operator()(const Modify& x) const { return to_string(Request{x}); }
    std::string operator()(const InsertPassive& x) const {
      return "InsertPassive owner=" + std::to_string(x.owner) + " side=" + lob::to_string(x.side) +
             " price=" + std::to_string(x.price) + " qty=" + std::to_string(x.qty);
    }
    std::string operator()(const Reduce& x) const {
      return "Reduce id=" + std::to_string(x.order_id) + " qty=" + std::to_string(x.qty);
    }
    std::string operator()(const Remove& x) const { return "Remove id=" + std::to_string(x.order_id); }
    std::string operator()(const PreviewMatch& x) const {
      return "PreviewMatch side=" + std::string(lob::to_string(x.taker_side)) +
             " limit=" + std::to_string(x.limit) + " qty=" + std::to_string(x.qty);
    }
  };
  return std::visit(Visitor{}, op);
}

}  // namespace lob::fuzz
