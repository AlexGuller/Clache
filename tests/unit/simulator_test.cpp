#include "sim/simulator.h"

#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace clache::sim {
namespace {
Trace trace(std::initializer_list<TraceRecord> records) {
  Trace t;
  for (const auto& r : records) {
    t.timestamp_ns.push_back(r.timestamp_ns);
    t.key.push_back(r.key);
    t.size_bytes.push_back(r.size_bytes);
    t.miss_cost_us.push_back(r.miss_cost_us);
    t.ttl_s.push_back(r.ttl_s);
    t.region.push_back(r.region);
    t.op.push_back(r.op);
  }
  return t;
}
Options options() {
  Options o;
  o.policies = {"lru"};
  o.capacities = {2};
  o.warmup_fraction = 0;
  return o;
}
TEST(Simulator, PrepassUsesMaximumSizeAndGetCounts) {
  const auto t = trace(
      {{0, 1, 4, 10}, {1, 1, 8, 20}, {2, 2, 2, 30, 0, 2}, {3, 99, 0, 100, 0, 3, TraceOp::Del}});
  const auto s = prepass(t);
  EXPECT_EQ(s.records, 4U);
  EXPECT_EQ(s.gets, 3U);
  EXPECT_EQ(s.unique_keys, 2U);
  EXPECT_EQ(s.working_set_bytes, 10U);
  EXPECT_EQ(s.potential_cost, 60U);
  EXPECT_EQ(s.mean_object_size, 5);
  EXPECT_EQ(s.one_hit_fraction, 0.5);
  EXPECT_EQ(s.regions, (std::vector<std::uint16_t>{0, 2, 3}));
}
TEST(Simulator, WarmupUsesRecordsButMetricsCountGetsAndTrueCosts) {
  const auto t = trace({{0, 1, 1, 10},
                        {1, 2, 1, 20},
                        {2, 1, 1, 100},
                        {3, 3, 1, 200},
                        {4, 1, 0, 0, 0, 0, TraceOp::Del},
                        {5, 3, 1, 400}});
  auto o = options();
  o.warmup_fraction = 0.34;
  o.timeseries = "unused";
  o.timeseries_every = 2;
  const auto r = simulate(t, prepass(t), o).at(0);
  EXPECT_EQ(r.warmup_requests, 2U);
  EXPECT_EQ(r.metrics.requests, 3U);
  EXPECT_EQ(r.metrics.hits, 2U);
  EXPECT_EQ(r.metrics.cost, 700U);
  EXPECT_EQ(r.metrics.hit_cost, 500U);
  EXPECT_EQ(r.counters.admissions, 1U);
  EXPECT_EQ(r.counters.evictions, 1U);
  EXPECT_EQ(r.counters.invalidations, 1U);
  EXPECT_EQ(r.victims, 1U);
  EXPECT_EQ(r.max_victims, 1U);
  EXPECT_EQ(r.eviction_admits, 1U);
  ASSERT_EQ(r.intervals.size(), 2U);
  EXPECT_EQ(r.intervals[0].first_request, 2U);
  EXPECT_EQ(r.intervals[0].metrics.requests, 2U);
  EXPECT_EQ(r.intervals[1].metrics.hits, 1U);
}
TEST(Simulator, TtlSizeChangesAndOverheadAreReplayed) {
  const auto t = trace({{0, 1, 1, 100, 1},
                        {1000000000, 1, 1, 100},
                        {1000000001, 1, 2, 100},
                        {1000000002, 1, 2, 100}});
  auto o = options();
  o.capacities = {3};
  o.overhead = 1;
  const auto r = simulate(t, prepass(t), o).at(0);
  EXPECT_EQ(r.metrics.hits, 1U);
  EXPECT_EQ(r.metrics.hit_bytes, 2U);
  EXPECT_EQ(r.metrics.bytes, 6U);
  EXPECT_EQ(r.counters.expirations, 1U);
  EXPECT_EQ(r.counters.size_changes, 1U);
  o.capacities = {1};
  const auto rejected = simulate(t, prepass(t), o).at(0);
  EXPECT_EQ(rejected.counters.admission_rejections, 4U);
}
TEST(Simulator, MultiVictimMetricsExcludeWarmup) {
  const auto t = trace({{0, 1, 1, 1}, {1, 2, 1, 1}, {2, 3, 1, 1}, {3, 4, 3, 1}});
  auto o = options();
  o.capacities = {3};
  o.warmup_fraction = 0.75;
  const auto r = simulate(t, prepass(t), o).at(0);
  EXPECT_EQ(r.victims, 3U);
  EXPECT_EQ(r.max_victims, 3U);
  EXPECT_EQ(r.eviction_admits, 1U);
}
TEST(Simulator, RegionsRemainIndependentAndAggregateNumerators) {
  const auto t =
      trace({{0, 1, 1, 10, 0, 1}, {1, 1, 1, 100, 0, 2}, {2, 1, 1, 20, 0, 1}, {3, 2, 1, 200, 0, 2}});
  auto o = options();
  o.regional = true;
  o.jobs = 3;
  o.timeseries = "unused";
  o.timeseries_every = 2;
  const auto r = simulate(t, prepass(t), o);
  ASSERT_EQ(r.size(), 3U);
  EXPECT_EQ(r[0].domain, "1");
  EXPECT_EQ(r[1].domain, "2");
  EXPECT_EQ(r[2].domain, "ALL");
  EXPECT_EQ(r[2].metrics.requests, 4U);
  EXPECT_EQ(r[2].metrics.hits, 1U);
  EXPECT_EQ(r[2].metrics.cost, 330U);
  EXPECT_EQ(r[2].metrics.hit_cost, 20U);
  EXPECT_EQ(r[2].counters.admissions, 3U);
  EXPECT_EQ(r[2].intervals[1].metrics.requests, 2U);
}
TEST(Simulator, JobsDoNotChangeDecisionsAndFingerprintsCoverAblations) {
  const auto t = trace({{0, 1, 1, 10}, {1, 2, 1, 20}, {2, 1, 1, 30}, {3, 3, 1, 40}, {4, 1, 1, 50}});
  auto o = options();
  o.policies = {"lru",           "lfu",          "clache",         "clache_nocost",
                "clache_nosize", "clache_decay", "clache_noghost", "clache_noadmit"};
  o.decisions = "unused";
  o.config.engine.telemetry.decision_sample_rate = 1;
  o.config.engine.telemetry.sample_hits = true;
  const auto a = simulate(t, prepass(t), o);
  o.jobs = 8;
  const auto b = simulate(t, prepass(t), o);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].decision_log, b[i].decision_log);
    EXPECT_EQ(a[i].fingerprint, b[i].fingerprint);
    EXPECT_EQ(a[i].metrics.hit_cost, b[i].metrics.hit_cost);
    EXPECT_FALSE(a[i].decision_log.empty());
    if (i > 2) {
      EXPECT_NE(a[2].fingerprint, a[i].fingerprint);
    }
  }
  o.cost_oracle = true;
  EXPECT_NE(simulate(t, prepass(t), o)[0].fingerprint, a[0].fingerprint);
}
TEST(Simulator, DecimalFractionsDoNotUnderRoundExactIntegerBoundaries) {
  const auto t = trace({{0, 1, 2, 1}, {1, 2, 2, 1}, {2, 3, 2, 1}, {3, 4, 2, 1}, {4, 5, 2, 1}});
  auto o = options();
  o.capacities.clear();
  o.capacity_fractions = {0.3};
  o.warmup_fraction = 0.6;
  const auto r = simulate(t, prepass(t), o).at(0);
  EXPECT_EQ(r.capacity, 3U);
  EXPECT_EQ(r.warmup_requests, 3U);
  EXPECT_EQ(r.metrics.requests, 2U);
}

