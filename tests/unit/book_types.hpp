#pragma once

#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "lob/reference_book.hpp"

// Every behaviour test is a TYPED_TEST over BookTypes, so no test names a concrete book.
using BookTypes = ::testing::Types<lob::ReferenceBook, lob::OrderBook>;
