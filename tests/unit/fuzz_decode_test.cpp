// The byte format of upgrade 1, 3.1, checked on hand-built records.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "diff_driver.hpp"

namespace {

using namespace lob;
using Rec = std::array<std::uint8_t, 8>;

Rec rec(std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, std::uint8_t b3, std::uint8_t b4, std::uint8_t b5,
        std::uint8_t b6 = 0, std::uint8_t b7 = 0) {
  return {b0, b1, b2, b3, b4, b5, b6, b7};
}

Request dec(const Rec& r, const std::vector<OrderId>& known = {}, std::uint64_t index = 0) {
  return fuzz::decode_record(r.data(), index, known);
}

NewOrder as_new(const Request& r) { return std::get<NewOrder>(r); }

TEST(FuzzDecode, WorkedExample) {
  const std::vector<std::uint8_t> bytes = {0x00, 0x00, 0x00, 0x64, 0x00, 0x05, 0x00, 0x00,
                                           0x00, 0x01, 0x01, 0x64, 0x00, 0x03, 0x00, 0x00};
  std::vector<EventBuffer> events;
  const std::vector<Request> reqs = fuzz::decode(bytes.data(), bytes.size(), &events);

  NewOrder buy;
  buy.owner = 1;
  buy.client_order_id = 0;
  buy.side = Side::Buy;
  buy.price = 96;
  buy.qty = 6;
  NewOrder sell;
  sell.owner = 2;
  sell.client_order_id = 1;
  sell.side = Side::Sell;
  sell.price = 96;
  sell.qty = 4;
  ASSERT_EQ(reqs.size(), 2u);
  EXPECT_EQ(reqs[0], Request{buy});
  EXPECT_EQ(reqs[1], Request{sell});

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], (EventBuffer{Ack{1, 1, 0}}));
  EXPECT_EQ(events[1], (EventBuffer{Ack{2, 2, 1}, Trade{1, 96, 4, 1, 2, 1, 2, Side::Sell}}));

  fuzz::run_diff(bytes.data(), bytes.size());
}

TEST(FuzzDecode, KindIsB0Mod3) {
  EXPECT_TRUE(std::holds_alternative<NewOrder>(dec(rec(0, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<Cancel>(dec(rec(1, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<Modify>(dec(rec(2, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<NewOrder>(dec(rec(3, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<Cancel>(dec(rec(250, 0, 0, 0, 0, 0))));  // 250 % 3 == 1
  EXPECT_TRUE(std::holds_alternative<NewOrder>(dec(rec(255, 0, 0, 0, 0, 0))));  // 255 % 3 == 0
  EXPECT_TRUE(std::holds_alternative<Modify>(dec(rec(254, 0, 0, 0, 0, 0))));    // 254 % 3 == 2
}

TEST(FuzzDecode, OwnerIs1PlusB1Mod3) {
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0, 0, 0))).owner, 1u);
  EXPECT_EQ(as_new(dec(rec(0, 1, 0, 0, 0, 0))).owner, 2u);
  EXPECT_EQ(as_new(dec(rec(0, 2, 0, 0, 0, 0))).owner, 3u);
  EXPECT_EQ(as_new(dec(rec(0, 3, 0, 0, 0, 0))).owner, 1u);
  EXPECT_EQ(as_new(dec(rec(0, 255, 0, 0, 0, 0))).owner, 1u);  // 255 % 3 == 0
  EXPECT_EQ(std::get<Cancel>(dec(rec(1, 5, 0, 0, 0, 0))).owner, 3u);
  EXPECT_EQ(std::get<Modify>(dec(rec(2, 4, 0, 0, 0, 0))).owner, 2u);
}

TEST(FuzzDecode, FlagBits) {
  const NewOrder base = as_new(dec(rec(0, 0, 0x00, 0, 0, 0)));
  EXPECT_EQ(base.side, Side::Buy);
  EXPECT_FALSE(base.post_only);
  EXPECT_EQ(base.stp, STP::CancelTaking);
  EXPECT_EQ(base.type, OrderType::Limit);
  EXPECT_EQ(base.tif, TIF::GTC);

  EXPECT_EQ(as_new(dec(rec(0, 0, 0x01, 0, 0, 0))).side, Side::Sell);
  EXPECT_TRUE(as_new(dec(rec(0, 0, 0x02, 0, 0, 0))).post_only);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0x04, 0, 0, 0))).stp, STP::CancelResting);
  // Only bit 0 is the side, only bit 1 the post-only flag, only bit 2 the STP mode.
  const NewOrder only_others = as_new(dec(rec(0, 0, 0x04 | 0x02, 0, 0, 0)));
  EXPECT_EQ(only_others.side, Side::Buy);
  EXPECT_TRUE(only_others.post_only);
  EXPECT_EQ(only_others.stp, STP::CancelResting);
}

TEST(FuzzDecode, OrderTypes) {
  const NewOrder gtc = as_new(dec(rec(0, 0, 0x00, 0, 0, 0)));
  EXPECT_EQ(gtc.type, OrderType::Limit);
  EXPECT_EQ(gtc.tif, TIF::GTC);
  const NewOrder ioc = as_new(dec(rec(0, 0, 0x08, 0, 0, 0)));
  EXPECT_EQ(ioc.type, OrderType::Limit);
  EXPECT_EQ(ioc.tif, TIF::IOC);
  const NewOrder fok = as_new(dec(rec(0, 0, 0x10, 0, 0, 0)));
  EXPECT_EQ(fok.type, OrderType::Limit);
  EXPECT_EQ(fok.tif, TIF::FOK);
  const NewOrder mkt = as_new(dec(rec(0, 0, 0x18, 0, 0, 0)));
  EXPECT_EQ(mkt.type, OrderType::Market);
  EXPECT_EQ(mkt.tif, TIF::IOC);
}

TEST(FuzzDecode, BadTifOnlyAffectsMarket) {
  const NewOrder mkt = as_new(dec(rec(0, 0, 0x18 | 0x80, 0, 0, 0)));
  EXPECT_EQ(mkt.type, OrderType::Market);
  EXPECT_EQ(mkt.tif, TIF::GTC);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0x00 | 0x80, 0, 0, 0))).tif, TIF::GTC);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0x08 | 0x80, 0, 0, 0))).tif, TIF::IOC);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0x10 | 0x80, 0, 0, 0))).tif, TIF::FOK);
}