TEST(Simulator, RequestedFractionsStayStableAcrossWorkingSets) {
  auto o = options();
  o.capacities.clear();
  o.capacity_fractions = {0.3};
  const auto first = trace({{0, 1, 10, 1}});
  const auto second = trace({{0, 1, 11, 1}});
  const auto a = simulate(first, prepass(first), o).at(0);
  const auto b = simulate(second, prepass(second), o).at(0);
  EXPECT_EQ(a.capacity, 3U);
  EXPECT_EQ(b.capacity, 3U);
  EXPECT_EQ(a.capacity_fraction, 0.3);
  EXPECT_EQ(b.capacity_fraction, 0.3);
}

TEST(Simulator, CostOracleUpdatesHitCostsBeforeLaterEviction) {
  const auto t = trace(
      {{0, 1, 1, 1000}, {1, 2, 1, 2000}, {2, 1, 1, 100000}, {3, 3, 1, 1000}, {4, 1, 1, 1000}});
  auto o = options();
  o.policies = {"gds"};
  const auto ordinary = simulate(t, prepass(t), o).at(0);
  o.cost_oracle = true;
  const auto oracle = simulate(t, prepass(t), o).at(0);
  EXPECT_EQ(ordinary.metrics.hits, 1U);
  EXPECT_EQ(oracle.metrics.hits, 2U);
}

