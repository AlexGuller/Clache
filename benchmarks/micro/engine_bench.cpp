#include <memory>
#include <stdexcept>
#include <string_view>

#include <benchmark/benchmark.h>

#include "clache/engine.h"

#include "allocation_counter.h"

namespace clache {
namespace {
constexpr std::uint64_t kObjectBytes = 64;

std::unique_ptr<CacheEngine> populated_engine(std::size_t residents, std::string_view policy) {
  EngineConfig config;
  config.capacity_bytes = residents * kObjectBytes;
  config.observation.max_entries = 4096;
  config.telemetry.decision_sample_rate = 0;
  config.miss_cost.max_ms = 1e12;
  auto engine = std::make_unique<CacheEngine>(config, policy, PolicyConfig{});
  for (KeyId key = 0; key < residents; ++key) {
    const auto lookup = engine->lookup(key, TimePoint{});
    const auto admit =
        engine->complete_miss(key, kObjectBytes, 1, std::nullopt, lookup.generation, TimePoint{});
    if (!admit.admitted) throw std::logic_error("benchmark population rejected");
  }
  return engine;
}

void BM_EngineHit(benchmark::State& state, std::string_view policy) {
  const auto n = static_cast<std::size_t>(state.range(0));
  auto engine = populated_engine(n, policy);
  std::uint64_t total_allocations = 0;
  std::uint64_t key = 0;
  // Walk residents sequentially; timestamps stay in one bucket epoch.
  for (auto _ : state) {
    bench::begin_allocations();
    auto result = engine->lookup(key, TimePoint{});
    total_allocations += bench::end_allocations();
    benchmark::DoNotOptimize(result);
    if (!result.hit) {
      state.SkipWithError("steady-state hit became a miss");
      break;
    }
    if (++key == n) key = 0;
  }
  state.counters["allocations/op"] =
      static_cast<double>(total_allocations) / static_cast<double>(state.iterations());
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK_CAPTURE(BM_EngineHit, clache, "clache")->Arg(100'000)->Arg(1'000'000);
BENCHMARK_CAPTURE(BM_EngineHit, lru, "lru")->Arg(100'000)->Arg(1'000'000);

void BM_EngineMissAdmit(benchmark::State& state, std::string_view policy) {
  const auto n = static_cast<std::size_t>(state.range(0));
  auto engine = populated_engine(n, policy);
  std::uint64_t total_allocations = 0;
  std::uint64_t total_victims = 0;
  KeyId key = n;
  for (auto _ : state) {
    // Increasing first-sample cost ensures strict Clache admission for fresh keys.
    const double cost = 2.0 + static_cast<double>(key - n);
    bench::begin_allocations();
    const auto lookup = engine->lookup(key, TimePoint{});
    auto result = engine->complete_miss(key, kObjectBytes, cost, std::nullopt, lookup.generation,
                                        TimePoint{});
    total_allocations += bench::end_allocations();
    benchmark::DoNotOptimize(result);
    if (!result.admitted || result.evicted.size() != 1) {
      state.SkipWithError("steady-state miss did not admit with one victim");
      break;
    }
    total_victims += result.evicted.size();
    ++key;
  }
  const auto iterations = static_cast<double>(state.iterations());
  state.counters["allocations/op"] = static_cast<double>(total_allocations) / iterations;
  state.counters["victims/op"] = static_cast<double>(total_victims) / iterations;
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK_CAPTURE(BM_EngineMissAdmit, clache, "clache")->Arg(100'000)->Arg(1'000'000);
BENCHMARK_CAPTURE(BM_EngineMissAdmit, lru, "lru")->Arg(100'000)->Arg(1'000'000);

// Measures the policy's complete lazy rebuild, without populating an engine or
// including setup accesses. Fresh per-slot records are outside measured time.
void BM_BucketRebuild(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  EngineConfig config;
  config.capacity_bytes = n * kObjectBytes;
  MetadataStore metadata(config.observation);
  metadata.reserve(n);
  FrequencyEstimator frequency(config.frequency);
  MissCostEstimator cost(config.miss_cost);
  PolicyContext context{&metadata, &frequency, &cost, &config, config.capacity_bytes, 1, n};
  auto policy = make_policy("clache", PolicyConfig{}, context);
  for (KeyId key = 0; key < n; ++key) {
    const auto slot = metadata.lookup_or_observe(key, TimePoint{});
    metadata[slot].charged_bytes = kObjectBytes;
    frequency.record(metadata[slot].freq, TimePoint{});
    cost.observe(metadata[slot].cost, 1);
    metadata.mark_resident(slot);
    policy->on_insert(slot, TimePoint{});
  }
  std::int64_t seconds = 0;
  for (auto _ : state) {
    seconds += config.frequency.bucket_seconds;
    const auto now = TimePoint::from_ns(seconds * 1'000'000'000);
    state.PauseTiming();
    for (std::size_t slot = 0; slot < n; ++slot) {
      frequency.record(metadata[static_cast<SlotId>(slot)].freq, now);
    }
    state.ResumeTiming();
    policy->on_access(0, now);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_BucketRebuild)
    ->Arg(100'000)
    ->Arg(1'000'000)
    ->Arg(10'000'000)
    ->Unit(benchmark::kMillisecond);
}  // namespace
}  // namespace clache
