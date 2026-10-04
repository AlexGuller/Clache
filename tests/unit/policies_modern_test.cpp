#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "clache/policy.h"
#include "clache/rng.h"
namespace clache {
namespace {
struct Harness {
  EngineConfig config;
  FrequencyEstimator freq{config.frequency};
  MissCostEstimator cost{config.miss_cost};
  MetadataStore meta{config.observation};
  std::unique_ptr<Policy> policy;
  std::uint64_t bytes = 0, seq = 0;
  TimePoint now{};
  std::vector<KeyId> evicted;
  Harness(std::string_view name, std::uint64_t capacity = 100, std::uint64_t seed = 42,
          PolicyConfig cfg = {}) {
    config.capacity_bytes = capacity;
    freq.set_origin(now);
    PolicyContext ctx{&meta, &freq, &cost, &config, capacity, seed, 16};
    policy = make_policy(name, cfg, ctx);
  }
  SlotId access(KeyId key, std::uint64_t size = 10) {
    now = now + Nanos(1);
    const SlotId s = meta.lookup_or_observe(key, now);
    auto& m = meta[s];
    freq.record(m.freq, now);
    m.last_access = now;
    m.last_access_seq = ++seq;
    policy->on_access(s, now);
    if (m.resident) {
      policy->on_hit(s, now);
      return s;
    }
    m.charged_bytes = size;
    cost.observe(m.cost, 1);
    if (bytes + size > config.capacity_bytes) {
      AdmissionPlan p;
      policy->plan_admission(s, bytes + size - config.capacity_bytes, now, p);
      EXPECT_TRUE(p.admit);
      EXPECT_GE(p.victim_bytes, bytes + size - config.capacity_bytes);
      std::set<SlotId> distinct;
      std::uint64_t sum = 0;
      for (auto v : p.victims) {
        EXPECT_NE(v, s);
        EXPECT_TRUE(meta[v].resident);
        EXPECT_TRUE(distinct.insert(v).second);
        sum += meta[v].charged_bytes;
        evicted.push_back(meta[v].key);
        remove(v, RemovalReason::Evicted);
      }
      EXPECT_EQ(sum, p.victim_bytes);
    }
    meta.mark_resident(s);
    bytes += size;
    policy->on_insert(s, now);
    EXPECT_TRUE(policy->check_invariants());
    return s;
  }
  void remove(SlotId s, RemovalReason reason) {
    policy->on_remove(s, reason);
    bytes -= meta[s].charged_bytes;
    meta.mark_nonresident(s, now);
  }
};
TEST(ModernPolicies, RandomizedContractAndRemoval) {
  for (auto name : {"wtinylfu", "s3fifo", "redis_lru", "redis_lfu"}) {
    Harness h(name);
    Xoshiro256ss rng(98);
    for (unsigned i = 0; i < 4000; ++i) {
      const auto key = rng.uniform_below(40);
      auto s = h.meta.find(key);
      if (i % 7 == 0 && s != kNoSlot && h.meta[s].resident)
        h.remove(s, i % 2 ? RemovalReason::Expired : RemovalReason::Invalidated);
      else
        h.access(key, 1 + rng.uniform_below(100));
      ASSERT_TRUE(h.policy->check_invariants()) << name << " step " << i;
      ASSERT_LE(h.bytes, 100U);
    }
    EXPECT_GT(h.policy->metadata_bytes(), 0U);
  }
}
std::vector<KeyId> run(std::string_view name, std::uint64_t seed) {
  Harness h(name, 100, seed);
  for (unsigned i = 0; i < 600; ++i) h.access(i % 31, 10);
  return h.evicted;
}
TEST(ModernPolicies, Deterministic) {
  for (auto name : {"wtinylfu", "s3fifo", "redis_lru", "redis_lfu"})
    EXPECT_EQ(run(name, 1), run(name, 1));
}
TEST(ModernPolicies, RedisSeedChangesSampling) {
  for (auto name : {"redis_lru", "redis_lfu"}) EXPECT_NE(run(name, 1), run(name, 2));
}
TEST(ModernPolicies, TinySketchSaturatesAndHalves) {
  Harness h("wtinylfu");
  auto s = h.access(1);
  for (unsigned i = 1; i < 159; ++i) h.access(1);
  EXPECT_EQ(h.policy->value(s, h.now), 15);
  h.access(1);
  EXPECT_EQ(h.policy->value(s, h.now), 7);
  h.access(1);
  EXPECT_EQ(h.policy->value(s, h.now), 8);
}
TEST(ModernPolicies, TinyFrequencyCompetition) {
  Harness h("wtinylfu", 30);
  h.access(1);
  h.access(2);
  h.access(3);
  h.access(1);
  h.access(4);
  ASSERT_FALSE(h.evicted.empty());
  EXPECT_EQ(h.evicted.front(), 3U);
  EXPECT_TRUE(h.meta[h.meta.find(1)].resident);
}
TEST(ModernPolicies, S3SmallPromotesAndGhostReturns) {
  Harness h("s3fifo", 30);
  h.access(1);
  h.access(2);
  h.access(3);
  h.access(1);
  h.access(4);
  ASSERT_EQ(h.evicted.size(), 1U);
  EXPECT_EQ(h.evicted[0], 2U);
  h.access(2);
  EXPECT_TRUE(h.policy->check_invariants());
  EXPECT_TRUE(h.meta[h.meta.find(1)].resident);
  EXPECT_TRUE(h.meta[h.meta.find(2)].resident);
}
TEST(ModernPolicies, RedisLfuDecayAndInitialization) {
  Harness h("redis_lfu");
  auto s = h.access(1);
  EXPECT_EQ(h.policy->value(s, h.now), 250);
  h.access(1);
  EXPECT_EQ(h.policy->value(s, h.now), 249);
  h.now = h.now + Nanos(60LL * 1'000'000'000 * 100);
  h.access(1);
  EXPECT_EQ(h.policy->value(s, h.now), 254);
}
TEST(ModernPolicies, RedisLruPrefersOldEntries) {
  PolicyConfig c;
  c.redis.samples = 100;
  Harness h("redis_lru", 100, 1, c);
  for (unsigned i = 0; i < 10; ++i) h.access(i);
  h.access(0);
  h.access(10);
  ASSERT_EQ(h.evicted.size(), 1U);
  EXPECT_EQ(h.evicted[0], 1U);
}
TEST(ModernPolicies, SlotReinsertionClearsState) {
  for (auto name : {"wtinylfu", "s3fifo", "redis_lru", "redis_lfu"}) {
    Harness h(name);
    auto s = h.access(1);
    for (unsigned i = 0; i < 20; ++i) h.access(1);
    h.remove(s, RemovalReason::Replaced);
    h.access(1, 80);
    EXPECT_TRUE(h.policy->check_invariants());
  }
}
TEST(ModernPolicies, TinyFrequentWindowWinsAndProtectedDemotes) {
  PolicyConfig c;
  c.tinylfu.window_fraction = 0.25;
  c.tinylfu.protected_fraction = 0.34;
  Harness h("wtinylfu", 40, 42, c);
  h.access(1);
  h.access(2);
  h.access(3);
  h.access(4);
  h.access(5);
  // 1 moved into probation; equally frequent 2 lost the competition.
  EXPECT_EQ(h.evicted.back(), 2U);
  h.access(1);  // probation -> protected (budget only fits one entry)
  h.access(3);
  h.access(4);
  h.access(5);
  h.access(3);
  h.access(6);
  EXPECT_TRUE(h.policy->check_invariants());
  EXPECT_TRUE(h.meta[h.meta.find(3)].resident);
}
TEST(ModernPolicies, S3ClockLowersFrequencyAndGhostIsBounded) {
  Harness h("s3fifo", 30);
  h.access(1);
  h.access(2);
  h.access(3);
  h.access(4);  // 1 is ghost
  h.access(5);  // 2 replaces 1 in the one-entry ghost
  h.access(1);  // returns to small, not main
  h.access(6);
  h.access(7);
  h.access(8);
  EXPECT_FALSE(h.meta[h.meta.find(1)].resident);
  Harness clock("s3fifo", 30);
  clock.access(1);
  clock.access(2);
  clock.access(3);
  clock.access(4);
  clock.access(1);
  auto s = clock.meta.find(1);
  clock.access(1);
  clock.access(1);
  clock.access(1);
  clock.access(1);
  EXPECT_EQ(clock.policy->value(s, clock.now), 3);
  // A full-cache object forces the main queue's CLOCK rotations.
  clock.access(9, 30);
  EXPECT_FALSE(clock.meta[s].resident);
  EXPECT_TRUE(clock.policy->check_invariants());
}
TEST(ModernPolicies, RedisLogCounterGrowsSublinearly) {
  Harness h("redis_lfu");
  auto s = h.access(1);
  for (unsigned i = 0; i < 10000; ++i) h.access(1);
  const double counter = 255 - h.policy->value(s, h.now);
  EXPECT_GT(counter, 30);
  EXPECT_LT(counter, 100);
}
TEST(ModernPolicies, SingleByteCacheAndMultiVictimPlans) {
  for (auto name : {"wtinylfu", "s3fifo", "redis_lru", "redis_lfu"}) {
    Harness tiny(name, 1);
    for (unsigned i = 0; i < 20; ++i) tiny.access(i, 1);
    Harness h(name, 100);
    for (unsigned i = 0; i < 100; ++i) h.access(i, 1);
    h.access(101, 100);
    EXPECT_EQ(h.bytes, 100U);
    EXPECT_EQ(h.evicted.size(), 100U);
    EXPECT_TRUE(h.policy->check_invariants());
  }
}

TEST(ModernPolicies, TinyFrequentWindowDisplacesMainVictim) {
  Harness h("wtinylfu", 30);
  h.access(1);
  h.access(2);
  h.access(3);
  h.access(4);
  EXPECT_EQ(h.evicted.back(), 2U);
  h.access(3);
  h.access(4);
  h.access(5);
  EXPECT_EQ(h.evicted.back(), 1U);
  EXPECT_TRUE(h.meta[h.meta.find(3)].resident);
}
TEST(ModernPolicies, RedisMaximumDecayIntervalDoesNotOverflow) {
  PolicyConfig c;
  c.redis.lfu_decay_time_min = 0xffffffffU;
  Harness h("redis_lfu", 100, 42, c);
  auto s = h.access(1);
  h.now = h.now + Nanos(120LL * 1'000'000'000);
  h.access(1);
  EXPECT_EQ(h.policy->value(s, h.now), 249);
}
}  // namespace
}  // namespace clache