TEST(Simulator, EmptyAndZeroCostTracesHaveFiniteOutput) {
  const auto t = trace({});
  auto o = options();
  auto r = simulate(t, prepass(t), o);
  ASSERT_EQ(r.size(), 1U);
  EXPECT_EQ(r[0].metrics.requests, 0U);
  std::ostringstream out;
  write_results(out, r, prepass(t), o, 0);
  EXPECT_EQ(out.str().find("nan"), std::string::npos);
  EXPECT_EQ(out.str().find("inf"), std::string::npos);
}
TEST(Simulator, RejectsRangeOverflowAndInvalidCli) {
  for (const auto& args : std::vector<std::vector<std::string>>{
           {},
           {"--trace", "x", "--jobs", "0"},
           {"--trace", "x", "--jobs", "-1"},
           {"--trace", "x", "--warmup-fraction", "nan"},
           {"--trace", "x", "--warmup-fraction", "1"},
           {"--trace", "x", "--capacity", "0"},
           {"--trace", "x", "--capacity", "1", "--capacity-fractions", "0.5"},
           {"--trace", "x", "--unknown"},
           {"--trace", "x", "--timeseries-every", "0"},
           {"--trace", "x", "--jobs", "18446744073709551616"}}) {
    EXPECT_THROW(parse_options(args), std::invalid_argument);
  }
  const auto t = trace({{std::numeric_limits<std::uint64_t>::max(), 1, 1, 1}});
  EXPECT_THROW(prepass(t), std::invalid_argument);
  const auto normal = trace({{0, 1, 1, 1}});
  auto o = options();
  o.overhead = std::numeric_limits<std::uint64_t>::max();
  EXPECT_THROW(simulate(normal, prepass(normal), o), std::overflow_error);
  o = options();
  o.capacities.clear();
  o.capacity_fractions = {0.001};
  EXPECT_THROW(simulate(normal, prepass(normal), o), std::invalid_argument);
}
TEST(Simulator, CommandLineOverridesAndFractions) {
  auto o = parse_options({"--trace", "x", "--capacity-fractions", "0.5,1", "--set", "clache.seed=4",
                          "--set", "clache.seed=8", "--decision-sample-rate", "1"});
  EXPECT_EQ(o.config.engine.seed, 8U);
  EXPECT_EQ(o.config.engine.telemetry.decision_sample_rate, 1);
  o.policies = {"lru"};
  o.warmup_fraction = 0;
  const auto t = trace({{0, 1, 4, 1}, {1, 2, 4, 1}});
  const auto r = simulate(t, prepass(t), o);
  ASSERT_EQ(r.size(), 2U);
  EXPECT_EQ(r[0].capacity, 4U);
  EXPECT_EQ(r[1].capacity, 8U);
}
}  // namespace
}  // namespace clache::sim
