#include <benchmark/benchmark.h>

#include "clache/estimators.h"

namespace clache {
namespace {
FrequencyConfig config_for(std::int64_t mode) {
  FrequencyConfig config;
  if (mode != 0) config.mode = FrequencyConfig::Mode::ForwardDecay;
  return config;
}
void BM_FrequencyRecord(benchmark::State& state) {
  FrequencyEstimator estimator(config_for(state.range(0)));
  FrequencyState frequency;
  std::int64_t ns = 0;
  for (auto _ : state) {
    // Advancing one millisecond keeps the ring below saturation and decay finite.
    ns += 1'000'000;
    benchmark::DoNotOptimize(estimator.record(frequency, TimePoint::from_ns(ns)));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FrequencyRecord)->ArgName("mode")->Arg(0)->Arg(1);

void BM_FrequencyRate(benchmark::State& state) {
  FrequencyEstimator estimator(config_for(state.range(0)));
  FrequencyState frequency;
  for (std::int64_t seconds = 0; seconds < 240; ++seconds) {
    estimator.record(frequency, TimePoint::from_ns(seconds * 1'000'000'000));
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(estimator.rate(frequency, TimePoint::from_ns(250'000'000'000)));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FrequencyRate)->ArgName("mode")->Arg(0)->Arg(1);
}  // namespace
}  // namespace clache
