#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "clache/policy.h"
#include "clache/rng.h"

namespace clache {
namespace {

struct ClassicFixture {
  EngineConfig config;
  FrequencyEstimator freq{config.frequency};
  MissCostEstimator cost{config.miss_cost};
  MetadataStore meta{config.observation};
  std::unique_ptr<Policy> policy;
  std::uint64_t seq = 0;
  TimePoint now{};

  explicit ClassicFixture(std::string_view name, PolicyConfig pc = {}) {
    config.capacity_bytes = 100;
    freq.set_origin({});
    policy = make_policy(name, pc, PolicyContext{&meta, &freq, &cost, &config, 100, 0, 10});
  }
  SlotId prepare(KeyId key, std::uint64_t bytes = 10, double price = 1) {
    const SlotId slot = meta.lookup_or_observe(key, now);
    auto& m = meta[slot];
    freq.record(m.freq, now);
    m.last_access_seq = ++seq;
    m.last_access = now;
    m.charged_bytes = bytes;
    if (!m.resident) cost.observe(m.cost, price);
    policy->on_access(slot, now);
    return slot;
  }
  SlotId insert(KeyId key, std::uint64_t bytes = 10, double price = 1) {
    const auto slot = prepare(key, bytes, price);
    meta.mark_resident(slot);
    policy->on_insert(slot, now);
    EXPECT_TRUE(policy->check_invariants());
    return slot;
  }
  void hit(SlotId slot) {
    prepare(meta[slot].key, meta[slot].charged_bytes);
    policy->on_hit(slot, now);
    EXPECT_TRUE(policy->check_invariants());
  }
  void remove(SlotId slot, RemovalReason reason) {
    policy->on_remove(slot, reason);
    meta.mark_nonresident(slot, now);
    EXPECT_TRUE(policy->check_invariants());
  }
  AdmissionPlan plan(SlotId candidate, std::uint64_t bytes) {
    AdmissionPlan p;
    policy->plan_admission(candidate, bytes, now, p);
    EXPECT_TRUE(p.admit);
    EXPECT_EQ(p.decision, Decision::AdmitWithEviction);
    std::set<SlotId> seen;
    std::uint64_t total = 0;
    for (auto v : p.victims) {
      EXPECT_NE(v, candidate);
      EXPECT_TRUE(meta[v].resident);
      EXPECT_TRUE(seen.insert(v).second);
      total += meta[v].charged_bytes;
    }
    EXPECT_EQ(total, p.victim_bytes);
    EXPECT_GE(total, bytes);
    if (!p.victims.empty()) {
      EXPECT_LT(total - meta[p.victims.back()].charged_bytes, bytes);
    }
    EXPECT_TRUE(policy->check_invariants());
    return p;
  }
};

TEST(ClassicPolicies, FrequencyAndLruTieBreak) {
  for (const auto name : {"lfu", "lfu_da", "gdsf"}) {
    ClassicFixture f(name);
    auto a = f.insert(10);
    auto b = f.insert(20);
    auto c = f.insert(30);
    f.hit(a);
    auto d = f.prepare(40);
    EXPECT_EQ(f.plan(d, 20).victims, (std::vector<SlotId>{b, c}));
    EXPECT_EQ(f.plan(d, 30).victims, (std::vector<SlotId>{b, c, a}));
  }
}

TEST(ClassicPolicies, KeyBreaksEqualSequenceTies) {
  ClassicFixture f("lfu");
  f.seq = 0;
  auto a = f.insert(20);
  f.seq = 0;
  auto b = f.insert(10);
  EXPECT_EQ(f.plan(f.prepare(30), 10).victims, (std::vector<SlotId>{b}));
  EXPECT_NE(a, b);
}

TEST(ClassicPolicies, AgingOnlyOnEvictionAndCountsReset) {
  for (const auto name : {"lfu_da", "gds", "gdsf", "gdsf_window"}) {
    for (auto reason : {RemovalReason::Expired, RemovalReason::Invalidated, RemovalReason::Replaced,
                        RemovalReason::Evicted}) {
      ClassicFixture f(name);
      const auto a = f.insert(1);
      const double initial = f.policy->value(a, f.now);
      f.remove(a, reason);
      const auto b = f.insert(2);
      EXPECT_DOUBLE_EQ(f.policy->value(b, f.now),
                       reason == RemovalReason::Evicted ? 2 * initial : initial);
    }
  }
  ClassicFixture f("lfu");
  auto a = f.insert(1);
  f.hit(a);
  f.remove(a, RemovalReason::Evicted);
  f.insert(1);
  EXPECT_DOUBLE_EQ(f.policy->value(a, f.now), 1);
}

TEST(ClassicPolicies, GdsCostSizeAndCostUpdates) {
  ClassicFixture f("gds");
  const auto a = f.insert(1, 20, 1);
  const auto b = f.insert(2, 10, 1);
  const auto c = f.prepare(3);
  EXPECT_EQ(f.plan(c, 10).victims, (std::vector<SlotId>{a}));
  f.cost.observe(f.meta[a].cost, 100);
  f.policy->on_cost_update(a, f.now);
  EXPECT_EQ(f.plan(c, 10).victims, (std::vector<SlotId>{b}));
  PolicyConfig pc;
  pc.gdsf.size_exponent = 0;
  ClassicFixture no_size("gds", pc);
  const auto large = no_size.insert(1, 20, 2);
  const auto small = no_size.insert(2, 10, 1.5);
  EXPECT_EQ(no_size.plan(no_size.prepare(3), 10).victims, (std::vector<SlotId>{small}));
  EXPECT_DOUBLE_EQ(no_size.policy->value(large, {}), 2);
}

TEST(ClassicPolicies, WindowUsesObservationHistoryAndHonorsConfiguration) {
  for (const auto name : {"gdsf", "gdsf_window"}) {
    PolicyConfig pc;
    pc.gdsf.frequency_source = GdsfPolicyConfig::FrequencySource::EngineWindow;
    ClassicFixture f(name, pc);
    f.prepare(1);
    f.prepare(1);
    auto a = f.insert(1);
    auto b = f.insert(2);
    EXPECT_NEAR(f.policy->value(a, {}), 3.0 / 300 / 10, 1e-15);
    EXPECT_EQ(f.plan(f.prepare(3), 10).victims, (std::vector<SlotId>{b}));
    f.now = TimePoint{Nanos{400'000'000'000}};
    f.hit(a);
    EXPECT_NEAR(f.policy->value(a, f.now), 1.0 / 300 / 10, 1e-15);
  }
  ClassicFixture f("gdsf");
  f.prepare(1);
  f.prepare(1);
  auto a = f.insert(1);
  f.insert(2);
  EXPECT_EQ(f.plan(f.prepare(3), 10).victims, (std::vector<SlotId>{a}));
}

TEST(ClassicPolicies, PlanningIsPureAndInsertionUsesFinalAge) {
  ClassicFixture f("lfu_da");
  auto a = f.insert(1);
  auto b = f.insert(2);
  f.hit(b);
  auto c = f.prepare(3);
  auto p = f.plan(c, 20);
  EXPECT_EQ(p.victims, f.plan(c, 20).victims);
  EXPECT_DOUBLE_EQ(p.candidate_value, 1);
  EXPECT_DOUBLE_EQ(p.threshold_value, 2);
  for (auto v : p.victims) f.remove(v, RemovalReason::Evicted);
  f.meta.mark_resident(c);
  f.policy->on_insert(c, {});
  EXPECT_DOUBLE_EQ(f.policy->value(c, {}), 3);
  EXPECT_TRUE(f.policy->check_invariants());
  EXPECT_GT(f.policy->metadata_bytes(), 0U);
  EXPECT_NE(a, b);
}

std::vector<KeyId> random_run(std::string_view name) {
  ClassicFixture f(name);
  Xoshiro256ss rng(7);
  std::uint64_t used = 0;
  std::vector<KeyId> decisions;
  for (int i = 0; i < 2000; ++i) {
    const auto key = rng.uniform_below(50);
    const auto slot = f.meta.find(key);
    if (slot != kNoSlot && f.meta[slot].resident) {
      if (rng.uniform_below(5) == 0) {
        used -= f.meta[slot].charged_bytes;
        f.remove(slot, RemovalReason::Invalidated);
      } else {
        f.hit(slot);
      }
    } else {
      const auto bytes = 1 + rng.uniform_below(40);
      const auto c = f.prepare(key, bytes, static_cast<double>(1 + rng.uniform_below(100)));
      if (used + bytes > 100) {
        const auto p = f.plan(c, used + bytes - 100);
        for (auto v : p.victims) {
          decisions.push_back(f.meta[v].key);
          used -= f.meta[v].charged_bytes;
          f.remove(v, RemovalReason::Evicted);
        }
      }
      f.meta.mark_resident(c);
      f.policy->on_insert(c, f.now);
      used += bytes;
    }
    EXPECT_LE(used, 100U);
    EXPECT_TRUE(f.policy->check_invariants());
  }
  return decisions;
}

TEST(ClassicPolicies, RandomMixedSizesContractAndDeterminism) {
  for (const auto name : {"lfu", "lfu_da", "gds", "gdsf", "gdsf_window"}) {
    EXPECT_EQ(random_run(name), random_run(name));
  }
}

}  // namespace
}  // namespace clache
