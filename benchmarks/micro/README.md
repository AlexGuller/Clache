# Micro benchmarks

Build and run from the repository root with CMake, a C++20 compiler, and
Google Benchmark (fetched by CMake):

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCLACHE_WERROR=ON \
  -DCLACHE_SANITIZE= -DCLACHE_CHECK_INVARIANTS=OFF
cmake --build build/release --target clache_micro -j 4
./build/release/benchmarks/clache_micro --benchmark_list_tests

# Repeat the Phase 1 target cases and estimator cases three times.
./build/release/benchmarks/clache_micro \
  '--benchmark_filter=^(BM_Engine(Hit|MissAdmit)/(clache|lru)/1000000|BM_BucketRebuild/1000000|BM_Frequency.*)$' \
  --benchmark_min_time=0.15s --benchmark_repetitions=3 \
  --benchmark_out=benchmarks/micro/release_results.json --benchmark_out_format=json

# Run the remaining sizes and heap cases once (10M residents needs several GB).
./build/release/benchmarks/clache_micro \
  '--benchmark_filter=^(BM_Engine(Hit|MissAdmit)/(clache|lru)/100000|BM_BucketRebuild/(100000|10000000)|BM_Heap.*)$' \
  --benchmark_min_time=0.01s --benchmark_repetitions=1 \
  --benchmark_out=benchmarks/micro/release_scale_results.json --benchmark_out_format=json
```

Use an otherwise idle machine for timing. Debug builds run full engine invariant
checks after operations and are unsuitable for these performance targets.
Sanitizers should be exercised separately from the Release timing runs.

## What is measured

- `BM_EngineHit`: sequential resident lookups, with 64-byte objects and timestamps
  held in one bucket epoch. Population and destruction of the engine are outside
  the measured loop. The operation result is checked on every iteration.
- `BM_EngineMissAdmit`: one missing lookup followed by `complete_miss`, including
  pinning, observation-table maintenance, victim planning, eviction and admission.
  Fresh keys have increasing miss costs so Clache admits them strictly; every
  iteration checks admission and exactly one victim. The observation table holds
  at most 4096 entries. Decision sampling is disabled for both engine benchmarks.
  These timings cover one victim, within the spec's limit of four; they do not
  establish a bound for every possible four-victim workload.
- `allocations/op`: a thread-local hook counts successful C++ `new`/`new[]`
  allocations, including aligned and nothrow forms, only around the engine
  calls. Population and benchmark framework allocations are excluded. This does
  not count direct `malloc` calls or allocations on other threads. Hook overhead
  remains in measured time. Small fractions above an integer reflect occasional
  container growth amortized over the measured operations.
- `BM_BucketRebuild`: a Clache policy over populated metadata, advancing one
  bucket boundary per iteration and measuring `on_access`'s lazy rebuild. Each
  resident receives a fresh frequency observation while timing is paused, so the
  rebuild processes live scores instead of only expired, zero-valued scores.
  It includes score recomputation, heap construction and the triggering access;
  population and the per-iteration frequency refresh are excluded.
- `BM_FrequencyRecord` / `BM_FrequencyRate`: bucket mode is `mode:0`, forward
  decay is `mode:1`. Record advances by one millisecond per sample; rate repeatedly
  reads a prepopulated state. `BM_HeapUpdate` includes random input generation;
  `BM_HeapForEachAscending` visits 1, 8 or 64 nodes in a million-node heap;
  `BM_HeapHeapify` includes assigning random keys before rebuilding the heap.

The benchmark binary provides the allocation hook; the library's allocator and
public interface are unchanged. Existing heap benchmarks and the new sources
are discovered automatically by CMake.

## Recorded Release results

Captured on October 3, 2026 on an Apple M1 MacBook Air (8 CPU cores, 8 GiB RAM),
macOS 26.0.1 (25A362), Apple Clang 17.0.0 (`clang-1700.3.19.1`), arm64,
Google Benchmark 1.9.1. Library code was at `555d134`, with the benchmark sources
in this directory. Build flags were `-O3 -DNDEBUG`, `CLACHE_WERROR=ON`,
`CLACHE_CHECK_INVARIANTS=OFF`, and no sanitizer. The JSON context's reported
24 MHz is not a useful CPU clock measurement on this platform.

The following are **wall-time medians and min–max ranges across three
repetitions**, from [release_results.json](release_results.json). CPU times are
also preserved there. These are observations from this machine and workload,
not assertions enforced by CI.

| Case, 1M residents | Median | Min–max | Allocations/op | Spec §20.1 target | Result |
|---|---:|---:|---:|---|---|
| Clache hit | 113.22 ns | 112.96–136.73 ns | 0 | ≤250 ns, 0 allocations | Pass |
| Clache miss + admit, 1 victim | 819.01 ns | 782.62–841.16 ns | 4.000015 | ≤1000 ns, ≤4 victims | Pass |
| Clache bucket rebuild, live scores | 17.79 ms | 17.77–17.90 ms | — | ≤50 ms | Pass |
| LRU hit | 24.26 ns | 24.21–24.42 ns | 0 | Reference | — |
| LRU miss + admit, 1 victim | 729.94 ns | 724.49–740.35 ns | 5.000010 | Reference | — |

| Estimator case | Median | Min–max |
|---|---:|---:|
| Bucket record | 3.523 ns | 3.517–3.529 ns |
| Forward-decay record | 35.684 ns | 35.673–35.699 ns |
| Bucket rate | 4.106 ns | 4.076–4.158 ns |
| Forward-decay rate | 4.052 ns | 4.011–4.061 ns |

[release_scale_results.json](release_scale_results.json) contains one repetition
for each remaining size and heap case. All 15 cases completed without benchmark
errors; a single repetition provides no variability estimate.

| Case | Size / victims | Wall time |
|---|---:|---:|
| Clache hit | 100K residents | 100.65 ns |
| LRU hit | 100K residents | 24.09 ns |
| Clache miss + admit | 100K residents, 1 victim | 588.93 ns |
| LRU miss + admit | 100K residents, 1 victim | 491.12 ns |
| Bucket rebuild | 100K residents | 1.86 ms |
| Bucket rebuild | 10M residents | 294.68 ms |
| Heap update | 10K nodes | 29.01 ns |
| Heap update | 100K nodes | 31.87 ns |
| Heap update | 1M nodes | 74.93 ns |
| Heap update | 10M nodes | 111.98 ns |
| Heap ascending traversal | 1 victim | 2.11 ns |
| Heap ascending traversal | 8 victims | 49.14 ns |
| Heap ascending traversal | 64 victims | 726.82 ns |
| Heap randomize + heapify | 100K nodes | 1.05 ms |
| Heap randomize + heapify | 1M nodes | 12.38 ms |

Together the two artifacts cover every Phase 1 benchmark parameter in §20.1
for the implemented Clache and LRU policies. The Phase 3 concurrent cache hit
benchmark and simulator experiment tables are outside this microbenchmark suite.