TEST(FuzzDecode, PriceBand) {
  // 95 + (uint16_t)p16 % 11
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0x00, 0x00, 0))).price, 95);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0x64, 0x00, 0))).price, 96);   // 100 % 11 == 1
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0x0A, 0x00, 0))).price, 105);  // 10
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0x0B, 0x00, 0))).price, 95);   // 11
  // 0xFFFF is taken as unsigned (65535 % 11 == 8); as the signed -1 the band would be missed.
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0xFF, 0xFF, 0))).price, 103);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0x00, 0x80, 0))).price, 95 + 32768 % 11);
}

TEST(FuzzDecode, RawPriceIsSignedInt16) {
  const std::uint8_t raw = 0x20;
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0xFF, 0xFF, 0))).price, -1);
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0x00, 0x00, 0))).price, 0);
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0x64, 0x00, 0))).price, 100);
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0xFF, 0x7F, 0))).price, 32767);
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0x00, 0x80, 0))).price, -32768);
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0xE8, 0x03, 0))).price, 1000);
}

TEST(FuzzDecode, QtyBand) {
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0, 0, 0))).qty, 1);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0, 0, 5))).qty, 6);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0, 0, 9))).qty, 10);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0, 0, 10))).qty, 1);
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0, 0, 255))).qty, 1 + 255 % 10);
}

TEST(FuzzDecode, RawQtyIsSignedInt8) {
  const std::uint8_t raw = 0x40;
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0, 0, 0))).qty, 0);
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0, 0, 0xFF))).qty, -1);
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0, 0, 127))).qty, 127);
  EXPECT_EQ(as_new(dec(rec(0, 0, raw, 0, 0, 0x80))).qty, -128);
}

TEST(FuzzDecode, ModifyUsesTheSamePriceAndQtyRules) {
  const Modify band = std::get<Modify>(dec(rec(2, 0, 0x00, 0x64, 0x00, 0x05)));
  EXPECT_EQ(band.new_price, 96);
  EXPECT_EQ(band.new_qty, 6);
  const Modify raw = std::get<Modify>(dec(rec(2, 0, 0x60, 0xFF, 0xFF, 0x80)));
  EXPECT_EQ(raw.new_price, -1);
  EXPECT_EQ(raw.new_qty, -128);
}

TEST(FuzzDecode, ClientOrderIdIsTheRecordIndex) {
  EXPECT_EQ(as_new(dec(rec(0, 0, 0, 0, 0, 0), {}, 41)).client_order_id, 41u);
}

TEST(FuzzDecode, TargetWithNoKnownIdsIsTUnchanged) {
  EXPECT_EQ(std::get<Cancel>(dec(rec(1, 0, 0, 0, 0, 0, 0x34, 0x12))).order_id, 0x1234u);
  EXPECT_EQ(std::get<Modify>(dec(rec(2, 0, 0, 0, 0, 0, 0x03, 0x00))).order_id, 3u);
}

