#pragma once

#include <gtest/gtest.h>

#include "lob/reference_book.hpp"

// Every behaviour test is a TYPED_TEST over BookTypes, so no test names a concrete book.
// Phase 3 adds lob::OrderBook to this list.
using BookTypes = ::testing::Types<lob::ReferenceBook>;
