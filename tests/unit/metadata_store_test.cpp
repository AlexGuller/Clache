#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

#include "clache/metadata.h"

namespace clache {
namespace {

TEST(MetadataStore, TouchControlsLruAndTrimPreservesResidents) {
  MetadataStore store({2, 1200});
  auto a = store.lookup_or_observe(1, TimePoint{});
  auto b = store.lookup_or_observe(2, TimePoint{});
  auto resident = store.lookup_or_observe(3, TimePoint{});
  store.mark_resident(resident);
  EXPECT_EQ(store.lookup_or_observe(1, TimePoint{}), a);
  store.lookup_or_observe(4, TimePoint{});
  EXPECT_EQ(store.observed_count(), 3U);
  store.trim();
  EXPECT_EQ(store.find(2), kNoSlot);
  EXPECT_EQ(store.find(1), a);
  EXPECT_EQ(store.find(3), resident);
  EXPECT_EQ(store.resident_count(), 1U);
  EXPECT_EQ(store.observed_count(), 2U);
  EXPECT_TRUE(store.check_invariants());
  (void)b;
}

TEST(MetadataStore, TelemetryResetsAtTtlBoundaryButGenerationSurvives) {
  MetadataStore store({10, 2});
  const auto slot = store.lookup_or_observe(1, TimePoint::from_ns(10));
  store[slot].freq.recorded = true;
  store[slot].freq.buckets[0] = 3;
  store[slot].cost = {5, 4};
  store[slot].access_count = 8;
  store[slot].generation = 7;
  store.lookup_or_observe(1, TimePoint::from_ns(2'000'000'009));
  EXPECT_EQ(store[slot].access_count, 8U);
  store.lookup_or_observe(1, TimePoint::from_ns(2'000'000'010));
  EXPECT_FALSE(store[slot].freq.recorded);
  EXPECT_EQ(store[slot].cost.samples, 0U);
  EXPECT_EQ(store[slot].access_count, 0U);
  EXPECT_EQ(store[slot].generation, 7U);
  EXPECT_TRUE(store.check_invariants());
}

TEST(MetadataStore, ResidentTelemetryDoesNotExpireAndGhostTransitionKeepsIt) {
  MetadataStore store({10, 1});
  const auto slot = store.lookup_or_observe(1, TimePoint{});
  store[slot].cost = {4, 2};
  store[slot].access_count = 10;
  store.mark_resident(slot);
  store.lookup_or_observe(1, TimePoint::from_ns(2'000'000'000));
  EXPECT_EQ(store[slot].access_count, 10U);
  store.mark_nonresident(slot, TimePoint::from_ns(2'000'000'000));
  EXPECT_EQ(store[slot].cost.samples, 2U);
  EXPECT_EQ(store[slot].access_count, 10U);
  EXPECT_EQ(store.resident_count(), 0U);
  EXPECT_EQ(store.observed_count(), 1U);
  EXPECT_TRUE(store.check_invariants());
}

TEST(MetadataStore, RecyclingUsesLifoAndFreshMetadata) {
  MetadataStore store({0, 1200});
  const auto a = store.lookup_or_observe(1, TimePoint{});
  const auto b = store.lookup_or_observe(2, TimePoint{});
  store[a].generation = 9;
  store[b].cost = {100, 8};
  store.trim();
  EXPECT_EQ(store.tracked_count(), 0U);
  EXPECT_EQ(store.lookup_or_observe(3, TimePoint{}), b);
  EXPECT_EQ(store.lookup_or_observe(4, TimePoint{}), a);
  EXPECT_EQ(store[a].generation, 0U);
  EXPECT_EQ(store[b].cost.samples, 0U);
  EXPECT_TRUE(store.check_invariants());
}

TEST(MetadataStore, PinSurvivesTrimAndStaleCleanupUntilFinalUnpin) {
  MetadataStore store({0, 1});
  const auto pinned = store.lookup_or_observe(1, TimePoint{});
  store.pin(pinned);
  store.pin(pinned);
  store.lookup_or_observe(2, TimePoint::from_ns(2'000'000'000));
  store.trim();
  EXPECT_EQ(store.find(1), pinned);
  EXPECT_EQ(store.find(2), kNoSlot);
  store.unpin(pinned);
  store.trim();
  EXPECT_EQ(store.find(1), pinned);
  store.unpin(pinned);
  store.trim();
  EXPECT_EQ(store.find(1), kNoSlot);
  EXPECT_TRUE(store.check_invariants());
}

TEST(MetadataStore, TrimSkipsPinnedTailAndDropsNextEligibleLru) {
  MetadataStore store({2, 1200});
  const auto pinned = store.lookup_or_observe(1, TimePoint{});
  store.pin(pinned);
  store.lookup_or_observe(2, TimePoint{});
  const auto newest = store.lookup_or_observe(3, TimePoint{});
  store.trim();
  EXPECT_EQ(store.find(1), pinned);
  EXPECT_EQ(store.find(2), kNoSlot);
  EXPECT_EQ(store.find(3), newest);
  EXPECT_EQ(store.observed_count(), 2U);
  EXPECT_TRUE(store.check_invariants());
}

TEST(MetadataStore, CleanupDropsAtMostTwoAndNeverReturnedSlot) {
  MetadataStore store({10, 1});
  for (KeyId key = 1; key <= 4; ++key) store.lookup_or_observe(key, TimePoint{});
  auto slot = store.lookup_or_observe(5, TimePoint::from_ns(1'000'000'000));
  EXPECT_EQ(store.tracked_count(), 3U);
  EXPECT_EQ(store.find(1), kNoSlot);
  EXPECT_EQ(store.find(2), kNoSlot);
  EXPECT_EQ(store.find(5), slot);
  EXPECT_TRUE(store.check_invariants());
  MetadataStore zero_ttl({0, 0});
  slot = zero_ttl.lookup_or_observe(1, TimePoint{});
  EXPECT_EQ(zero_ttl.find(1), slot);
  zero_ttl.trim();
  EXPECT_EQ(zero_ttl.find(1), kNoSlot);
}

TEST(MetadataStore, TtlArithmeticHandlesExtremeTimes) {
  MetadataStore store({10, 1});
  auto slot = store.lookup_or_observe(1, TimePoint::from_ns(std::numeric_limits<int64_t>::min()));
  store[slot].access_count = 10;
  store.lookup_or_observe(1, TimePoint::from_ns(std::numeric_limits<int64_t>::max()));
  EXPECT_EQ(store[slot].access_count, 0U);
  EXPECT_TRUE(store.check_invariants());
}

TEST(MetadataStore, ContractsAndAccounting) {
  MetadataStore store({2, 1200});
  store.reserve(10);
  EXPECT_GE(store.metadata_bytes(), 10U * sizeof(EntryMetadata));
  const auto slot = store.lookup_or_observe(1, TimePoint{});
  EXPECT_THROW(store.unpin(slot), std::logic_error);
  EXPECT_THROW(store[kNoSlot], std::logic_error);
  store[slot].pins = std::numeric_limits<std::uint16_t>::max();
  EXPECT_THROW(store.pin(slot), std::logic_error);
  store[slot].pins = 0;
  store.mark_resident(slot);
  store.mark_resident(slot);
  EXPECT_EQ(store.resident_count(), 1U);
  store.mark_nonresident(slot, TimePoint{});
  store.mark_nonresident(slot, TimePoint{});
  EXPECT_EQ(store.observed_count(), 1U);
  EXPECT_TRUE(store.check_invariants());
  store[slot].obs_next = slot;
  EXPECT_FALSE(store.check_invariants());
}

}  // namespace
}  // namespace clache
