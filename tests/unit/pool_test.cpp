// The chunked object pool behind OrderBook (SPEC 3.5).
#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "lob/pool.hpp"

using lob::ObjectPool;

namespace {

struct Item {
  std::uint64_t a;
  std::int64_t b;
  Item* link;
};

}  // namespace

TEST(ObjectPool, AddressesStayValidAcrossGrowthPastSeveralChunks) {
  ObjectPool<Item, 4> pool;
  std::vector<Item*> items;
  for (std::uint64_t i = 0; i < 50; ++i) {
    Item* p = pool.create(Item{i, -static_cast<std::int64_t>(i), nullptr});
    if (!items.empty()) p->link = items.back();
    items.push_back(p);
  }
  EXPECT_GE(pool.chunks(), 13u);  // 50 objects in chunks of 4
  EXPECT_EQ(pool.capacity(), pool.chunks() * 4);
  // Every pointer handed out before the later chunks were added still reads its object.
  for (std::uint64_t i = 0; i < items.size(); ++i) {
    EXPECT_EQ(items[i]->a, i);
    EXPECT_EQ(items[i]->b, -static_cast<std::int64_t>(i));
    EXPECT_EQ(items[i]->link, i == 0 ? nullptr : items[i - 1]);
  }
}

TEST(ObjectPool, AllLiveObjectsAreDistinct) {
  ObjectPool<Item, 8> pool;
  std::set<Item*> seen;
  for (int i = 0; i < 100; ++i) EXPECT_TRUE(seen.insert(pool.create(Item{})).second);
  EXPECT_EQ(seen.size(), 100u);
}

TEST(ObjectPool, ReleasedSlotIsReusedWithoutGrowing) {
  ObjectPool<Item, 4> pool;
  Item* a = pool.create(Item{1, 1, nullptr});
  Item* b = pool.create(Item{2, 2, nullptr});
  const std::size_t chunks = pool.chunks();
  pool.destroy(a);
  Item* c = pool.create(Item{3, 3, nullptr});
  EXPECT_EQ(c, a);
  EXPECT_EQ(c->a, 3u);
  EXPECT_EQ(b->a, 2u);
  EXPECT_EQ(pool.chunks(), chunks);
}

TEST(ObjectPool, LiveCountTracksCreateAndDestroy) {
  ObjectPool<Item, 4> pool;
  EXPECT_EQ(pool.live(), 0u);
  std::vector<Item*> items;
  for (int i = 0; i < 10; ++i) items.push_back(pool.create(Item{}));
  EXPECT_EQ(pool.live(), 10u);
  for (int i = 0; i < 10; i += 2) pool.destroy(items[static_cast<std::size_t>(i)]);
  EXPECT_EQ(pool.live(), 5u);
  for (int i = 0; i < 7; ++i) pool.create(Item{});
  EXPECT_EQ(pool.live(), 12u);
  EXPECT_EQ(pool.capacity(), 12u);  // the 5 freed slots were reused: 3 chunks, not 4
}

TEST(ObjectPool, ReuseAfterChurnKeepsOtherObjectsIntact) {
  ObjectPool<Item, 4> pool;
  std::vector<Item*> live;
  for (std::uint64_t round = 0; round < 20; ++round) {
    for (std::uint64_t i = 0; i < 6; ++i) live.push_back(pool.create(Item{round * 100 + i, 0, nullptr}));
    for (std::size_t i = 0; i < 4; ++i) pool.destroy(live[i]);
    live.erase(live.begin(), live.begin() + 4);
  }
  EXPECT_EQ(pool.live(), live.size());
  std::set<Item*> distinct(live.begin(), live.end());
  EXPECT_EQ(distinct.size(), live.size());
  std::set<std::uint64_t> values;
  for (Item* p : live) values.insert(p->a);
  EXPECT_EQ(values.size(), live.size());  // no object was overwritten by a reused slot
}

#if defined(LOB_POOL_ASAN)
TEST(ObjectPool, ReleasedSlotIsPoisonedAndUnpoisonedOnReuse) {
  ObjectPool<Item, 4> pool;
  Item* a = pool.create(Item{1, 1, nullptr});
  EXPECT_FALSE(__asan_address_is_poisoned(a));
  pool.destroy(a);
  EXPECT_TRUE(__asan_address_is_poisoned(a));
  EXPECT_TRUE(__asan_address_is_poisoned(reinterpret_cast<char*>(a) + sizeof(Item) - 1));
  Item* b = pool.create(Item{2, 2, nullptr});
  ASSERT_EQ(b, a);
  EXPECT_FALSE(__asan_address_is_poisoned(b));
  EXPECT_FALSE(__asan_address_is_poisoned(reinterpret_cast<char*>(b) + sizeof(Item) - 1));
}
#endif
