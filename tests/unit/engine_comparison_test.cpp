#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "clache/engine.h"
#include "clache/policy.h"

namespace clache {
namespace {
EngineConfig comparison_config() {
  EngineConfig c;
  c.capacity_bytes = 100;
  return c;
}
TimePoint time_at(std::int64_t seconds) { return TimePoint::from_ns(seconds * 1000000000LL); }
std::vector<KeyId> keys(std::span<const KeyId> values) { return {values.begin(), values.end()}; }
void insert(CacheEngine& engine, KeyId key, std::optional<Nanos> ttl = Nanos{1000000000LL}) {
  const auto lookup = engine.lookup(key, time_at(0), 10);
  ASSERT_FALSE(lookup.hit);
  ASSERT_TRUE(engine.complete_miss(key, 10, 1, ttl, lookup.generation, time_at(0)).admitted);
}

TEST(EngineComparison, InvalidationPurgesExpiredTargetAndUnrelatedKeysFirst) {
  for (const auto* policy : {"lru", "clache"}) {
    SCOPED_TRACE(policy);
    CacheEngine engine(comparison_config(), policy, {});
    EXPECT_TRUE(engine.expired_keys().empty());
    insert(engine, 1);
    insert(engine, 2);
    const auto invalidated = engine.invalidate(1, time_at(65));
    EXPECT_TRUE(invalidated.empty());
    EXPECT_EQ(keys(engine.expired_keys()), (std::vector<KeyId>{1, 2}));
    EXPECT_EQ(engine.stats().expirations, 2U);
    EXPECT_EQ(engine.stats().invalidations, 0U);
    EXPECT_EQ(engine.stats().requests, 2U);
    EXPECT_EQ(engine.bytes_resident(), 0U);
    EXPECT_TRUE(engine.check_invariants(time_at(65)));
    // Const operations leave the expiry span intact; the next mutation replaces it.
    (void)engine.explain(1, time_at(65));
    EXPECT_EQ(keys(engine.expired_keys()), (std::vector<KeyId>{1, 2}));
    engine.invalidate(99, time_at(65));
    EXPECT_TRUE(engine.expired_keys().empty());
  }
}

TEST(EngineComparison, InvalidationSeparatesLiveTargetFromExpiredKeys) {
  CacheEngine engine(comparison_config(), "lru", {});
  insert(engine, 1, std::nullopt);
  insert(engine, 2);
  EXPECT_EQ(keys(engine.invalidate(1, time_at(1))), (std::vector<KeyId>{1}));
  EXPECT_EQ(keys(engine.expired_keys()), (std::vector<KeyId>{2}));
  EXPECT_EQ(engine.stats().invalidations, 1U);
  EXPECT_EQ(engine.stats().expirations, 1U);
  EXPECT_TRUE(engine.check_invariants(time_at(1)));
}

TEST(EngineComparison, CostHintPurgesBeforeObservingExpiredTarget) {
  for (const auto* policy : {"lru", "clache"}) {
    SCOPED_TRACE(policy);
    CacheEngine engine(comparison_config(), policy, {});
    insert(engine, 1);
    insert(engine, 2);
    const auto score_updates = engine.stats().score_updates;
    engine.observe_cost_hint(1, 5, time_at(65));
    EXPECT_EQ(keys(engine.expired_keys()), (std::vector<KeyId>{1, 2}));
    EXPECT_EQ(engine.stats().expirations, 2U);
    EXPECT_EQ(engine.stats().requests, 2U);
    EXPECT_EQ(engine.stats().score_updates, score_updates);
    const auto explanation = engine.explain(1, time_at(65));
    ASSERT_TRUE(explanation.has_value());
    EXPECT_FALSE(explanation->resident);
    EXPECT_DOUBLE_EQ(explanation->miss_cost_ms, 1.8);
    EXPECT_EQ(engine.bytes_resident(), 0U);
    EXPECT_TRUE(engine.check_invariants(time_at(65)));
  }
}

TEST(EngineComparison, AbortingConcurrentLoadPurgesTargetAndPreservesOtherPins) {
  auto config = comparison_config();
  config.observation.max_entries = 0;
  CacheEngine engine(config, "lru", {});
  const auto first = engine.lookup(1, time_at(0), 10);
  const auto concurrent = engine.lookup(1, time_at(0), 10);
  ASSERT_EQ(first.slot, concurrent.slot);
  ASSERT_TRUE(
      engine.complete_miss(1, 10, 1, Nanos{1000000000LL}, first.generation, time_at(0)).admitted);
  insert(engine, 2);
  const auto pending = engine.lookup(3, time_at(0), 10);
  engine.abort_miss(1, time_at(1));
  EXPECT_EQ(keys(engine.expired_keys()), (std::vector<KeyId>{1, 2}));
  EXPECT_EQ(engine.stats().expirations, 2U);
  EXPECT_EQ(engine.stats().requests, 4U);
  EXPECT_EQ(engine.resident_count(), 0U);
  EXPECT_TRUE(engine.check_invariants(time_at(1)));
  // Zero ghost budget can trim expired records, but never the outstanding load.
  ASSERT_TRUE(engine.explain(3, time_at(1)).has_value());
  EXPECT_TRUE(
      engine.complete_miss(3, 10, 2, std::nullopt, pending.generation, time_at(1)).admitted);
  EXPECT_TRUE(engine.expired_keys().empty());
  EXPECT_TRUE(engine.check_invariants(time_at(1)));
}

TEST(EngineComparison, HintAndInvalidateKeepOutstandingLoadPinned) {
  auto config = comparison_config();
  config.observation.max_entries = 0;
  CacheEngine engine(config, "lru", {});
  insert(engine, 1);
  const auto pending = engine.lookup(3, time_at(0), 10);
  engine.observe_cost_hint(99, 3, time_at(1));
  EXPECT_EQ(keys(engine.expired_keys()), (std::vector<KeyId>{1}));
  engine.invalidate(3, time_at(1));
  const auto result = engine.complete_miss(3, 10, 1, std::nullopt, pending.generation, time_at(1));
  EXPECT_EQ(result.decision, Decision::RejectInvalidatedDuringLoad);
  EXPECT_TRUE(engine.check_invariants(time_at(1)));
}

TEST(EngineComparison, CompleteMissAndLookupExposeCentralizedExpiry) {
  CacheEngine engine(comparison_config(), "lru", {});
  insert(engine, 1);
  const auto pending = engine.lookup(2, time_at(0), 10);
  const auto completed =
      engine.complete_miss(2, 10, 1, Nanos{1000000000LL}, pending.generation, time_at(1));
  EXPECT_EQ(keys(completed.expired), (std::vector<KeyId>{1}));
  EXPECT_EQ(keys(engine.expired_keys()), keys(completed.expired));
  const auto lookup = engine.lookup(3, time_at(2), 10);
  EXPECT_EQ(keys(lookup.expired), (std::vector<KeyId>{2}));
  EXPECT_EQ(keys(engine.expired_keys()), keys(lookup.expired));
  engine.abort_miss(3, time_at(2));
  EXPECT_EQ(engine.stats().expirations, 2U);
  EXPECT_TRUE(engine.check_invariants(time_at(2)));
}

TEST(EngineComparison, PublicValidationAllowsNonnegativeDefaultsOutsideSampleBounds) {
  for (double default_ms : {0.0, 0.001, 50000.0}) {
    SCOPED_TRACE(default_ms);
    auto config = comparison_config();
    config.miss_cost.default_ms = default_ms;
    config.miss_cost.min_samples = 2;
    ASSERT_TRUE(validate(config).empty());
    CacheEngine engine(config, "lru", {});
    const auto lookup = engine.lookup(1, time_at(0), 10);
    ASSERT_TRUE(engine.explain(1, time_at(0)).has_value());
    EXPECT_EQ(engine.explain(1, time_at(0))->miss_cost_ms, default_ms);
    // Observed samples remain clamped independently of the cold-start default.
    ASSERT_TRUE(
        engine.complete_miss(1, 10, 40000, std::nullopt, lookup.generation, time_at(0)).admitted);
    EXPECT_EQ(engine.explain(1, time_at(0))->miss_cost_ms, std::min(30000.0, default_ms));
  }
}

TEST(EngineComparison, ConstructorReportsAllPublicValidationErrors) {
  auto config = comparison_config();
  config.capacity_bytes = 0;
  config.max_object_bytes = 1;
  config.miss_cost.default_ms = -1;
  config.frequency.bucket_seconds = 0;
  PolicyConfig policy;
  policy.redis.samples = 0;
  policy.tinylfu.window_fraction = 1;
  auto expected = validate(config);
  const auto policy_errors = validate(policy);
  expected.insert(expected.end(), policy_errors.begin(), policy_errors.end());
  ASSERT_GT(expected.size(), 3U);
  try {
    CacheEngine engine(config, "lru", policy);
    FAIL() << "invalid configuration accepted";
  } catch (const std::invalid_argument& error) {
    for (const auto& message : expected) {
      EXPECT_NE(std::string(error.what()).find(message), std::string::npos) << message;
    }
  }
  for (double bad :
       {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    config = comparison_config();
    config.miss_cost.default_ms = bad;
    EXPECT_THROW(CacheEngine(config, "lru", {}), std::invalid_argument);
  }
}

class EntryHintPolicy final : public Policy {
 public:
  explicit EntryHintPolicy(std::uint64_t hint) : hint_(hint) {}
  std::string_view name() const override { return "engine_comparison_hint"; }
  void on_hit(SlotId, TimePoint) override {}
  void plan_admission(SlotId, std::uint64_t, TimePoint, AdmissionPlan& plan) override {
    plan.reset();
  }
  void on_insert(SlotId, TimePoint) override {}
  void on_remove(SlotId, RemovalReason) override {}
  double value(SlotId, TimePoint) const override { return static_cast<double>(hint_); }
  std::uint64_t metadata_bytes() const override { return 0; }

 private:
  std::uint64_t hint_;
};
CLACHE_REGISTER_POLICY("engine_comparison_hint",
                       [](const PolicyConfig&, const PolicyContext& context) {
                         return std::make_unique<EntryHintPolicy>(context.expected_entries);
                       });

TEST(EngineComparison, ExpectedEntryHintReachesPolicyAndOldCallsKeepDefault) {
  CacheEngine old_call(comparison_config(), "engine_comparison_hint", {});
  CacheEngine four_arguments(comparison_config(), "engine_comparison_hint", {}, nullptr);
  CacheEngine hinted(comparison_config(), "engine_comparison_hint", {}, nullptr, 12345);
  for (auto* engine : {&old_call, &four_arguments, &hinted}) insert(*engine, 1, std::nullopt);
  EXPECT_EQ(old_call.explain(1, time_at(0))->value, 0);
  EXPECT_EQ(four_arguments.explain(1, time_at(0))->value, 0);
  EXPECT_EQ(hinted.explain(1, time_at(0))->value, 12345);
}
}  // namespace
}  // namespace clache
