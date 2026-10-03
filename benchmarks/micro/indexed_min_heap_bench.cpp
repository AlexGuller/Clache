#include <benchmark/benchmark.h>

#include "clache/indexed_min_heap.h"
#include "clache/rng.h"

namespace clache {
namespace {

IndexedMinHeap make_heap(std::size_t n, Xoshiro256ss& rng) {
  IndexedMinHeap heap;
  heap.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    heap.push(static_cast<SlotId>(i), rng.uniform01(), i, i);
  }
  return heap;
}

void BM_HeapUpdate(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  Xoshiro256ss rng(1);
  auto heap = make_heap(n, rng);
  std::uint64_t seq = n;
  for (auto _ : state) {
    const auto slot = static_cast<SlotId>(rng.uniform_below(n));
    heap.update(slot, rng.uniform01(), ++seq);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HeapUpdate)->RangeMultiplier(10)->Range(10'000, 10'000'000);

void BM_HeapForEachAscending(benchmark::State& state) {
  const auto k = static_cast<std::size_t>(state.range(0));
  Xoshiro256ss rng(2);
  const auto heap = make_heap(1'000'000, rng);
  for (auto _ : state) {
    std::size_t visited = 0;
    heap.for_each_ascending([&](const IndexedMinHeap::Node& node) {
      double key = node.key;
      benchmark::DoNotOptimize(key);
      return ++visited < k;
    });
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HeapForEachAscending)->Arg(1)->Arg(8)->Arg(64);

void BM_HeapHeapify(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  Xoshiro256ss rng(3);
  auto heap = make_heap(n, rng);
  for (auto _ : state) {
    for (auto& node : heap.mutable_nodes()) node.key = rng.uniform01();
    heap.heapify();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_HeapHeapify)
    ->RangeMultiplier(10)
    ->Range(100'000, 1'000'000)
    ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace clache
