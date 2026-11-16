// SPEC 3.2 validation of new orders: each reason, precedence, and what a Reject carries.
#include "support/book_harness.hpp"
#include "unit/book_types.hpp"

using namespace lob;
using namespace lob::test;
using enum RejectReason;

template <class B>
class ValidationTest : public ::testing::Test {};
TYPED_TEST_SUITE(ValidationTest, BookTypes);

constexpr Price kMax = 1000;

TYPED_TEST(ValidationTest, InvalidQtyForZeroAndNegative) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  for (Qty q : {Qty{0}, Qty{-1}, Qty{-100}}) {
    h.submit(limit(1, Buy, 100, q));
    EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidQty)})) << "qty " << q;
  }
  h.submit(market(1, Sell, 0));
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidQty)}));
  h.submit(ioc(1, Sell, 100, 0));
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidQty)}));
}

TYPED_TEST(ValidationTest, InvalidFlagsPostOnlyWithMarketIocOrFok) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  NewOrder po_market = market(1, Buy, 5);
  po_market.post_only = true;
  h.submit(po_market);
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidFlags)}));

  NewOrder po_ioc = post_only(1, Buy, 100, 5);
  po_ioc.tif = TIF::IOC;
  h.submit(po_ioc);
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidFlags)}));

  NewOrder po_fok = post_only(1, Buy, 100, 5);
  po_fok.tif = TIF::FOK;
  h.submit(po_fok);
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidFlags)}));
}

TYPED_TEST(ValidationTest, InvalidFlagsMarketMustBeIoc) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  NewOrder gtc = market(1, Buy, 5);
  gtc.tif = TIF::GTC;
  h.submit(gtc);
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidFlags)}));

  NewOrder fok_market = market(1, Buy, 5);
  fok_market.tif = TIF::FOK;
  h.submit(fok_market);
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidFlags)}));
}

TYPED_TEST(ValidationTest, InvalidPriceOutsideOneToMaxPrice) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  for (Price p : {Price{0}, Price{-1}, kMax + 1}) {
    h.submit(limit(1, Buy, p, 5));
    EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidPrice)})) << "price " << p;
    h.submit(ioc(1, Sell, p, 5));
    EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidPrice)})) << "price " << p;
  }
  // The boundaries themselves are valid.
  h.submit(limit(1, Buy, 1, 5));
  EXPECT_EQ(h.last(), (Events{ack(1, 1)}));
  h.submit(limit(1, Sell, kMax, 5));
  EXPECT_EQ(h.last(), (Events{ack(2, 1)}));
  EXPECT_EQ(h.book().best_bid(), 1);
  EXPECT_EQ(h.book().best_ask(), kMax);
}

TYPED_TEST(ValidationTest, PrecedenceQtyBeatsFlags) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  NewOrder o = market(1, Buy, 0);
  o.post_only = true;  // bad flags as well
  h.submit(o);
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidQty)}));
}

TYPED_TEST(ValidationTest, PrecedenceFlagsBeatPrice) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  NewOrder o = post_only(1, Buy, 0, 5);
  o.tif = TIF::IOC;  // bad flags and bad price
  h.submit(o);
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidFlags)}));
}

TYPED_TEST(ValidationTest, PrecedenceQtyBeatsPrice) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  h.submit(limit(1, Buy, 0, 0));
  EXPECT_EQ(h.last(), (Events{reject_new(1, InvalidQty)}));
}

TYPED_TEST(ValidationTest, PrecedencePriceBeatsPostOnlyWouldCross) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  h.submit(limit(1, Sell, 100, 5));  // resting ask
  h.submit(limit(1, Buy, 90, 5));    // resting bid
  h.submit(post_only(2, Buy, kMax + 1, 1));
  EXPECT_EQ(h.last(), (Events{reject_new(2, InvalidPrice)}));
  h.submit(post_only(2, Sell, 0, 1));
  EXPECT_EQ(h.last(), (Events{reject_new(2, InvalidPrice)}));
}

TYPED_TEST(ValidationTest, RejectCarriesKindClientOrderIdAndZeroOrderIdForNewOrders) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  NewOrder o = limit(4, Buy, 100, 0);
  o.client_order_id = 42;
  h.submit(o);
  EXPECT_EQ(h.last(), (Events{Reject{4, RequestKind::New, 42, 0, InvalidQty}}));
}

TYPED_TEST(ValidationTest, RejectCarriesOrderIdAndZeroClientOrderIdForCancelAndModify) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  h.cancel(4, 77);
  EXPECT_EQ(h.last(), (Events{Reject{4, RequestKind::Cancel, 0, 77, UnknownOrder}}));
  h.modify(4, 78, 100, 5);
  EXPECT_EQ(h.last(), (Events{Reject{4, RequestKind::Modify, 0, 78, UnknownOrder}}));
}

TYPED_TEST(ValidationTest, RejectedRequestsChangeNothingAndAssignNoId) {
  BookHarness<TypeParam> h(BookConfig{kMax});
  h.submit(limit(1, Sell, 100, 5));  // id1
  h.submit(limit(2, Buy, 90, 5));    // id2
  const Snapshot before = h.snapshot();

  h.submit(limit(3, Buy, 100, 0));
  NewOrder fok_market = market(3, Buy, 5);
  fok_market.tif = TIF::FOK;
  h.submit(fok_market);
  h.submit(limit(3, Buy, kMax + 1, 5));
  h.submit(post_only(3, Buy, 100, 5));
  h.cancel(3, 99);
  h.cancel(3, 1);
  h.modify(3, 1, 100, 4);
  h.modify(1, 1, 100, 5);
  EXPECT_EQ(h.snapshot(), before);

  h.submit(limit(3, Buy, 80, 1));
  EXPECT_EQ(h.last(), (Events{ack(3, 3)}));
}
