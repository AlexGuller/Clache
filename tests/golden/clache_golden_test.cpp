#include <vector>

#include <gtest/gtest.h>

#include "clache/engine.h"

namespace clache {
namespace {
TimePoint At(std::int64_t seconds) { return TimePoint::from_ns(seconds * 1'000'000'000); }
EngineConfig Config(std::uint64_t capacity) {
  EngineConfig config;
  config.capacity_bytes = capacity;
  config.telemetry.decision_sample_rate = 0;
  return config;
}
struct Result {
  Decision decision;
  std::vector<KeyId> victims;
};
Result Request(CacheEngine& engine, KeyId key, std::uint64_t bytes, double cost,
               std::int64_t seconds) {
  const auto lookup = engine.lookup(key, At(seconds), bytes);
  if (lookup.hit) return {Decision::Hit, {}};
  const auto result =
      engine.complete_miss(key, bytes, cost, std::nullopt, lookup.generation, At(seconds));
  EXPECT_TRUE(engine.check_invariants(At(seconds)));
  return {result.decision, {result.evicted.begin(), result.evicted.end()}};
}
TEST(ClacheGolden, G1CostBeatsRecency) {
  for (const auto name : {"clache", "lru"}) {
    CacheEngine engine(Config(100), name, {});
    EXPECT_EQ(Request(engine, 1, 50, 1, 1).decision, Decision::AdmitFreeSpace);
    EXPECT_EQ(Request(engine, 2, 50, 100, 2).decision, Decision::AdmitFreeSpace);
    EXPECT_EQ(Request(engine, 3, 50, 10, 3).victims, std::vector<KeyId>{1});
    const auto fourth = Request(engine, 1, 50, 1, 4);
    if (name == std::string_view("clache")) {
      EXPECT_EQ(fourth.decision, Decision::RejectCandidateNotHigher);
      EXPECT_TRUE(fourth.victims.empty());
      EXPECT_EQ(Request(engine, 2, 50, 100, 5).decision, Decision::Hit);
      EXPECT_DOUBLE_EQ(engine.stats().origin_cost_ms_total, 112);
      EXPECT_EQ(engine.stats().hits, 1);
      EXPECT_EQ(engine.stats().misses, 4);
    } else {
      EXPECT_EQ(fourth.victims, std::vector<KeyId>{2});
      EXPECT_EQ(Request(engine, 2, 50, 100, 5).decision, Decision::AdmitWithEviction);
      EXPECT_DOUBLE_EQ(engine.stats().origin_cost_ms_total, 212);
    }
  }
}
TEST(ClacheGolden, G2AtomicMultiVictimAdmitAndReject) {
  for (const double cost : {5.0, 1.8}) {
    CacheEngine engine(Config(100), "clache", {});
    Request(engine, 1, 30, 1, 1);
    Request(engine, 2, 30, 100, 2);
    Request(engine, 3, 40, 1, 3);
    const auto result = Request(engine, 4, 60, cost, 4);
    if (cost == 5.0) {
      EXPECT_EQ(result.decision, Decision::AdmitWithEviction);
      EXPECT_EQ(result.victims, (std::vector<KeyId>{3, 1}));
      EXPECT_EQ(engine.bytes_resident(), 90);
      EXPECT_EQ(engine.resident_count(), 2);
      EXPECT_EQ(engine.stats().evictions, 2);
      EXPECT_TRUE(engine.explain(2, At(4))->resident);
      EXPECT_TRUE(engine.explain(4, At(4))->resident);
    } else {
      EXPECT_EQ(result.decision, Decision::RejectCandidateNotHigher);
      EXPECT_TRUE(result.victims.empty());
      EXPECT_EQ(engine.bytes_resident(), 100);
      EXPECT_EQ(engine.resident_count(), 3);
      EXPECT_TRUE(engine.explain(3, At(4))->resident);
      EXPECT_EQ(engine.stats().evictions, 0);
    }
  }
}
TEST(ClacheGolden, G3ExactTieRejects) {
  CacheEngine engine(Config(60), "clache", {});
  Request(engine, 1, 30, 1, 1);
  Request(engine, 2, 30, 1, 2);
  const auto result = Request(engine, 3, 30, 1, 3);
  EXPECT_EQ(result.decision, Decision::RejectCandidateNotHigher);
  EXPECT_TRUE(result.victims.empty());
  EXPECT_EQ(engine.bytes_resident(), 60);
  EXPECT_EQ(engine.stats().admission_rejections, 1);
}
TEST(ClacheGolden, G6GhostHistoryEarnsAdmission) {
  for (const auto name : {"clache", "clache_noghost"}) {
    CacheEngine engine(Config(50), name, {});
    Request(engine, 1, 50, 10, 1);
    EXPECT_EQ(Request(engine, 2, 50, 10, 2).decision, Decision::RejectCandidateNotHigher);
    const auto result = Request(engine, 2, 50, 10, 3);
    if (name == std::string_view("clache")) {
      EXPECT_EQ(result.decision, Decision::AdmitWithEviction);
      EXPECT_EQ(result.victims, std::vector<KeyId>{1});
    } else {
      EXPECT_EQ(result.decision, Decision::RejectCandidateNotHigher);
      EXPECT_TRUE(result.victims.empty());
    }
  }
}
TEST(ClacheGolden, G7WindowAgingRebuild) {
  CacheEngine engine(Config(50), "clache", {});
  Request(engine, 1, 50, 10, 0);
  Request(engine, 1, 50, 10, 1);
  Request(engine, 1, 50, 10, 2);
  const auto result = Request(engine, 2, 50, 10, 400);
  EXPECT_EQ(result.decision, Decision::AdmitWithEviction);
  EXPECT_EQ(result.victims, std::vector<KeyId>{1});
  EXPECT_GE(engine.stats().heap_rebuilds, 1);
}
TEST(ClacheGolden, VictimLimitRejectsAtomically) {
  auto config = Config(100);
  config.admission.max_victims = 1;
  CacheEngine engine(config, "clache", {});
  Request(engine, 1, 30, 1, 1);
  Request(engine, 2, 30, 100, 2);
  Request(engine, 3, 40, 1, 3);
  const auto result = Request(engine, 4, 60, 5, 4);
  EXPECT_EQ(result.decision, Decision::RejectTooManyVictims);
  EXPECT_TRUE(result.victims.empty());
  EXPECT_EQ(engine.bytes_resident(), 100);
}
TEST(ClacheGolden, VariantsRegisterAndApplySwitches) {
  for (const auto name : {"clache", "clache_noadmit", "clache_nocost", "clache_nosize",
                          "clache_decay", "clache_noghost"}) {
    EXPECT_TRUE(is_registered_policy(name));
    CacheEngine engine(Config(50), name, {});
    Request(engine, 1, 50, 100, 1);
    const auto result = Request(engine, 2, 50, 1, 2);
    if (name == std::string_view("clache_noadmit")) {
      EXPECT_EQ(result.decision, Decision::AdmitWithEviction);
    } else {
      EXPECT_EQ(result.decision, Decision::RejectCandidateNotHigher);
    }
  }
}
TEST(ClacheGolden, DelayedCompletionRefreshesAgedScores) {
  CacheEngine engine(Config(50), "clache", {});
  Request(engine, 1, 50, 10, 0);
  Request(engine, 1, 50, 10, 1);
  const auto lookup = engine.lookup(2, At(2), 50);
  const auto result = engine.complete_miss(2, 50, 10, std::nullopt, lookup.generation, At(400));
  EXPECT_EQ(result.decision, Decision::RejectCandidateNotHigher);
  EXPECT_TRUE(result.evicted.empty());
  EXPECT_TRUE(engine.check_invariants(At(400)));
  EXPECT_DOUBLE_EQ(engine.explain(1, At(400))->frequency_rps, 0);
  EXPECT_DOUBLE_EQ(engine.explain(2, At(400))->frequency_rps, 0);
}
TEST(ClacheGolden, CostHintAcrossEpochRefreshesAllScores) {
  CacheEngine engine(Config(100), "clache", {});
  Request(engine, 1, 50, 10, 0);
  Request(engine, 2, 50, 20, 1);
  engine.observe_cost_hint(1, 100, At(400));
  EXPECT_TRUE(engine.check_invariants(At(400)));
  EXPECT_DOUBLE_EQ(engine.explain(1, At(400))->value, 0);
  EXPECT_DOUBLE_EQ(engine.explain(2, At(400))->value, 0);
  EXPECT_EQ(engine.stats().requests, 2);
}
}  // namespace
}  // namespace clache