TEST(FuzzDecode, TargetWithTMod8Is7IsTUnchanged) {
  const std::vector<OrderId> known = {10, 20, 30};
  EXPECT_EQ(std::get<Cancel>(dec(rec(1, 0, 0, 0, 0, 0, 7, 0), known)).order_id, 7u);
  EXPECT_EQ(std::get<Modify>(dec(rec(2, 0, 0, 0, 0, 0, 0x0F, 0x01), known)).order_id, 0x10Fu);  // 271 % 8 == 7
}

TEST(FuzzDecode, TargetIsKnownIdAtTModSize) {
  const std::vector<OrderId> known = {10, 20, 30};
  EXPECT_EQ(std::get<Cancel>(dec(rec(1, 0, 0, 0, 0, 0, 0, 0), known)).order_id, 10u);
  EXPECT_EQ(std::get<Cancel>(dec(rec(1, 0, 0, 0, 0, 0, 1, 0), known)).order_id, 20u);
  EXPECT_EQ(std::get<Cancel>(dec(rec(1, 0, 0, 0, 0, 0, 2, 0), known)).order_id, 30u);
  EXPECT_EQ(std::get<Cancel>(dec(rec(1, 0, 0, 0, 0, 0, 3, 0), known)).order_id, 10u);
  EXPECT_EQ(std::get<Modify>(dec(rec(2, 0, 0, 0, 0, 0, 0x00, 0x01), known)).order_id, 20u);  // 256 % 3 == 1
}

TEST(FuzzDecode, TrailingBytesAreIgnored) {
  std::vector<std::uint8_t> bytes(8, 0);
  EXPECT_EQ(fuzz::decode(bytes.data(), bytes.size()).size(), 1u);
  bytes.resize(8 + 7, 0xFF);
  EXPECT_EQ(fuzz::decode(bytes.data(), bytes.size()).size(), 1u);
  bytes.resize(8 + 8, 0);
  EXPECT_EQ(fuzz::decode(bytes.data(), bytes.size()).size(), 2u);
  fuzz::run_diff(bytes.data(), 8 + 7);
}

TEST(FuzzDecode, EmptyInput) {
  std::vector<EventBuffer> events{EventBuffer{}};
  EXPECT_TRUE(fuzz::decode(nullptr, 0, &events).empty());
  EXPECT_TRUE(events.empty());
  const std::uint8_t one = 0;
  EXPECT_TRUE(fuzz::decode(&one, 0).empty());
  EXPECT_TRUE(fuzz::decode(&one, 1).empty());
  fuzz::run_diff(nullptr, 0);
  fuzz::run_diff(&one, 1);
}

TEST(FuzzDecode, KnownIdsComeFromAcksOfTheReferenceBook) {
  // New (Ack 1), then a Cancel whose target is known_ids[0] = 1.
  const std::vector<std::uint8_t> bytes = {0, 0, 0, 0x64, 0, 5, 0, 0,   //
                                           1, 0, 0, 0,    0, 0, 0, 0};  // t = 0 -> known_ids[0]
  std::vector<EventBuffer> events;
  const std::vector<Request> reqs = fuzz::decode(bytes.data(), bytes.size(), &events);
  ASSERT_EQ(reqs.size(), 2u);
  EXPECT_EQ(std::get<Cancel>(reqs[1]).order_id, 1u);
  ASSERT_EQ(events[1].size(), 1u);
  EXPECT_TRUE(std::holds_alternative<Cancelled>(events[1][0]));
  fuzz::run_diff(bytes.data(), bytes.size());
}

TEST(FuzzDecode, ToString) {
  NewOrder n;
  n.owner = 1;
  n.side = Side::Buy;
  n.price = 96;
  n.qty = 6;
  EXPECT_EQ(fuzz::to_string(n), "New owner=1 side=Buy type=Limit tif=GTC price=96 qty=6 post_only=0 stp=CancelTaking");
  EXPECT_EQ(fuzz::to_string(Cancel{2, 5}), "Cancel owner=2 id=5");
  EXPECT_EQ(fuzz::to_string(Modify{2, 1, 97, 4}), "Modify owner=2 id=1 price=97 qty=4");
}

using fuzz::BuilderOp;
using fuzz::InsertPassive;
using fuzz::PreviewMatch;
using fuzz::Reduce;
using fuzz::Remove;

