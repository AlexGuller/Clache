#include "clache/indexed_min_heap.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "clache/rng.h"

namespace clache {
namespace {

using Node = IndexedMinHeap::Node;
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

std::vector<SlotId> drain(IndexedMinHeap& heap) {
  std::vector<SlotId> order;
  while (!heap.empty()) order.push_back(heap.pop().slot);
  return order;
}

std::vector<SlotId> ascending(const IndexedMinHeap& heap, std::size_t limit = SIZE_MAX) {
  std::vector<SlotId> order;
  heap.for_each_ascending([&](const Node& n) {
    order.push_back(n.slot);
    return order.size() < limit;
  });
  return order;
}

TEST(IndexedMinHeap, EmptyHeap) {
  IndexedMinHeap heap;
  EXPECT_TRUE(heap.empty());
  EXPECT_EQ(heap.size(), 0U);
  EXPECT_FALSE(heap.contains(0));
  EXPECT_TRUE(ascending(heap).empty());
  EXPECT_TRUE(heap.check_invariants());
}

TEST(IndexedMinHeap, PopsInKeyOrder) {
  IndexedMinHeap heap;
  heap.push(0, 5.0, 0, 100);
  heap.push(1, 1.0, 0, 101);
  heap.push(2, 3.0, 0, 102);
  heap.push(3, kNegInf, 0, 103);
  EXPECT_EQ(heap.top().slot, 3U);
  EXPECT_TRUE(heap.check_invariants());
  EXPECT_EQ(drain(heap), (std::vector<SlotId>{3, 1, 2, 0}));
}

TEST(IndexedMinHeap, TiesBreakOnSeqThenId) {
  IndexedMinHeap heap;
  heap.push(0, 1.0, 7, 50);
  heap.push(1, 1.0, 3, 90);  // older access wins
  heap.push(2, 1.0, 7, 10);  // same seq as slot 0, smaller id wins
  heap.push(3, kNegInf, 9, 1);
  heap.push(4, kNegInf, 9, 0);
  EXPECT_EQ(drain(heap), (std::vector<SlotId>{4, 3, 1, 2, 0}));
}

TEST(IndexedMinHeap, UpdateMovesBothDirections) {
  IndexedMinHeap heap;
  for (SlotId s = 0; s < 5; ++s) heap.push(s, static_cast<double>(s), 0, s);
  heap.update(4, -1.0, 0);
  EXPECT_EQ(heap.top().slot, 4U);
  heap.update(4, 10.0, 0);
  EXPECT_EQ(heap.top().slot, 0U);
  EXPECT_EQ(heap.node(4).key, 10.0);
  EXPECT_TRUE(heap.check_invariants());
  EXPECT_EQ(drain(heap), (std::vector<SlotId>{0, 1, 2, 3, 4}));
}

TEST(IndexedMinHeap, EraseArbitraryPositions) {
  IndexedMinHeap heap;
  for (SlotId s = 0; s < 10; ++s) heap.push(s, static_cast<double>(9 - s), 0, s);
  heap.erase(9);  // root
  heap.erase(0);  // a leaf
  heap.erase(5);
  EXPECT_FALSE(heap.contains(9));
  EXPECT_FALSE(heap.contains(5));
  EXPECT_TRUE(heap.check_invariants());
  EXPECT_EQ(drain(heap), (std::vector<SlotId>{8, 7, 6, 4, 3, 2, 1}));
}

TEST(IndexedMinHeap, SlotCanBeReinsertedAfterErase) {
  IndexedMinHeap heap;
  heap.push(3, 1.0, 0, 3);
  heap.erase(3);
  heap.push(3, 2.0, 1, 3);
  EXPECT_TRUE(heap.contains(3));
  EXPECT_EQ(heap.node(3).key, 2.0);
}

TEST(IndexedMinHeap, ForEachAscendingDoesNotMutateAndCanStop) {
  IndexedMinHeap heap;
  for (SlotId s = 0; s < 8; ++s) heap.push(s, static_cast<double>((s * 5) % 8), 0, s);
  const auto full = ascending(heap);
  EXPECT_EQ(full.size(), 8U);
  EXPECT_EQ(ascending(heap, 3), (std::vector<SlotId>(full.begin(), full.begin() + 3)));
  EXPECT_EQ(heap.size(), 8U);
  EXPECT_TRUE(heap.check_invariants());
  EXPECT_EQ(drain(heap), full);
}

TEST(IndexedMinHeap, HeapifyAfterBulkRescore) {
  IndexedMinHeap heap;
  for (SlotId s = 0; s < 100; ++s) heap.push(s, static_cast<double>(s), 0, s);
  for (Node& n : heap.mutable_nodes()) n.key = -static_cast<double>(n.slot);
  heap.heapify();
  EXPECT_TRUE(heap.check_invariants());
  EXPECT_EQ(heap.top().slot, 99U);
  EXPECT_EQ(heap.node(42).key, -42.0);
}

TEST(IndexedMinHeap, ClearRemovesEverything) {
  IndexedMinHeap heap;
  for (SlotId s = 0; s < 4; ++s) heap.push(s, 1.0, s, s);
  heap.clear();
  EXPECT_TRUE(heap.empty());
  for (SlotId s = 0; s < 4; ++s) EXPECT_FALSE(heap.contains(s));
  EXPECT_TRUE(heap.check_invariants());
}

// Randomised comparison against an ordered-set model (Spec §19.1).
TEST(IndexedMinHeap, MatchesOrderedSetModel) {
  using Key = std::tuple<double, std::uint64_t, std::uint64_t, SlotId>;
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    Xoshiro256ss rng(seed);
    IndexedMinHeap heap;
    std::set<Key> model;
    std::map<SlotId, Key> by_slot;
    constexpr SlotId kSlots = 64;

    const auto random_key = [&] {
      // Few distinct keys so ties (and -inf) are frequent.
      const auto k = rng.uniform_below(12);
      return k == 0 ? kNegInf : static_cast<double>(k);
    };

    for (int op = 0; op < 5000; ++op) {
      const auto slot = static_cast<SlotId>(rng.uniform_below(kSlots));
      const std::uint64_t seq = rng.uniform_below(4);
      const std::uint64_t id = 1000 + slot;
      const auto action = rng.uniform_below(10);

      if (!heap.contains(slot)) {
        if (action < 7) {
          const double key = random_key();
          heap.push(slot, key, seq, id);
          const Key k{key, seq, id, slot};
          model.insert(k);
          by_slot[slot] = k;
        }
      } else if (action < 4) {
        const double key = random_key();
        heap.update(slot, key, seq);
        model.erase(by_slot[slot]);
        const Key k{key, seq, id, slot};
        model.insert(k);
        by_slot[slot] = k;
      } else if (action < 7) {
        heap.erase(slot);
        model.erase(by_slot[slot]);
        by_slot.erase(slot);
      } else if (!model.empty() && action == 9) {
        const Node top = heap.pop();
        ASSERT_EQ(top.slot, std::get<3>(*model.begin()));
        by_slot.erase(top.slot);
        model.erase(model.begin());
      }

      ASSERT_EQ(heap.size(), model.size());
      if (!model.empty()) ASSERT_EQ(heap.top().slot, std::get<3>(*model.begin()));
      if (op % 97 == 0) {
        ASSERT_TRUE(heap.check_invariants());
        std::vector<SlotId> expected;
        for (const auto& k : model) expected.push_back(std::get<3>(k));
        ASSERT_EQ(ascending(heap), expected);
      }
    }
  }
}

}  // namespace
}  // namespace clache
