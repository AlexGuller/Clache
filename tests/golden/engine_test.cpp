#include "clache/engine.h"

#include <limits>

#include <gtest/gtest.h>

namespace clache {
namespace {
TimePoint at(int second) {
  return TimePoint::from_ns(static_cast<std::int64_t>(second) * 1000000000);
}
EngineConfig config() {
  EngineConfig c;
  c.capacity_bytes = 100;
  return c;
}
AdmissionResult load(CacheEngine& e, KeyId key, std::uint64_t bytes, int time,
                     std::optional<Nanos> ttl = std::nullopt) {
  auto lookup = e.lookup(key, at(time), bytes);
  EXPECT_FALSE(lookup.hit);
  return e.complete_miss(key, bytes, 10, ttl, lookup.generation, at(time));
}
TEST(EngineGolden, G4Oversized) {
  auto c = config();
  c.max_object_bytes = 80;
  CacheEngine e(c, "lru", {});
  EXPECT_EQ(load(e, 1, 90, 1).decision, Decision::RejectOversized);
  EXPECT_EQ(load(e, 1, 90, 2).decision, Decision::RejectOversized);
  EXPECT_EQ(e.stats().misses, 2);
  EXPECT_EQ(e.resident_count(), 0);
}
TEST(EngineGolden, G5TTL) {
  CacheEngine e(config(), "lru", {});
  EXPECT_TRUE(load(e, 1, 50, 1, Nanos{5000000000}).admitted);
  EXPECT_TRUE(e.lookup(1, at(5)).hit);
  auto result = e.lookup(1, at(6));
  ASSERT_FALSE(result.hit);
  ASSERT_EQ(result.expired.size(), 1);
  EXPECT_EQ(result.expired[0], 1);
  EXPECT_EQ(e.stats().expirations, 1);
  EXPECT_TRUE(e.complete_miss(1, 50, 10, Nanos{5000000000}, result.generation, at(6)).admitted);
  EXPECT_EQ(e.explain(1, at(6))->expiry, at(11));
}
TEST(EngineGolden, G8InvalidationDuringLoad) {
  CacheEngine e(config(), "lru", {});
  auto result = e.lookup(1, at(1));
  e.invalidate(1, at(1));
  EXPECT_EQ(e.complete_miss(1, 50, 10, {}, result.generation, at(1)).decision,
            Decision::RejectInvalidatedDuringLoad);
}
TEST(EngineGolden, G9SizeChange) {
  CacheEngine e(config(), "lru", {});
  ASSERT_TRUE(load(e, 1, 50, 1).admitted);
  auto result = e.lookup(1, at(2), 60);
  EXPECT_FALSE(result.hit);
  ASSERT_EQ(result.replaced.size(), 1);
  EXPECT_EQ(result.replaced[0], 1);
  EXPECT_TRUE(e.complete_miss(1, 60, 10, {}, result.generation, at(2)).admitted);
  EXPECT_EQ(e.stats().size_changes, 1);
  EXPECT_EQ(e.bytes_resident(), 60);
}
TEST(EngineGolden, LruEvictionOrderAndPins) {
  auto c = config();
  c.observation.max_entries = 0;
  CacheEngine e(c, "lru", {});
  EXPECT_TRUE(load(e, 1, 40, 1).admitted);
  EXPECT_TRUE(load(e, 2, 40, 2).admitted);
  EXPECT_TRUE(e.lookup(1, at(3)).hit);
  auto pending = e.lookup(4, at(4));
  EXPECT_FALSE(pending.hit);
  auto result = load(e, 3, 50, 5);
  ASSERT_EQ(result.evicted.size(), 1);
  EXPECT_EQ(result.evicted[0], 2);
  e.abort_miss(4, at(5));
  EXPECT_FALSE(e.explain(4, at(5)));
  EXPECT_TRUE(e.check_invariants(at(5)));
}
TEST(EngineGolden, ExpiryDeliveredByCostHintBeforeNextLookup) {
  CacheEngine e(config(), "lru", {});
  ASSERT_TRUE(load(e, 1, 50, 1, Nanos{1000000000}).admitted);
  e.observe_cost_hint(2, 5, at(3));
  ASSERT_EQ(e.expired_keys().size(), 1);
  EXPECT_EQ(e.expired_keys()[0], 1);
  EXPECT_EQ(e.stats().expirations, 1);
  EXPECT_TRUE(e.check_invariants(at(3)));
  auto result = e.lookup(3, at(3));
  EXPECT_TRUE(result.expired.empty());
  EXPECT_EQ(e.stats().expirations, 1);
  e.abort_miss(3, at(3));
  EXPECT_TRUE(e.check_invariants(at(3)));
}
TEST(EngineGolden, RejectInvalidConfiguration) {
  auto expect_bad = [](EngineConfig c) {
    EXPECT_THROW(CacheEngine(c, "lru", {}), std::invalid_argument);
  };
  auto c = config();
  c.capacity_bytes = 0;
  expect_bad(c);
  c = config();
  c.frequency.bucket_seconds = 0;
  expect_bad(c);
  c = config();
  c.frequency.window_seconds = 299;
  expect_bad(c);
  c = config();
  c.frequency.window_seconds = 540;
  expect_bad(c);
  c = config();
  c.frequency.mode = FrequencyConfig::Mode::ForwardDecay;
  c.frequency.decay_tau_seconds = 0;
  expect_bad(c);
  c = config();
  c.miss_cost.ewma_alpha = std::numeric_limits<double>::quiet_NaN();
  expect_bad(c);
  c = config();
  c.miss_cost.min_ms = 0;
  expect_bad(c);
  c = config();
  c.miss_cost.max_ms = 0;
  expect_bad(c);
  c = config();
  c.miss_cost.default_ms = std::numeric_limits<double>::infinity();
  expect_bad(c);
  c = config();
  c.miss_cost.min_samples = 0;
  expect_bad(c);
  c = config();
  c.admission.min_observations = 0;
  expect_bad(c);
  c = config();
  c.value.size_exponent = 3;
  expect_bad(c);
  c = config();
  c.telemetry.decision_sample_rate = -1;
  expect_bad(c);
  PolicyConfig pc;
  pc.tinylfu.window_fraction = 1;
  EXPECT_THROW(CacheEngine(config(), "lru", pc), std::invalid_argument);
  pc = {};
  pc.redis.samples = 0;
  EXPECT_THROW(CacheEngine(config(), "lru", pc), std::invalid_argument);
  EXPECT_THROW(CacheEngine(config(), "unknown", {}), std::invalid_argument);
}
class InvalidPlanPolicy final : public Policy {
 public:
  explicit InvalidPlanPolicy(bool admit) : admit_(admit) {}
  std::string_view name() const override { return "invalid_plan"; }
  void on_hit(SlotId, TimePoint) override {}
  void on_insert(SlotId, TimePoint) override {}
  void on_remove(SlotId, RemovalReason) override {}
  std::uint64_t metadata_bytes() const override { return 0; }
  void plan_admission(SlotId, std::uint64_t, TimePoint, AdmissionPlan& plan) override {
    plan.admit = admit_;
    plan.decision = Decision::RejectPolicy;
    plan.victim_bytes = 1;
  }