BuilderOp dec_b(const Rec& r, const std::vector<OrderId>& known = {}, std::uint64_t index = 0) {
  return fuzz::decode_builder_record(r.data(), index, known);
}

TEST(FuzzDecodeBuilder, KindIsB0Mod7AndKinds0To2MatchTheOriginalFormat) {
  EXPECT_TRUE(std::holds_alternative<NewOrder>(dec_b(rec(7, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<Cancel>(dec_b(rec(8, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<Modify>(dec_b(rec(9, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<InsertPassive>(dec_b(rec(3, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<Reduce>(dec_b(rec(4, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<Remove>(dec_b(rec(5, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<PreviewMatch>(dec_b(rec(6, 0, 0, 0, 0, 0))));
  EXPECT_TRUE(std::holds_alternative<PreviewMatch>(dec_b(rec(13, 0, 0, 0, 0, 0))));  // 13 % 7 == 6

  const std::vector<OrderId> known = {4, 9};
  for (unsigned kind = 0; kind < 3; ++kind) {
    const Rec r = rec(static_cast<std::uint8_t>(kind), 2, 0x69, 0x34, 0x12, 0x83, 0x01, 0x00);
    const BuilderOp b = dec_b(r, known, 5);
    const Request q = dec(r, known, 5);
    std::visit([&](const auto& x) { EXPECT_EQ(b, BuilderOp{x}); }, q);
  }
}

TEST(FuzzDecodeBuilder, BuilderOperationsUseTheSharedFieldRules) {
  const std::vector<OrderId> known = {4, 9};
  // Sell, band price 95 + 3, band qty 1 + 4, owner 3; t = 1 -> known[1].
  EXPECT_EQ(dec_b(rec(3, 2, 0x01, 3, 0, 4, 1, 0), known), (BuilderOp{InsertPassive{Side::Sell, 98, 5, 3}}));
  // raw qty -3 (0xFD), target t = 0 -> known[0]
  EXPECT_EQ(dec_b(rec(4, 0, 0x40, 0, 0, 0xFD, 0, 0), known), (BuilderOp{Reduce{4, -3}}));
  EXPECT_EQ(dec_b(rec(5, 0, 0, 0, 0, 0, 1, 0), known), (BuilderOp{Remove{9}}));
  EXPECT_EQ(dec_b(rec(5, 0, 0, 0, 0, 0, 7, 0), known), (BuilderOp{Remove{7}}));  // t % 8 == 7 is the raw id
  // raw price 0x0140 = 320, Buy taker, qty 10
  EXPECT_EQ(dec_b(rec(6, 0, 0x20, 0x40, 0x01, 9, 0, 0), known), (BuilderOp{PreviewMatch{Side::Buy, 320, 10}}));
}

TEST(FuzzDecodeBuilder, ToStringAndDecodeBuilderOutcomes) {
  EXPECT_EQ(fuzz::op_to_string(InsertPassive{Side::Sell, 98, 5, 3}),
            "InsertPassive owner=3 side=Sell price=98 qty=5");
  EXPECT_EQ(fuzz::op_to_string(Reduce{4, 2}), "Reduce id=4 qty=2");
  EXPECT_EQ(fuzz::op_to_string(Remove{4}), "Remove id=4");
  EXPECT_EQ(fuzz::op_to_string(PreviewMatch{Side::Buy, 101, 7}), "PreviewMatch side=Buy limit=101 qty=7");
  EXPECT_EQ(fuzz::op_to_string(Cancel{2, 5}), "Cancel owner=2 id=5");

  // insert_passive(Buy 96 x 6), then a Remove aimed at it (t = 0 -> known[0] = 1).
  const std::vector<std::uint8_t> bytes = {3, 0, 0, 0x64, 0, 5, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0};
  std::vector<std::vector<std::string>> outcomes;
  const std::vector<BuilderOp> ops = fuzz::decode_builder(bytes.data(), bytes.size(), &outcomes);
  ASSERT_EQ(ops.size(), 2u);
  EXPECT_EQ(ops[0], (BuilderOp{InsertPassive{Side::Buy, 96, 6, 1}}));
  EXPECT_EQ(ops[1], (BuilderOp{Remove{1}}));
  ASSERT_EQ(outcomes.size(), 2u);
  EXPECT_EQ(outcomes[0], (std::vector<std::string>{"-> status=Ok id=1 fills=[]"}));
  EXPECT_EQ(outcomes[1], (std::vector<std::string>{"-> status=Ok id=0 fills=[]"}));
  fuzz::run_diff_builder(bytes.data(), bytes.size());
}

}  // namespace
