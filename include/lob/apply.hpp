#pragma once

#include <variant>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob {

// Feeds one Request to any book type (D-012). Events are appended to `out`.
template <class Book>
void apply(Book& book, const Request& req, EventBuffer& out) {
  struct Dispatch {
    Book& book;
    EventBuffer& out;
    void operator()(const NewOrder& r) const { book.submit(r, out); }
    void operator()(const Cancel& r) const { book.cancel(r, out); }
    void operator()(const Modify& r) const { book.modify(r, out); }
  };
  std::visit(Dispatch{book, out}, req);
}

}  // namespace lob
