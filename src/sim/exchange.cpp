#include "sim/exchange.hpp"

#include <ostream>
#include <stdexcept>
#include <variant>

#include "lob/apply.hpp"
#include "sim/order_log.hpp"

namespace sim {

lob::Request with_owner(lob::Request request, lob::OwnerId owner) {
  std::visit([owner](auto& r) { r.owner = owner; }, request);
  return request;
}

BBO top_of_book(const lob::OrderBook& book) {
  BBO top;
  if (const auto bid = book.best_level(lob::Side::Buy)) {
    top.bid = bid->price;
    top.bid_qty = bid->total_qty;
  }
  if (const auto ask = book.best_level(lob::Side::Sell)) {
    top.ask = ask->price;
    top.ask_qty = ask->total_qty;
  }
  return top;
}

Exchange::Exchange(lob::Price max_price, std::ostream* order_log)
    : max_price_(max_price), book_(lob::BookConfig{max_price}), log_(order_log) {
  out_.events.reserve(256);
  out_.public_data.reserve(64);
  if (log_ != nullptr) *log_ << format_header(max_price_) << '\n';
}

const ExchangeOutput& Exchange::process(TimeNs now, lob::OwnerId sender, const lob::Request& request) {
  const lob::Request req = with_owner(request, sender);
  if (log_ != nullptr) {
    line_.clear();
    append_request(line_, now, req);
    line_.push_back('\n');
    log_->write(line_.data(), static_cast<std::streamsize>(line_.size()));
  }
  ++requests_;

  const BBO before = top_;
  out_.events.clear();
  out_.public_data.clear();
  lob::apply(book_, req, out_.events);

  for (const lob::Event& ev : out_.events) {
    hasher_.add(ev);
    ++events_;
    if (const auto* t = std::get_if<lob::Trade>(&ev)) {
      ++trades_;
      out_.public_data.emplace_back(TradePrint{t->price, t->qty, t->aggressor_side});
    }
  }

  top_ = top_of_book(book_);
  // A crossed book is an engine bug. Checked in every build type, not only in Debug.
  if (top_.two_sided() && top_.bid >= top_.ask) throw std::logic_error("Exchange: the book is crossed");
  if (top_.two_sided()) last_mid_ = static_cast<double>(top_.bid + top_.ask) / 2.0;

  out_.top = top_;
  out_.top_changed = !(top_ == before);
  if (out_.top_changed) out_.public_data.emplace_back(top_);
  return out_;
}

}  // namespace sim