 private:
  bool admit_;
};
TEST(EngineGolden, InvalidPolicyPlanDoesNotRemoveResidents) {
  register_policy("test_invalid_admission", [](const PolicyConfig&, const PolicyContext&) {
    return std::make_unique<InvalidPlanPolicy>(true);
  });
  register_policy("test_invalid_rejection", [](const PolicyConfig&, const PolicyContext&) {
    return std::make_unique<InvalidPlanPolicy>(false);
  });
  for (auto name : {"test_invalid_admission", "test_invalid_rejection"}) {
    CacheEngine e(config(), name, {});
    ASSERT_TRUE(load(e, 1, 100, 1).admitted);
    auto result = e.lookup(2, at(2));
    EXPECT_THROW(e.complete_miss(2, 50, 1, {}, result.generation, at(2)), std::logic_error);
    EXPECT_EQ(e.bytes_resident(), 100);
    EXPECT_TRUE(e.explain(1, at(2))->resident);
    e.abort_miss(2, at(2));
  }
}
TEST(EngineGolden, TimeAndMissingLoadContracts) {
  CacheEngine e(config(), "lru", {});
  e.observe_cost_hint(1, 2, at(2));
  EXPECT_THROW(e.lookup(1, at(1)), std::invalid_argument);
  EXPECT_THROW(e.complete_miss(2, 1, 1, {}, 0, at(2)), std::logic_error);
}
}  // namespace
}  // namespace clache
