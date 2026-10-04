#include <algorithm>
#include <array>
#include <bit>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "clache/engine.h"
#include "clache/hash.h"
#include "clache/rng.h"

namespace clache {
namespace {
void Append(std::string& bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    bytes.push_back(static_cast<char>((value >> shift) & 255));
}
void Append(std::string& bytes, double value) {
  Append(bytes, std::bit_cast<std::uint64_t>(value));
}

struct Operation {
  KeyId key;
  TimePoint time;
  std::uint64_t size;
  double cost;
  std::optional<Nanos> ttl;
  bool invalidate;
};
std::vector<Operation> MixedTrace() {
  Xoshiro256ss rng(7);
  std::vector<Operation> trace;
  trace.reserve(200'000);
  std::int64_t nanos = 0;
  for (std::size_t i = 0; i < 200'000; ++i) {
    if (i != 0) nanos += rng.uniform_below(100) == 0 ? 61'000'000'000 : 1'000'000;
    const auto key = rng.uniform_below(100) < 80 ? rng.uniform_below(10) : rng.uniform_below(50);
    std::optional<Nanos> ttl;
    if (rng.uniform_below(8) == 0)
      ttl = Nanos(static_cast<std::int64_t>(1 + rng.uniform_below(600)) * 1'000'000'000);
    trace.push_back({key, TimePoint::from_ns(nanos), 1 + rng.uniform_below(40),
                     0.1 + rng.uniform01() * 1000, ttl, rng.uniform_below(20) == 0});
  }
  // Establish the shared estimator origin on a request, rather than a deletion.
  trace.front().invalidate = false;
  return trace;
}

class CanonicalLog final : public DecisionSink {
 public:
  void record(const DecisionRecord& r) override {
    Append(bytes, r.seq);
    Append(bytes, static_cast<std::uint64_t>(r.now.ns()));
    Append(bytes, r.key);
    Append(bytes, static_cast<std::uint64_t>(r.decision));
    Append(bytes, static_cast<std::uint64_t>(r.policy.size()));
    bytes.append(r.policy);
    Append(bytes, r.candidate_value);
    Append(bytes, r.threshold_value);
    Append(bytes, r.frequency_rps);
    Append(bytes, r.miss_cost_ms);
    Append(bytes, r.size_bytes);
    Append(bytes, r.bytes_needed);
    Append(bytes, static_cast<std::uint64_t>(r.victim_count));
    for (const auto key : r.victims_head) Append(bytes, key);
  }
  std::string bytes;
};

struct ReplayResult {
  std::string decisions;
  std::string log;
  std::string counters;
  std::uint64_t trace_fingerprint;
  std::uint64_t config_fingerprint;
  bool valid;
};
ReplayResult Replay(std::string_view name, const std::vector<Operation>& trace) {
  EngineConfig config;
  config.capacity_bytes = 200;
  config.seed = 7;
  config.telemetry.decision_sample_rate = 0.01;
  config.telemetry.sample_hits = true;
  config.observation = {50, 1200};
  CanonicalLog log;
  CacheEngine engine(config, name, {}, &log);
  std::string decisions, canonical_trace, canonical_config;
  Append(canonical_config, config.capacity_bytes);
  Append(canonical_config, config.seed);
  Append(canonical_config, config.telemetry.decision_sample_rate);
  Append(canonical_config, static_cast<std::uint64_t>(config.observation.max_entries));
  Append(canonical_config, static_cast<std::uint64_t>(config.observation.ttl_seconds));
  canonical_config.append(name);
  bool valid = true;
  for (const auto& op : trace) {
    Append(canonical_trace, op.key);
    Append(canonical_trace, static_cast<std::uint64_t>(op.time.ns()));
    Append(canonical_trace, op.size);
    Append(canonical_trace, op.cost);
    Append(canonical_trace, op.ttl ? static_cast<std::uint64_t>(op.ttl->count()) : 0);
    Append(canonical_trace, static_cast<std::uint64_t>(op.invalidate));
    if (op.invalidate) {
      const auto removed = engine.invalidate(op.key, op.time);
      Append(decisions, static_cast<std::uint64_t>(removed.size()));
      for (const auto key : removed) Append(decisions, key);
      continue;
    }
    const auto lookup = engine.lookup(op.key, op.time, op.size);
    Append(decisions, static_cast<std::uint64_t>(lookup.hit));
    Append(decisions, static_cast<std::uint64_t>(lookup.expired.size()));
    for (const auto key : lookup.expired) Append(decisions, key);
    Append(decisions, static_cast<std::uint64_t>(lookup.replaced.size()));
    for (const auto key : lookup.replaced) Append(decisions, key);
    if (!lookup.hit) {
      const auto admission =
          engine.complete_miss(op.key, op.size, op.cost, op.ttl, lookup.generation, op.time);
      Append(decisions, static_cast<std::uint64_t>(admission.decision));
      Append(decisions, static_cast<std::uint64_t>(admission.evicted.size()));
      for (const auto key : admission.evicted) Append(decisions, key);
    }
    valid = valid && engine.check_invariants(op.time);
  }
  const auto& stats = engine.stats();
  std::string counters;
  for (const auto value :
       {stats.requests, stats.hits, stats.misses, stats.admissions, stats.admission_rejections,
        stats.evictions, stats.expirations, stats.invalidations, stats.size_changes,
        stats.bytes_evicted, stats.score_updates, stats.heap_rebuilds, stats.freq_saturations,
        engine.bytes_resident(), static_cast<std::uint64_t>(engine.resident_count())})
    Append(counters, value);
  for (const auto value : stats.by_decision) Append(counters, value);
  Append(counters, stats.origin_cost_ms_total);
  Append(counters, stats.origin_cost_ms_saved_estimate);
  return {std::move(decisions),    std::move(log.bytes),     std::move(counters),
          hash64(canonical_trace), hash64(canonical_config), valid};
}

TEST(Determinism, MixedSeedSevenAcrossSerialAndEightWorkers) {
  const auto trace = MixedTrace();
  std::vector<std::string> names;
  for (const auto& name : registered_policies())
    if (!name.starts_with("test_")) names.push_back(name);
  ASSERT_FALSE(names.empty());
  std::vector<ReplayResult> serial;
  for (const auto& name : names) serial.push_back(Replay(name, trace));
  for (int repeat = 0; repeat < 2; ++repeat) {
    // Batch at most eight independent policy engines, matching simulator job isolation.
    for (std::size_t begin = 0; begin < names.size(); begin += 8) {
      std::vector<std::future<ReplayResult>> workers;
      const auto end = std::min(names.size(), begin + 8);
      for (std::size_t i = begin; i < end; ++i) {
        workers.push_back(
            std::async(std::launch::async, [&, i] { return Replay(names[i], trace); }));
      }
      for (std::size_t i = begin; i < end; ++i) {
        SCOPED_TRACE(names[i]);
        const auto concurrent = workers[i - begin].get();
        EXPECT_TRUE(serial[i].valid);
        EXPECT_TRUE(concurrent.valid);
        EXPECT_EQ(serial[i].decisions, concurrent.decisions);
        EXPECT_EQ(serial[i].log, concurrent.log);
        EXPECT_EQ(serial[i].counters, concurrent.counters);
        EXPECT_EQ(serial[i].trace_fingerprint, concurrent.trace_fingerprint);
        EXPECT_EQ(serial[i].config_fingerprint, concurrent.config_fingerprint);
      }
    }
  }
  RecordProperty("trace_requests", 200000);
  RecordProperty("seed", 7);
  RecordProperty("max_workers", 8);
}
}  // namespace
}  // namespace clache
