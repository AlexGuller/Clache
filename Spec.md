# Clache — Implementation Specification

| Field | Value |
|---|---|
| Version | 0.2 (implementation spec) |
| Date | 2026-10-03 |
| Design baseline | *Clache Engineering Specification v0.1* (PDF, 03 Oct 2026), cited below as **[D§n]** |
| Language / build | C++20, CMake ≥ 3.24 |
| Scope of this document | Phases 1–3 in full detail (simulator, policy tuning, in-process cache); Phases 4+ in outline |

The v0.1 design doc says **what** Clache is and **why**. This document says **exactly how to build it**: module boundaries, data structures, algorithms, interfaces, file formats, configuration, tests, benchmarks, and the order of work. If the two disagree, this document wins for implementation, and every deliberate deviation is listed in §2.

Requirement keywords: **MUST** = required for the phase to exit; **SHOULD** = expected unless there is a written reason not to; **MAY** = optional.

---

## Table of contents

1. [Summary](#1-summary)
2. [Decisions resolved by this spec](#2-decisions-resolved-by-this-spec)
3. [Architecture](#3-architecture)
4. [Repository layout and build](#4-repository-layout-and-build)
5. [Core types](#5-core-types)
6. [Estimators](#6-estimators)
7. [Metadata store and observation table](#7-metadata-store-and-observation-table)
8. [Indexed min-heap](#8-indexed-min-heap)
9. [Policy interface](#9-policy-interface)
10. [Clache v0 policy (CostValuePolicy)](#10-clache-v0-policy-costvaluepolicy)
11. [Baseline policies](#11-baseline-policies)
12. [CacheEngine: the deterministic decision core](#12-cacheengine-the-deterministic-decision-core)
13. [Trace formats](#13-trace-formats)
14. [Simulator: clache_sim](#14-simulator-clache_sim)
15. [Trace generator: clache_tracegen](#15-trace-generator-clache_tracegen)
16. [In-process cache (Phase 3)](#16-in-process-cache-phase-3)
17. [Observability](#17-observability)
18. [Configuration](#18-configuration)
19. [Testing](#19-testing)
20. [Benchmarks and experiment protocol](#20-benchmarks-and-experiment-protocol)
21. [Redis comparison (Phase 4) outline](#21-redis-comparison-phase-4-outline)
22. [Phase plan and task breakdown](#22-phase-plan-and-task-breakdown)
23. [Coding standards](#23-coding-standards)
24. [Experiment matrix for open research questions](#24-experiment-matrix-for-open-research-questions)
25. [Glossary](#25-glossary)

---

## 1. Summary

Clache is a policy engine that decides which objects get scarce cache memory. Every tracked key has a **Cache Value**

```
V_i = (F_i × C_i) / S_i^α          [ms of avoided miss work per second per byte]
```

where `F_i` is the recent request rate (req/s), `C_i` is the estimated miss cost (ms), `S_i` is the charged size (bytes), and `α` is the size exponent (default 1.0) [D§4]. On a miss, the candidate is admitted only if enough lower-value residents can be evicted to fit it, and **every** required victim has strictly lower value. Otherwise it is served but not cached [D§5].

The primary metric is **total miss cost** at a fixed byte budget [D§10.4]. Clache is explicitly a GDSF-family policy [D§3.4]. Any research claim comes from measured comparisons with ablations against GDS/GDSF, W-TinyLFU, S3-FIFO, LRU and LFU, never from the formula itself.

The build order is:

1. **Phase 1:** a deterministic trace simulator with every baseline.
2. **Phase 2:** tuning experiments.
3. **Phase 3:** a thread-safe in-process cache that reuses the *same* decision core as the simulator, so simulator results carry over to the real cache.

---

## 2. Decisions resolved by this spec

The v0.1 document leaves these points open or underspecified. Each has a binding decision here.

| # | Topic | v0.1 says | This spec decides | Why |
|---|---|---|---|---|
| 1 | Score staleness | Indexed min-heap of `V` [D§8.4] | `V` depends on time through `F`, so stored heap keys go stale. **Rolling-bucket mode:** rebuild the whole heap (O(n) heapify) lazily at each bucket-epoch boundary; in between, keys are exact. **Decay mode:** use *forward decay*, which gives a time-invariant heap key, so no rebuild is needed (§6.1.3). | Without this, the heap's minimum is wrong and the admission rule is silently violated. |
| 2 | Score representation | `double` | The heap key is `K = ln V` (bucket mode) or `K = ln V + u(t)` (decay mode). A zero frequency gives `K = -inf`. | Allows forward decay without overflow; ordering is unchanged. |
| 3 | Ties | `V_victim >= V_new` rejects [D§5.2] | Kept: a victim must be **strictly** lower. Heap order uses a total order `(K, last_access_seq, key_id)` for determinism. | Deterministic, matches the PDF. |
| 4 | Sim vs. production code | Separate simulator and CacheEngine | One deterministic, value-free **`CacheEngine`** (§12) is shared by `clache_sim` and the thread-safe `Cache` (§16). | Simulator results apply directly to the real cache. |
| 5 | Who owns telemetry | Per-key metadata [D§6.1] | The **engine** owns F/C/S telemetry for all tracked keys, resident and observed. Policies read it. Baselines can therefore get *equivalent telemetry* [D§15 Q2]. | Fair comparison against GDSF. |
| 6 | Policy interface | `score / plan_admission / on_hit / on_insert / on_remove` [D App. B] | Reshaped (§9): slot IDs, an `on_access` hook, `plan_admission(candidate, bytes_needed)`, and an explicit contract that always-admit policies may commit internal reordering during planning. | Needed to express W-TinyLFU and S3-FIFO in the same engine. |
| 7 | Miss cost in the simulator | Trace has `miss_cost_us` per request | The estimator sees the trace cost **only on misses** (realistic). `--cost-oracle` also feeds hit costs, as an ablation. Metrics always use the trace's true cost. | Hits never measure cost in production. |
| 8 | Expiry handling | Remove immediately or lazily [D§5.5] | The engine keeps an expiry heap and purges every entry with `expiry <= now` **before** each operation. Purged keys keep their telemetry as observation entries. | Expired bytes are never counted as occupied when planning, and history survives. |
| 9 | Size changes | Not specified | A request for a resident key whose size differs from the stored size is counted as a **miss** with removal reason `Replaced`, then handled through normal admission. | Simple and conservative. |
| 10 | Hit return type | `LoadResult` by value (copies) | `get_or_load` returns `ValueRef = std::shared_ptr<const Bytes>`. A hit is a refcount increment with no allocation. | Meets the "no heap allocation on hit" requirement [D§9.1]. |
| 11 | Key identity in `Cache` | string / hash | `KeyId = XXH3_64(key)`. Storage keeps the full key and verifies it on hit. On a collision the request is a miss and the new object is not admitted. | Never returns wrong data; collision cost is negligible. |
| 12 | Reproducibility | "Pin seeds" [D§10.6] | No `std::*_distribution` (their output differs between standard libraries). All randomness uses in-repo xoshiro256** and in-repo distributions (§15.1). No `-ffast-math`. Determinism is guaranteed per build (same compiler, flags and platform), not bit-for-bit across platforms. | `std::` distributions differ between libstdc++ and libc++. |
| 13 | Config format | YAML [D§13] | A restricted YAML subset (nested maps of scalars, `#` comments), parsed in-repo, plus `--set a.b.c=v` overrides. | Avoids a yaml-cpp dependency. |
| 14 | GDSF frequency | Not specified | `gdsf.frequency_source = resident_count` (classic: resets on eviction) **or** `engine_window` (same windowed F as Clache). Both are run. | Answers [D§15 Q2] directly. |
| 15 | "LFU with aging" | Not specified | Implemented as **LFU-DA** (dynamic aging; GDSF with C = S = 1). | O(log n), standard. |
| 16 | One-hit pollution / huge victim sets | Listed as risks [D§12] | Two engine knobs, both off by default: `admission.min_observations` (applies to every policy) and `admission.max_victims` (Clache only). | Makes [D§15 Q4/Q5] experiments possible. |
| 17 | Invalidation racing a load | Not specified | Each key has a generation counter. A load that finishes after an invalidation is not admitted (`REJECT_INVALIDATED_DURING_LOAD`). | Prevents caching stale data. |
| 18 | Rate denominator | "req/s over last 5 min" | `F = (count in current partial bucket + previous B−1 full buckets) / window_seconds`. The effective window is between `window − bucket` and `window`. | Exact, cheap, documented bias. |
| 19 | EWMA initialisation | Not specified | The first sample sets the EWMA directly; later samples use `α_ewma`. All samples are clamped to `[min_ms, max_ms]`. | Avoids a cold-start bias toward 0. |
| 20 | Hit-rate accounting with warm-up | Separate phases [D§10.6] | All requests change state. Metrics count only requests after the warm-up cutoff (default: first 20% of requests). | Standard practice. |

---

## 3. Architecture

### 3.1 Layers

```
                    ┌──────────────────────────────────────────────┐
  clache_sim ──────▶│                CacheEngine                    │◀────── Cache (Phase 3)
  (trace replay,    │  deterministic · single-threaded · no values  │        (thread-safe API,
   one engine per   │                                               │         storage, loader,
   run)             │  MetadataStore ── ObservationTable            │         single-flight,
                    │  FrequencyEstimator · MissCostEstimator       │         locking)
                    │  ExpiryIndex (IndexedMinHeap)                 │             │
                    │  Policy ◀── CostValuePolicy | LRU | LFU-DA |  │             ▼
                    │             GDS | GDSF | W-TinyLFU | S3-FIFO |│       StorageBackend
                    │             RedisSampledLRU/LFU               │   (InMemoryStore; Redis
                    │  EngineStats · DecisionSink (sampled)         │    adapter in Phase 4)
                    └──────────────────────────────────────────────┘
```

* **`CacheEngine`** knows keys (`KeyId`), sizes, costs, TTLs and time. It decides hits, misses, admissions, evictions and expirations. It never sees value bytes, never locks, never reads a clock (time is always passed in), and never allocates in steady state on the hit path.
* **`Cache`** (Phase 3) adds value storage, the loader call, measurement of loader time, locking, and request coalescing (single-flight).
* **`clache_sim`** replays a trace through one `CacheEngine` per (policy, capacity) run and does its own true-cost accounting.

### 3.2 Request flow through the engine

```
lookup(key, now)
  ├─ purge_expired(now)                     → expired keys reported to caller
  ├─ slot = metadata.lookup_or_observe(key) → creates observation entry if new
  ├─ telemetry.record_access(slot, now)
  ├─ policy.on_access(slot, now)
  ├─ resident? ── yes ─▶ policy.on_hit(slot, now) → HIT
  └─ no ─▶ MISS (caller loads / simulator reads trace cost)

complete_miss(key, charged_bytes, cost_ms, ttl, now)
  ├─ telemetry.observe_cost / set size
  ├─ prefilters: oversize, min_observations, generation
  ├─ bytes_needed = max(0, size − free)
  ├─ bytes_needed == 0 ─▶ admit
  ├─ else policy.plan_admission(slot, bytes_needed, now, plan) → validate
  ├─ plan.admit ─▶ evict victims (in plan order), insert candidate
  └─ else reject (no state removed)
```

---

## 4. Repository layout and build

### 4.1 Layout

Extends [D§8.2]. Files marked † are new relative to the PDF.

```
clache/
├── CMakeLists.txt
├── cmake/
│   ├── Dependencies.cmake        † FetchContent: googletest, benchmark, xxhash
│   ├── Sanitizers.cmake          †
│   └── Warnings.cmake            †
├── include/clache/
│   ├── types.h                   KeyId, SlotId, TimePoint, Nanos, enums
│   ├── config.h                  EngineConfig, CacheConfig, policy configs
│   ├── config_parser.h           † YAML-subset + --set overrides
│   ├── estimators.h              † FrequencyEstimator, MissCostEstimator
│   ├── metadata.h                † EntryMetadata, MetadataStore
│   ├── indexed_min_heap.h        † header-only template
│   ├── policy.h                  Policy, PolicyContext, AdmissionPlan, registry
│   ├── engine.h                  † CacheEngine
│   ├── cache.h                   Cache (Phase 3)
│   ├── storage.h                 StorageBackend, InMemoryStore
│   ├── telemetry.h               DecisionRecord, DecisionSink
│   ├── metrics.h                 MetricsSink, Histogram
│   └── rng.h                     † Xoshiro256ss, SplitMix64, distributions
├── src/
│   ├── core/
│   │   ├── cache_engine.cpp
│   │   ├── metadata_store.cpp
│   │   ├── observation_table.cpp
│   │   ├── cache.cpp             Phase 3
│   │   └── config_parser.cpp     †
│   ├── estimators/
│   │   ├── frequency_window.cpp  rolling buckets + forward decay
│   │   └── miss_cost_ewma.cpp
│   ├── policy/
│   │   ├── registry.cpp          † name → factory
│   │   ├── cost_value_policy.cpp Clache v0 (+ ablation switches)
│   │   ├── lru_policy.cpp
│   │   ├── lfu_policy.cpp        LFU and LFU-DA
│   │   ├── gdsf_policy.cpp       GDS and GDSF
│   │   ├── tinylfu_policy.cpp    † W-TinyLFU
│   │   ├── s3fifo_policy.cpp     † S3-FIFO
│   │   └── redis_sampled_policy.cpp † Redis approx-LRU / approx-LFU
│   ├── storage/
│   │   ├── in_memory_store.cpp
│   │   └── redis_adapter.cpp     Phase 4, behind CLACHE_WITH_REDIS
│   ├── telemetry/
│   │   ├── trace_reader.cpp      †
│   │   ├── trace_writer.cpp
│   │   └── decision_log.cpp      † NDJSON decision records
│   └── sim/                      †
│       ├── sim_runner.cpp        one run = one engine over one trace
│       └── results_writer.cpp
├── apps/
│   ├── clache_sim.cpp
│   ├── trace_generator.cpp       binary: clache_tracegen
│   └── clache_bench.cpp          Phase 3 multi-thread load driver
├── tests/
│   ├── unit/
│   ├── golden/                   † hand-computed scenarios (§19.2)
│   ├── property/                 randomized differential + invariant tests
│   └── integration/              sim end-to-end, determinism
├── benchmarks/
│   ├── micro/                    † Google Benchmark
│   ├── traces/                   generated, git-ignored (+ .meta.json kept)
│   ├── configs/                  experiment configs
│   └── scripts/
│       ├── run_matrix.sh
│       └── report.py             CSV → markdown tables + plots
└── docs/
```

### 4.2 CMake targets and options

| Target | Kind | Contents |
|---|---|---|
| `clache_core` | static lib | everything under `src/` except `sim/` and `storage/redis_adapter.cpp` |
| `clache_sim_lib` | static lib | `src/sim/` |
| `clache_sim`, `clache_tracegen`, `clache_bench` | executables | `apps/` |
| `clache_tests` | executable | GoogleTest; registered with CTest |
| `clache_micro` | executable | Google Benchmark |

| Option | Default | Effect |
|---|---|---|
| `CLACHE_BUILD_TESTS` | ON | build tests |
| `CLACHE_BUILD_BENCHMARKS` | ON | build micro benchmarks |
| `CLACHE_SANITIZE` | "" | `address`, `undefined`, `thread` (mutually exclusive with each other except address+undefined) |
| `CLACHE_CHECK_INVARIANTS` | OFF (ON in Debug) | full invariant check after every engine operation (§19.4) |
| `CLACHE_WITH_REDIS` | OFF | builds Redis adapter (needs hiredis) |

Compiler flags: `-std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` (warnings-as-errors in CI only). `-ffast-math` and `-Ofast` are **forbidden** (they would break determinism and `-inf` handling). Release builds use `-O2 -g`.

### 4.3 Dependencies

| Dependency | Use | How |
|---|---|---|
| GoogleTest | tests | FetchContent, pinned tag |
| Google Benchmark | micro benchmarks | FetchContent, pinned tag |
| xxHash | `XXH3_64bits` for key hashing, trace/config fingerprints | FetchContent, header-only mode |
| hiredis | Phase 4 only | system package, optional |

There are no other runtime dependencies. Python 3 with `pandas` and `matplotlib` is used only by `benchmarks/scripts/report.py`.

### 4.4 CI

GitHub Actions matrix:

* `{ubuntu-latest: gcc-13, clang-17} × {Debug, Release}` and `macos-latest: AppleClang × Release`
* Sanitizer jobs: ASan+UBSan (Debug), and TSan (Phase 3 tests only).
* Every job runs `ctest`. Release jobs also run the determinism integration test (§19.5).

---

## 5. Core types

`include/clache/types.h`:

```cpp
namespace clache {

using KeyId  = std::uint64_t;   // trace key_id, or XXH3_64(key) in Cache
using SlotId = std::uint32_t;   // dense index into MetadataStore; recycled
inline constexpr SlotId kNoSlot = std::numeric_limits<SlotId>::max();

using Nanos = std::chrono::nanoseconds;

// Engine time: nanoseconds since an arbitrary epoch. The simulator uses trace
// timestamps; Cache uses steady_clock. Never wall-clock.
struct TimePoint {
  Nanos since_epoch{0};
  friend auto operator<=>(const TimePoint&, const TimePoint&) = default;
  friend TimePoint operator+(TimePoint t, Nanos d) { return {t.since_epoch + d}; }
  friend Nanos operator-(TimePoint a, TimePoint b) { return a.since_epoch - b.since_epoch; }
};
inline constexpr TimePoint kNever{Nanos::max()};

enum class RemovalReason : std::uint8_t { Evicted, Expired, Invalidated, Replaced };

enum class Decision : std::uint8_t {
  Hit,
  AdmitFreeSpace,                        // fit without eviction
  AdmitWithEviction,
  RejectOversized,                       // size > max_object_bytes or > capacity
  RejectInsufficientHistory,             // access_count < admission.min_observations
  RejectCandidateNotHigher,              // CANDIDATE_VALUE_NOT_HIGHER_THAN_REQUIRED_VICTIMS
  RejectTooManyVictims,                  // admission.max_victims exceeded
  RejectInvalidatedDuringLoad,
  RejectHashCollision,                   // Cache only
  RejectPolicy,                          // generic, for policies with their own rule
};
std::string_view to_string(Decision);  // e.g. "CANDIDATE_VALUE_NOT_HIGHER_THAN_REQUIRED_VICTIMS"

}  // namespace clache
```

Units used everywhere inside the engine:

| Quantity | Unit | Type |
|---|---|---|
| time | ns (`TimePoint`) | `int64` |
| frequency `F` | requests / second | `double` |
| miss cost `C` | milliseconds | `double` |
| size `S` | bytes | `uint64_t` |
| value `V` | ms · s⁻¹ · byte⁻α | `double` |

The trace stores cost in integer microseconds. Conversion is `cost_ms = miss_cost_us / 1000.0`.

---

## 6. Estimators

### 6.1 Frequency estimator

`FrequencyEstimator` is a stateless object (holding only config and `T0`) that operates on a per-key `FrequencyState` stored in `EntryMetadata`.

```cpp
struct FrequencyState {                 // 32 bytes
  double   log_mass = -std::numeric_limits<double>::infinity();  // decay mode
  std::uint32_t last_epoch = 0;          // bucket mode: epoch of most recent record
  std::array<std::uint16_t, 8> buckets{};// bucket mode ring, index = epoch % B, saturating
  std::uint32_t pad_ = 0;
};

class FrequencyEstimator {
 public:
  enum class Mode { RollingBuckets, ForwardDecay };
  FrequencyEstimator(const FrequencyConfig&, TimePoint t0);
  void   record(FrequencyState&, TimePoint now) const;
  double rate(const FrequencyState&, TimePoint now) const;       // F in req/s
  double log_rate_key(const FrequencyState&, TimePoint now) const;// see §6.1.3
  std::uint32_t epoch(TimePoint now) const;                       // bucket mode
  Mode mode() const;
};
```

`T0` is the engine's time origin: the timestamp of the first operation the engine sees.

#### 6.1.1 Rolling buckets (default for v0)

* Config: `window_seconds` (W, default 300) and `bucket_seconds` (b, default 60). They MUST satisfy `W % b == 0` and `B = W / b ∈ [1, 8]`.
* `epoch(t) = floor((t − T0) / b)`.
* `record(s, t)`: let `e = epoch(t)`.
  * If this is the state's first record, or `e − s.last_epoch ≥ B`, zero all buckets.
  * Otherwise zero the buckets for epochs in `(s.last_epoch, e]`.
  * Then `buckets[e % B] = min(buckets[e % B] + 1, 65535)` and `s.last_epoch = e`.
* `rate(s, t)` (const; no mutation): let `e = epoch(t)`.
  * If the state has never been recorded, or `e − s.last_epoch ≥ B`, the count is 0.
  * Otherwise sum the buckets for epochs `e − B + 1 … s.last_epoch`.
  * Return `count / W`.
* Between epoch boundaries, `rate` of a key that is not accessed is **constant**. This is why heap keys stay exact until the next boundary (§10.3).

Saturation at 65535 per bucket caps measurable rate at ~1092 req/s per key per bucket at b = 60. That is enough for ranking: any key at the cap is far above every eviction candidate. Saturation is counted (`freq_saturations_total`).

#### 6.1.2 Count on first touch

The request that creates or refreshes a key's metadata is recorded **before** any decision. So a first-seen candidate has count 1 [D§5.3].

#### 6.1.3 Forward decay (production candidate)

* Config: `decay_tau_seconds` (τ, default = `window_seconds`).
* Define `u(t) = (t − T0) / τ` as a `double`, in units of τ.
* Exponentially decayed rate: `F(t) = (1/τ) · Σ_k exp(−(t − t_k)/τ)`. For a steady Poisson stream of rate r, its expectation is r, so it is unbiased in steady state.
* Store `log_mass L = ln Σ_k exp(u(t_k))`. `record(s, t)`: `L ← logaddexp(L, u(t))`, where
  `logaddexp(a, b) = max(a,b) + log1p(exp(−|a−b|))` and `logaddexp(−inf, b) = b`.
* `rate(s, t) = exp(L − u(t)) / τ`.
* **Time-invariant key:** `ln F(t) = L − ln τ − u(t)`. Every key shares the same `−u(t)` term at a given `t`, so ranking by `L − ln τ` is equal to ranking by `F(t)`. `log_rate_key(s, t)` returns `L − ln τ` in decay mode and `ln rate(s, t)` in bucket mode.
* Precision: after 30 days with τ = 300 s, `u ≈ 8640`, and the double ulp there is about 1.8e−12. That is negligible. No renormalisation is needed. Engines running longer than 10⁶ τ MUST be restarted or rebased; this is an assertion, not a supported mode.

### 6.2 Miss-cost estimator

```cpp
struct CostState { double ewma_ms = 0.0; std::uint32_t samples = 0; };  // 16 bytes padded

class MissCostEstimator {
 public:
  explicit MissCostEstimator(const MissCostConfig&);
  void   observe(CostState&, double observed_ms) const;
  double estimate_ms(const CostState&) const;   // clamped; see below
};
```

* `observe`: `x = clamp(observed_ms, min_ms, max_ms)`. If `samples == 0`, set `ewma = x`; otherwise `ewma = α·x + (1−α)·ewma`. Then `samples = min(samples+1, UINT32_MAX)`.
* `estimate_ms`: if `samples == 0`, return `default_ms` (config, default = `min_ms`). Otherwise return `ewma`.
* `min_samples` (default 1): if `samples < min_samples`, `estimate_ms` returns `min(ewma, prior_ms)` where `prior_ms` defaults to `default_ms`. This is the "minimum sample count" mitigation from [D§12]. It stays off at the default value of 1.
* **Cost hints:** if a request carries a hint (the `LoadResult::cost_hint_ms` in `Cache`, or `--cost-oracle` in the simulator), the hint is passed to `observe` **instead of** the measured loader latency.

### 6.3 Charged size

* **Simulator:** `charged = size_bytes + sim.per_entry_overhead_bytes` (default 0).
* **Cache:** `charged = StorageBackend::charged_size(key, value)`. For `InMemoryStore` this is `key.size() + value.size() + per_entry_overhead_bytes` (default 64, an estimate of node, refcount and metadata bytes). `MetadataStore` per-key metadata is reported separately as `metadata_bytes` and is **not** charged against `capacity_bytes` in v0. Whether to charge it is measured in Phase 2 (§24).
* The value formula uses `max(1, charged)`.

---

## 7. Metadata store and observation table

### 7.1 EntryMetadata

```cpp
struct EntryMetadata {
  KeyId          key = 0;
  std::uint64_t  charged_bytes = 0;
  FrequencyState freq;                 // 32 B
  CostState      cost;                 // 16 B
  TimePoint      last_access{};
  std::uint64_t  last_access_seq = 0;  // engine request sequence number; unique
  TimePoint      expiry = kNever;
  std::uint32_t  access_count = 0;     // lifetime accesses since slot creation (saturating)
  std::uint32_t  generation = 0;       // bumped by invalidate()
  SlotId         obs_prev = kNoSlot;   // intrusive observation list
  SlotId         obs_next = kNoSlot;
  bool           resident = false;
  bool           in_use = false;
};
```

The target is ≤ 128 bytes per slot (checked by `static_assert`). Slots live in a `std::vector<EntryMetadata>`. The index is `std::unordered_map<KeyId, SlotId>` with `reserve()` sized from config. Freed slots go onto a LIFO free list, so slot reuse is deterministic.

### 7.2 MetadataStore API

```cpp
class MetadataStore {
 public:
  explicit MetadataStore(const ObservationConfig&);
  SlotId find(KeyId) const;                                   // kNoSlot if untracked
  // Returns slot for key, creating a non-resident observation entry if needed.
  // May drop the oldest observation entries to respect bounds (never residents).
  SlotId lookup_or_observe(KeyId, TimePoint now);
  EntryMetadata&       operator[](SlotId);
  const EntryMetadata& operator[](SlotId) const;
  void mark_resident(SlotId);        // unlinks from observation list
  void mark_nonresident(SlotId, TimePoint now); // links at observation MRU
  std::size_t resident_count() const;
  std::size_t observed_count() const;
  std::uint64_t metadata_bytes() const;  // §7.4
};
```

### 7.3 Observation table semantics

* The observation table is the set of slots with `in_use && !resident`. They are linked in an intrusive doubly-linked list ordered by most recent *touch*. A touch is creation, an access, or a transition to nonresident.
* **Count bound:** after inserting into the list, while `observed_count > max_entries`, drop the LRU tail. Dropping means: erase from the index, zero the slot, and push it on the free list.
* **TTL:** in `lookup_or_observe`, if the found slot is nonresident and `last_access + ttl ≤ now`, its telemetry is **reset** (frequency, cost and access count cleared; generation kept) before the access is recorded. The engine also drops stale tail entries opportunistically: on each `lookup_or_observe`, at most 2 tail entries with `last_access + ttl ≤ now` are dropped. This bounds the work per call.
* Evicted, expired and invalidated residents become observation entries (ghosts) and **keep** their telemetry. Exception: `Invalidated` resets the cost state, because the old cost may describe stale content.
* `max_entries = 0` disables observation. Nonresident slots are then dropped as soon as the request finishes. Telemetry for rejected keys is lost, which matches a "no ghost" ablation.

### 7.4 Metadata accounting

`metadata_bytes()` = `slots.capacity() * sizeof(EntryMetadata)` + `index.bucket_count() * sizeof(void*)` + `index.size() * (sizeof(std::pair<const KeyId,SlotId>) + 2*sizeof(void*))`.

Every policy also reports `metadata_bytes()`. The simulator records the peak of the sum and the per-resident-key average [D§10.4].

---

## 8. Indexed min-heap

`include/clache/indexed_min_heap.h`, header-only:

```cpp
// Binary min-heap over SlotIds with O(1) position lookup.
// Order: lexicographic (key, seq, id) ascending; all three stored inline.
class IndexedMinHeap {
 public:
  struct Node { double key; std::uint64_t seq; std::uint64_t id; SlotId slot; };
  void   reserve(std::size_t n);
  bool   contains(SlotId) const;
  void   push(SlotId, double key, std::uint64_t seq, std::uint64_t id);
  void   update(SlotId, double key, std::uint64_t seq);  // sift up or down
  void   erase(SlotId);
  const Node& top() const;
  std::size_t size() const;
  // Rebuild: caller rewrites keys via mutable_nodes(), then heapify() in O(n).
  std::span<Node> mutable_nodes();
  void   heapify();
  // Visit nodes in ascending order without mutating the heap.
  // Uses an auxiliary min-heap of indices (best-first traversal); O(k log k)
  // for k visited nodes. Visitor returns false to stop.
  template <class F> void for_each_ascending(F&& visit) const;
  bool   check_invariants() const;  // for tests / CLACHE_CHECK_INVARIANTS
 private:
  std::vector<Node>    nodes_;
  std::vector<std::uint32_t> pos_;  // indexed by SlotId; UINT32_MAX = absent
  mutable std::vector<std::uint32_t> scratch_;  // for_each_ascending frontier
};
```

* Comparisons use `key` first. `-inf` is valid and sorts first; NaN is a bug, checked with an assertion. Ties break on `seq` (older access first), then `id` (smaller `KeyId` first).
* `for_each_ascending` starts with the root in the frontier, repeatedly pops the smallest frontier node, visits it, and pushes its two children. The scratch vector keeps its capacity, so steady-state calls do not allocate.
* A 4-ary heap MAY replace the binary heap in Phase 2 if micro benchmarks show a win. The interface does not change.

The same class is used for the Clache value index, the GDS/GDSF/LFU index, and the engine's expiry index (key = expiry ns as double, exact up to 2⁵³ ns ≈ 104 days of engine time). The expiry index MUST switch to an integer-key template instantiation if engine time can exceed that; in v0 this is an assertion.

---

## 9. Policy interface

`include/clache/policy.h`:

```cpp
struct PolicyContext {
  const MetadataStore*      meta;
  const FrequencyEstimator* freq;
  const MissCostEstimator*  cost;
  std::uint64_t             capacity_bytes;
  std::uint64_t             seed;          // for policies that sample (Redis*)
  std::uint64_t             expected_entries; // sizing hint (TinyLFU sketch)
};

struct AdmissionPlan {
  bool      admit = false;
  Decision  decision = Decision::RejectPolicy;
  std::vector<SlotId> victims;            // eviction order; engine clears before call
  std::uint64_t victim_bytes = 0;
  double    candidate_value = std::numeric_limits<double>::quiet_NaN();
  double    threshold_value = std::numeric_limits<double>::quiet_NaN();
};

class Policy {
 public:
  virtual ~Policy() = default;
  virtual std::string_view name() const = 0;

  // Every request (hit or miss), after telemetry is recorded, before anything else.
  virtual void on_access(SlotId, TimePoint now) {}
  // Resident key was hit.
  virtual void on_hit(SlotId, TimePoint now) = 0;
  // Non-resident candidate needs `bytes_needed` > 0 bytes freed. Fill `plan`.
  virtual void plan_admission(SlotId candidate, std::uint64_t bytes_needed,
                              TimePoint now, AdmissionPlan& plan) = 0;
  // Candidate admitted (after all victims were removed).
  virtual void on_insert(SlotId, TimePoint now) = 0;
  // Resident removed for any reason. Slot metadata is still valid during the call.
  virtual void on_remove(SlotId, RemovalReason) = 0;
  // Resident's cost estimate changed via hint (no access recorded).
  virtual void on_cost_update(SlotId, TimePoint now) {}
  // Current policy score for explanations; NaN if not meaningful.
  virtual double value(SlotId, TimePoint now) const { return std::numeric_limits<double>::quiet_NaN(); }
  virtual std::uint64_t metadata_bytes() const = 0;
  // Invariant self-check (tests / CLACHE_CHECK_INVARIANTS).
  virtual bool check_invariants() const { return true; }
};

using PolicyFactory = std::function<std::unique_ptr<Policy>(const PolicyConfig&, const PolicyContext&)>;
std::unique_ptr<Policy> make_policy(std::string_view name, const PolicyConfig&, const PolicyContext&);
std::vector<std::string_view> registered_policies();
```

### 9.1 Contract

1. Policies hold per-entry state **only for resident slots**. State is kept in vectors indexed by `SlotId`, grown on demand, and cleared in `on_remove`. Policies with key-level history (W-TinyLFU sketch, S3-FIFO ghost queue) key it by `KeyId`, not `SlotId`, because slots are recycled.
2. `plan_admission` is only called when `bytes_needed > 0`, `size ≤ capacity`, and the prefilters passed. `bytes_needed ≤ resident bytes` always holds.
3. If `plan.admit == false`, `plan.victims` MUST be empty. A rejected plan never removes anything (atomicity, [D§16.1]).
4. If `plan.admit == true`, victims MUST be distinct, resident, not the candidate, and sum to at least `bytes_needed`. The engine checks this. A violation is a fatal assertion in Debug and `std::logic_error` in Release.
5. **Always-admit policies** (LRU, LFU, LFU-DA, GDS, GDSF, Redis*, W-TinyLFU, S3-FIFO) MAY commit internal state changes during `plan_admission`: queue moves, aging-value updates, unlinking victims from internal lists. In that case `on_remove(victim, Evicted)` MUST tolerate a victim that is already detached.
6. **Policies that may reject** (Clache) MUST NOT mutate any state in `plan_admission`, except scratch buffers.
7. Policies MUST be deterministic given the same call sequence and `seed`.

---

## 10. Clache v0 policy (CostValuePolicy)

### 10.1 Score

For slot `i` at time `t`:

```
log_f = freq.log_rate_key(i.freq, t)                 // §6.1.3
log_c = ln(cost.estimate_ms(i.cost))                 // or 0 if ablation use_cost=false
log_s = α · ln(max(1, i.charged_bytes))              // α = value.size_exponent
K_i   = log_f + log_c − log_s
```

* Bucket mode: `K_i = ln V_i(t)`, valid until the next epoch boundary.
* Decay mode: `K_i = ln V_i(t) + u(t)`, valid forever until the slot's own telemetry changes.
* A frequency of zero (bucket mode, idle for a full window) gives `K = −inf`.
* For explanations: `V_i(t) = exp(K_i)` in bucket mode and `exp(K_i − u(t))` in decay mode.

### 10.2 State

* `IndexedMinHeap heap_` over resident slots, with node `(K_i, last_access_seq, key)`.
* `std::uint32_t rebuilt_epoch_` (bucket mode).
* `std::vector<SlotId> scratch_` (unused if the heap's own scratch suffices).

### 10.3 Hooks

| Hook | Behaviour |
|---|---|
| `on_access(slot, t)` | **Bucket mode:** if `freq.epoch(t) != rebuilt_epoch_`, recompute `K` for every node in `heap_.mutable_nodes()`, call `heapify()`, and set `rebuilt_epoch_`. This is O(n) once per bucket (default every 60 s of trace time). Decay mode: no-op. |
| `on_hit(slot, t)` | Recompute `K_slot` and `heap_.update(slot, K, seq)`. |
| `on_insert(slot, t)` | Compute `K_slot` and push it. |
| `on_remove(slot, _)` | `heap_.erase(slot)`. |
| `on_cost_update(slot, t)` | Recompute and update. |
| `value(slot, t)` | `V` as in §10.1. Nonresident slots are computed on demand. |

`on_access` runs before `on_hit`, `plan_admission` and the engine's eviction steps in the same request. So the heap is always exact when it is used.

### 10.4 Admission (plan_admission)

```
K_c    = score(candidate, t)
plan.candidate_value = V(K_c)
bytes  = 0
blocked = false
heap_.for_each_ascending(node):
    if node.key >= K_c:                     # strictly-lower rule; compares K only
        plan.threshold_value = V(node.key); blocked = true; stop
    if max_victims > 0 and |victims| == max_victims:
        plan.decision = RejectTooManyVictims; plan.threshold_value = V(node.key); stop & reject
    victims.push(node.slot); bytes += charged(node.slot)
    if bytes >= bytes_needed: stop
if blocked or bytes < bytes_needed:
    victims.clear(); admit = false; decision = RejectCandidateNotHigher
else:
    admit = true; decision = AdmitWithEviction; victim_bytes = bytes
    threshold_value = V(key of last victim)
```

`bytes < bytes_needed` without being blocked cannot happen, because `size ≤ capacity` ⇒ `bytes_needed ≤ resident bytes`. It is an assertion. It still maps to a reject in Release.

The comparison uses only `K`, not the `(seq, id)` tie-breakers, so an exact value tie rejects [D§5.2]. The tie-breakers only decide which of several equal-valued victims is visited first.

### 10.5 Ablation switches (config `clache.*`)

These are implemented in the same class and registered as separate policy names, so they appear in result tables:

| Registered name | Switch | Purpose |
|---|---|---|
| `clache` | defaults | v0 policy |
| `clache_noadmit` | `admission_rule = always` (walk the lowest values until the object fits, ignoring the comparison) | isolates the effect of the admission gate |
| `clache_nocost` | `use_cost = false` | F / S only: is cost signal worth it? |
| `clache_nosize` | `size_exponent = 0` | F × C only |
| `clache_decay` | `frequency.mode = forward_decay` | estimator comparison |
| `clache_noghost` | `observation.max_entries = 0` | value of the observation table |

---

## 11. Baseline policies

Every baseline runs through the same engine. That means the same capacity accounting, TTL handling, prefilters and metrics. All are always-admit unless stated.

### 11.1 `lru`

An intrusive doubly linked list over resident slots (per-slot `prev/next` vectors). On hit, move to the head. The plan walks from the tail, adding victims until `bytes_needed` is covered. On insert, add at the head.

### 11.2 `lfu` and `lfu_da`

* Per-resident count `n`, reset on insertion to 1 and incremented on hit.
* `lfu`: heap key `n`; ties broken LRU-first via `seq`.
* `lfu_da`: heap key `L + n`, where `L` (initially 0) is set to the key of each evicted victim when it is removed (`L = max(L, key)`). Plan: walk ascending, take victims until covered.

### 11.3 `gds` and `gdsf`

* Heap key `H = L + Fr · C / S` (`gds`: `Fr = 1`). `C` = engine cost estimate (ms). `S` = charged bytes (exponent `gdsf.size_exponent`, default 1).
* `Fr`:
  * `gdsf.frequency_source = resident_count` (default; classic Cherkasova): count of accesses since insertion.
  * `engine_window`: `F_i` from the engine's estimator, the same telemetry Clache sees. Registered as `gdsf_window`.
* `H` is recomputed with the current `L` on hit and on insert.
* Plan: walk ascending until covered. In `on_remove(Evicted)`: `L = max(L, H_victim)`.

### 11.4 `redis_lru` and `redis_lfu` (approximations of Redis allkeys-*)

These model Redis's *policy*, not its memory accounting.

* Keep a dense `std::vector<SlotId>` of residents with a position index (swap-remove), so uniform random sampling is O(1).
* RNG: `Xoshiro256ss` seeded from `PolicyContext::seed`.
* `maxmemory-samples` (default 5) and an eviction pool of 16, as in Redis.
  * **Plan loop:** sample `samples` residents; insert them into the pool ordered by idle score (LRU: `now − last_access`; LFU: `255 − counter`), keeping the best 16. Evict the entry with the highest score from the pool; drop pool entries that are no longer resident. Repeat until covered.
* `redis_lfu` counter: 8-bit logarithmic counter with initial value 5.
  * Increment with probability `1 / ((counter − 5) · lfu_log_factor + 1)`, `lfu_log_factor = 10`.
  * Decay: subtract `floor(elapsed_minutes / lfu_decay_time)` on access, `lfu_decay_time = 1`.
* Phase 4 MUST check these against real Redis on the same trace (§21). If miss ratios differ by more than 2 percentage points, document the reason.

### 11.5 `wtinylfu`

Byte-capacity W-TinyLFU, after Caffeine:

* **Regions:** window LRU = `tinylfu.window_fraction` (default 0.01) of capacity. Main SLRU = the rest: protected = 0.80 of main, probation = 0.20 of main. All budgets are in bytes.
* **Frequency sketch:** Count-Min with 4 rows of 4-bit counters. Width = `next_pow2(expected_entries)`, where `expected_entries = capacity / mean_object_size`; the simulator computes the mean in its trace pre-pass. The doorkeeper is off in v0.
  * On every access, increment (conservative update).
  * After `10 × width` increments, halve all counters (reset).
  * Hashes: `XXH3_64(key, seed_r)` per row.
* **Insert:** new keys enter the window MRU.
* **Hit:** window → move to MRU. Probation → promote to protected MRU; if protected is over budget, demote its LRU to probation MRU. Protected → move to MRU.
* **Plan** (the new key is always admitted to the window). Repeat until `freed ≥ bytes_needed`:
  1. If `window_bytes + new_size > window_budget` and the window is not empty: let `c` be the window LRU. Detach it and let it compete:
     * Let `v` be the probation LRU, or the protected LRU if probation is empty. If main is empty, move `c` to probation and continue.
     * If `sketch(c) > sketch(v)`: `v` is a victim; move `c` to probation MRU. Otherwise `c` is a victim.
  2. Otherwise: take the probation LRU (or protected LRU) as a victim.
* Caffeine's random admission of warm candidates (anti hash-flooding) is omitted for determinism. This is documented in results.

### 11.6 `s3fifo`

After Yang et al., SOSP '23:

* Small FIFO `S` = `s3fifo.small_fraction` (default 0.10) of capacity bytes. Main FIFO `M` = the rest.
* Ghost FIFO `G` holds `KeyId`s only, bounded by **entry count** equal to `M`'s current entry count (minimum 1). Membership is tracked in a hash set.
* Per resident: `freq ∈ [0,3]`, incremented on hit (saturating).
* **Insert:** if the key is in `G`, remove it from `G` and insert at the head of `M`. Otherwise insert at the head of `S`.
* **Plan**, until covered:
  * If `S_bytes ≥ S_budget` or `M` is empty → evict from `S`: tail `t`.
    * If `t.freq ≥ s3fifo.move_to_main_threshold` (default 1, i.e. accessed at least once while in `S`): move `t` to the head of `M` with `freq = 0` (internal move; not a victim).
    * Otherwise `t` is a victim; push `t.key` into `G`.
  * Otherwise evict from `M`: tail `t`. If `t.freq > 0`: `t.freq -= 1` and reinsert at the head of `M`. Otherwise `t` is a victim.
  * The loop terminates because every reinsertion lowers some `freq`.

### 11.7 Deferred baselines

LeCaR and CACHEUS [D§10.2] are Phase 2+ research runs. They are run through libCacheSim with the trace converted by `clache_sim --export libcachesim` (§14.6), not reimplemented. An offline optimal bound (e.g. a size-aware Belady variant) MAY be added later as a reference line. It is not a v0 requirement.

---

## 12. CacheEngine: the deterministic decision core

### 12.1 Configuration

```cpp
struct EngineConfig {
  std::uint64_t capacity_bytes;          // required, > 0
  std::uint64_t max_object_bytes;        // 0 = capacity_bytes
  FrequencyConfig   frequency;
  MissCostConfig    miss_cost;
  ObservationConfig observation;
  AdmissionConfig   admission;           // min_observations (1), max_victims (0 = unlimited)
  ValueConfig       value;               // size_exponent (1.0)
  TelemetryConfig   telemetry;           // decision_sample_rate, seed
  std::uint64_t     seed = 0;
};
```

### 12.2 API

```cpp
struct LookupResult {
  bool hit;
  SlotId slot;
  std::uint32_t generation;               // pass back to complete_miss
  std::span<const KeyId> expired;         // keys purged during this call (erase from storage)
  std::span<const KeyId> replaced;        // resident removed due to size change
};

struct AdmissionResult {
  Decision decision;
  bool admitted;
  std::span<const KeyId> evicted;         // erase these from storage
  double candidate_value, threshold_value;
};

class CacheEngine {
 public:
  CacheEngine(EngineConfig, std::string_view policy_name, const PolicyConfig&,
              DecisionSink* decisions = nullptr);
  // Step 1. Records the access. `expected_bytes` lets the simulator detect size changes.
  LookupResult lookup(KeyId, TimePoint now,
                      std::optional<std::uint64_t> expected_bytes = std::nullopt);
  // Step 2 (miss only). Must follow lookup() of the same key; other keys' calls
  // may interleave (Cache releases its lock while loading).
  AdmissionResult complete_miss(KeyId, std::uint64_t charged_bytes, double cost_ms,
                                std::optional<Nanos> ttl, std::uint32_t generation,
                                TimePoint now);
  // Removes a resident (if any), bumps generation, resets cost state.
  std::span<const KeyId> invalidate(KeyId, TimePoint now);
  // Applies a cost observation to a key without recording an access.
  void observe_cost_hint(KeyId, double cost_ms, TimePoint now);

  const EngineStats& stats() const;
  std::uint64_t bytes_resident() const;
  std::uint64_t metadata_bytes() const;   // MetadataStore + policy + expiry index
  std::optional<KeyExplanation> explain(KeyId, TimePoint now) const;
  bool check_invariants(TimePoint now) const;
};
```

Spans point into engine-owned scratch vectors. They are valid until the next non-const call. `now` MUST be non-decreasing across calls; a decrease is a `std::invalid_argument`. In `Cache`, `steady_clock` guarantees this under the engine lock.

### 12.3 `lookup` algorithm

```
1  check now >= last_now; last_now = now; seq += 1
2  if first call: T0 = now (estimators initialised lazily)
3  purge_expired(now):
       while expiry_heap non-empty and top.expiry <= now:
           slot = top.slot; remove_resident(slot, Expired)  → expired_out.push(key)
4  slot = meta.lookup_or_observe(key, now)
5  m = meta[slot]; freq.record(m.freq, now); m.last_access = now;
   m.last_access_seq = seq; m.access_count += 1 (saturating)
6  policy.on_access(slot, now)
7  if m.resident and expected_bytes and *expected_bytes + overhead != m.charged_bytes:
       remove_resident(slot, Replaced) → replaced_out.push(key); stats.size_changes += 1
8  if m.resident:
       policy.on_hit(slot, now); stats.hits += 1; maybe_record(Hit); return hit
9  stats.misses += 1; return miss
```

`remove_resident(slot, reason)`: call `policy.on_remove(slot, reason)`, then erase from the expiry heap. Set `bytes_resident -= charged`, `meta.mark_nonresident(slot, now)`, and `m.expiry = kNever`. Increment the counter for `reason`.

### 12.4 `complete_miss` algorithm

```
1  check now monotonic; purge_expired(now)
2  slot = meta.find(key); assert slot != kNoSlot && !meta[slot].resident
       (if resident — possible only if a concurrent path admitted it — return
        RejectPolicy with no change; Cache's single-flight makes this unreachable)
3  m.charged_bytes = charged_bytes
4  if cost_ms is finite and >= 0: cost.observe(m.cost, cost_ms)   (hint already substituted by caller)
5  if generation != m.generation          → reject(RejectInvalidatedDuringLoad)
6  if charged_bytes > max_object_bytes or charged_bytes > capacity → reject(RejectOversized)
7  if m.access_count < admission.min_observations → reject(RejectInsufficientHistory)
8  if ttl and *ttl <= 0                    → reject(RejectPolicy)   // already expired
9  free = capacity − bytes_resident; needed = charged_bytes > free ? charged_bytes − free : 0
10 if needed == 0: admit(slot, AdmitFreeSpace, ttl)
11 else:
       plan.reset(); policy.plan_admission(slot, needed, now, plan); validate(plan, needed)
       if !plan.admit: reject(plan.decision)
       else: for v in plan.victims: remove_resident(v, Evicted) → evicted_out.push(key(v))
             admit(slot, AdmitWithEviction, ttl)
12 maybe_record(decision, plan); return result
```

`admit(slot, …, ttl)`:

1. `meta.mark_resident(slot)`; `bytes_resident += charged`.
2. If `ttl`: `m.expiry = now + *ttl` and push to the expiry heap.
3. `policy.on_insert(slot, now)`; `stats.admissions += 1`.

`reject(…)`: the slot stays in the observation table; `stats.admission_rejections += 1`.

Invariant after every public call: `bytes_resident ≤ capacity_bytes`. There is no transient overshoot: victims are removed before the candidate is inserted [D§16.1].

### 12.5 Stats

`EngineStats` holds plain `uint64_t` counters matching [D§11.1]:

* `requests, hits, misses, admissions, admission_rejections` (plus a per-`Decision` breakdown array)
* `evictions, expirations, invalidations, size_changes, bytes_evicted`
* `score_updates`, `heap_rebuilds`, `freq_saturations`
* `origin_cost_ms_total` (Σ observed miss cost)
* `origin_cost_ms_saved_estimate` (Σ `estimate_ms` at hit time)

---

## 13. Trace formats

### 13.1 CSV (canonical, human-readable)

```
timestamp_ns,key_id,size_bytes,miss_cost_us,ttl_s[,region][,op]
1000,42,4096,240000,60
1200,7,131072,5000,300
1300,42,4096,240000,60
```

| Column | Type | Rules |
|---|---|---|
| `timestamp_ns` | u64 | non-decreasing; the first row MAY be any value |
| `key_id` | u64 decimal | |
| `size_bytes` | u32 | > 0 for `get` |
| `miss_cost_us` | u32 | cost if this request misses; ≥ 0 |
| `ttl_s` | u32 | 0 = no TTL |
| `region` | u16 | optional; default 0 |
| `op` | `get` \| `del` | optional; default `get`. `del` ignores size/cost/ttl |

* The header row is required. Lines starting with `#` are comments.
* Any violation fails with `file:line: message`. There is no silent skipping.

### 13.2 Binary (`.ctrace`, for large traces)

* Header (64 bytes): magic `"CLACHETR"`, `u32 version = 1`, `u32 record_size = 32`, `u64 record_count`, `u64 flags`, then reserved bytes.
* Records, little-endian, 32 bytes each: `u64 timestamp_ns, u64 key_id, u32 size_bytes, u32 miss_cost_us, u32 ttl_s, u16 region, u8 op, u8 reserved`.
* `clache_tracegen --format bin` writes this format. `clache_sim` detects it by magic.
* Round-trip CSV ↔ bin is tested.

### 13.3 Trace metadata sidecar

Every generated trace has `<trace>.meta.json` containing: generator version, git SHA, workload name, all parameters, seed, request count, unique keys, working-set bytes, and the XXH3-64 fingerprint of the trace file.

---

## 14. Simulator: clache_sim

### 14.1 CLI

```
clache_sim --trace PATH
           [--policies lru,lfu_da,gds,gdsf,gdsf_window,wtinylfu,s3fifo,clache,...]  (default: all phase-1 policies)
           [--capacity BYTES[,BYTES...] | --capacity-fractions 0.001,0.01,0.05,0.1,0.2]
           [--config FILE.yaml] [--set key=value ...]
           [--warmup-fraction 0.2]
           [--cost-oracle]
           [--per-entry-overhead BYTES]
           [--domain-by none|region]
           [--jobs N]                         # parallel runs; each run single-threaded
           [--out results.csv] [--timeseries ts.csv --timeseries-every 1000000]
           [--decisions decisions.ndjson --decision-sample-rate 0.001]
           [--stats-only] [--export libcachesim]
```

Capacities accept suffixes `KiB`, `MiB` and `GiB`. Fractions are relative to the trace's **working-set bytes** (Σ over unique keys of the maximum size seen).

### 14.2 Execution

1. **Load the trace** into memory, struct-of-arrays, shared read-only across runs.
2. **Pre-pass:** unique keys, working-set bytes, mean object size, total potential cost (Σ `miss_cost_us` over `get`s), one-hit-wonder fraction (keys requested exactly once), regions present. Printed and written to the results file.
3. **Build the run list** = policies × capacities (× domains). Run it on a pool of `--jobs` threads. Each run owns its `CacheEngine` (seed = `config.seed`), so the results do not depend on `--jobs`.
4. **Per request `r`:**
   * `get`: `res = engine.lookup(key, t, size + overhead)`.
     * On a miss: `engine.complete_miss(key, size + overhead, cost_us/1000.0, ttl, res.generation, t)`.
     * On a hit with `--cost-oracle`: `engine.observe_cost_hint(key, cost_us/1000.0, t)`.
   * `del`: `engine.invalidate(key, t)`.
   * If `r ≥ warmup_cutoff`, accumulate the **sim-side metrics** using the trace's true `miss_cost_us`.
5. Each run is wall-clock timed. `ns_per_request = wall / requests` (this includes simulator overhead; precise policy cost comes from micro benchmarks, §20.1).

### 14.3 Sim-side metrics (post-warm-up only)

| Metric | Definition |
|---|---|
| `requests` | count of `get` |
| `hit_rate` | hits / requests |
| `byte_hit_rate` | Σ size over hits / Σ size over requests |
| `total_miss_cost_us` | Σ `miss_cost_us` over misses (**primary**) |
| `cost_hit_rate` | Σ `miss_cost_us` over hits / Σ `miss_cost_us` over requests |
| `admissions`, `rejections`, `evictions`, `expirations` | from engine counter deltas |
| `mean_victims_per_eviction_admit`, `max_victims` | over `AdmitWithEviction` decisions |
| `metadata_bytes_peak`, `metadata_bytes_per_resident` | sampled every 65 536 requests |

### 14.4 Output: results.csv

One row per run, with columns in this order:

```
clache_version,git_sha,trace_path,trace_fingerprint,config_fingerprint,policy,domain,
capacity_bytes,capacity_fraction,working_set_bytes,seed,warmup_requests,requests,
hits,misses,hit_rate,byte_hit_rate,total_miss_cost_us,cost_hit_rate,
admissions,rejections,evictions,expirations,invalidations,size_changes,
mean_victims,max_victims,metadata_bytes_peak,metadata_bytes_per_resident,
ns_per_request,wall_seconds
```

`config_fingerprint` = XXH3-64 of the canonicalised effective config (sorted `key=value` lines).

`--timeseries` writes one row per run per interval: `policy,capacity_bytes,interval_index,first_request,requests,misses,miss_cost_us,hit_rate,cost_hit_rate`.

### 14.5 Domains (regional split)

* `--domain-by region` creates one independent engine per region value. Each gets the full `capacity_bytes`, and a results row is written per domain plus an `ALL` aggregate row.
* The comparison point is `--domain-by none` with `capacity × R`. This is the experiment for [D§7.3].

### 14.6 Export

`--export libcachesim` writes the trace in libCacheSim's `oracleGeneral` binary format (cost is not representable there; only key, size and time). It is used to run LeCaR/CACHEUS and to cross-check our LRU and S3-FIFO miss ratios against libCacheSim's (they must agree within 0.5 percentage points when TTL = 0 and cost is ignored).

---

## 15. Trace generator: clache_tracegen

### 15.1 Randomness (`include/clache/rng.h`)

* `SplitMix64` seeds `Xoshiro256ss`. Every workload derives sub-streams as `Xoshiro256ss(splitmix(seed ^ stream_id))`, with a fixed `stream_id` per purpose (arrivals, popularity, sizes, costs, jitter, scans…). Adding a feature therefore does not shift the other streams.
* `uniform01()`: `(next() >> 11) * 0x1.0p-53`.
* `normal()`: Box–Muller (cache the second value), in-repo.
* `lognormal(median, sigma)`: `median * exp(sigma * normal())`.
* `exponential(rate)`: `-log1p(-uniform01()) / rate`.
* `ZipfTable(n, s)`: precomputed CDF (`double[n]`) with binary search, n ≤ 10⁷. Rejection-inversion MAY replace it later.
* **Key IDs:** `key_id = splitmix64_mix(rank ^ (seed * 0x9E3779B97F4A7C15))`. This is a bijection, so key ids are unique and do not reveal popularity rank.
* **Per-key attributes** (size, base cost) are derived deterministically from `key_id` through a per-key `Xoshiro256ss(splitmix(key_id ^ attr_stream))`. The same key always has the same size and base cost unless the workload explicitly changes them.

### 15.2 Common parameters

| Param | Default | Meaning |
|---|---|---|
| `requests` | 10 000 000 | rows emitted |
| `keys` | 1 000 000 | catalogue size (unless the workload overrides) |
| `seed` | 1 | |
| `rate_rps` | 10 000 | Poisson arrivals; inter-arrival times ~ Exp(rate); timestamps in integer ns, starting at 0 |
| `size.dist` | `lognormal` | `fixed`, `lognormal`, `loguniform` |
| `size.median` / `size.sigma` | 4096 / 1.0 | lognormal; clamped to `[size.min, size.max]` = `[64, 4 MiB]` |
| `cost.dist` | `lognormal` | per-key base cost |
| `cost.median_us` / `cost.sigma` | 2000 / 1.0 | clamped to `[10, 30 000 000]` µs |
| `cost.jitter_sigma` | 0.25 | per-request cost = base × lognormal(1, jitter_sigma) |
| `ttl.fraction` / `ttl.choices_s` | 0 / `60,300,3600` | fraction of keys with a TTL; TTL picked uniformly per key |

### 15.3 Workloads [D§10.3]

| Name | Generation rule (on top of common) | Key params (defaults) |
|---|---|---|
| `zipf` | rank ~ Zipf(`keys`, `alpha`); size and cost independent of rank | `alpha = 0.9` |
| `cost_inversion` | Zipf popularity. Ranks ≤ `hot_frac·keys` get cheap cost (lognormal median `cheap_us`). Others: with probability `expensive_p`, expensive (median `expensive_us`); else medium (median `medium_us`). | `hot_frac = 0.05, cheap_us = 500, medium_us = 2000, expensive_us = 200000, expensive_p = 0.2` |
| `variable_size` | Zipf popularity; size log-uniform in `[1 KiB, 8 MiB]`; cost = `base_us + per_kib_us · size/1024` × jitter | `base_us = 1000, per_kib_us = 5` |
| `scan` | Zipf background. Every `scan_every` requests, emit `scan_len` consecutive requests to fresh, never-repeated keys whose cost has median `scan_cost_us`. This is the adversarial case: expensive one-hit wonders. | `scan_every = 100000, scan_len = 20000, scan_cost_us = 100000` |
| `hotset_shift` | Zipf over a rank→key permutation. Every `shift_every_s` of trace time, the top `hot_keys` ranks are remapped to a fresh random set of keys. | `shift_every_s = 600, hot_keys = 10000` |
| `burst` | Zipf background. Bursts start as a Poisson process (`bursts_per_hour`). Each burst picks `burst_keys` new keys that receive `burst_share` of traffic, uniformly, for `burst_duration_s`. | `bursts_per_hour = 6, burst_keys = 1000, burst_share = 0.3, burst_duration_s = 120` |
| `regional_split` | `regions` regions; each request's region is uniform. Each region has its own popularity permutation of the shared catalogue (`overlap` fraction of the top ranks shared) and its own cost multiplier. | `regions = 3, overlap = 0.5, cost_multipliers = 1,3,10` |
| `mixed` | `cost_inversion` costs + `variable_size` sizes + `scan` + `hotset_shift` together | union of the above |

`clache_tracegen --workload NAME --param k=v ... --out PATH [--format csv|bin]` writes the trace and its `.meta.json`. `clache_tracegen --list` prints workloads and their defaults.

---

## 16. In-process cache (Phase 3)

### 16.1 Public API (`include/clache/cache.h`)

```cpp
namespace clache {

using Bytes    = std::vector<std::byte>;
using ValueRef = std::shared_ptr<const Bytes>;

struct LoadResult {
  Bytes value;
  std::optional<std::chrono::milliseconds> ttl;
  std::optional<double> cost_hint_ms;
};

struct CacheConfig {
  EngineConfig engine;
  std::string  policy = "clache";
  PolicyConfig policy_config;
  std::uint32_t shards = 1;                    // >1 = independent policy domains
  std::uint64_t per_entry_overhead_bytes = 64;
};

class Clock {                                   // injectable for tests
 public:
  virtual ~Clock() = default;
  virtual TimePoint now() const = 0;
};
std::shared_ptr<Clock> steady_clock_source();

class Cache {
 public:
  explicit Cache(CacheConfig,
                 std::unique_ptr<StorageBackend> storage = nullptr,  // nullptr → InMemoryStore
                 std::shared_ptr<Clock> clock = steady_clock_source(),
                 MetricsSink* metrics = nullptr,
                 DecisionSink* decisions = nullptr);

  // Returns the value (cached or freshly loaded). Loader exceptions propagate.
  ValueRef get_or_load(std::string_view key, const std::function<LoadResult()>& loader);
  void invalidate(std::string_view key);
  CacheStats stats() const;
  std::optional<KeyExplanation> explain(std::string_view key) const;
};

}  // namespace clache
```

### 16.2 StorageBackend

```cpp
class StorageBackend {
 public:
  virtual ~StorageBackend() = default;
  // Returns nullptr if absent or if stored full key != key (collision).
  virtual ValueRef get(KeyId, std::string_view key) = 0;
  virtual void put(KeyId, std::string_view key, ValueRef value) = 0;
  virtual void erase(KeyId) = 0;
  virtual bool holds_other_key(KeyId, std::string_view key) const = 0;
  virtual std::uint64_t charged_size(std::string_view key, const Bytes&) const = 0;
};
```

`InMemoryStore` is an `unordered_map<KeyId, {std::string key; ValueRef value;}>`. Residency and expiry are owned by the engine; storage holds bytes only [D§8.3]. Storage does not track TTL itself, because the engine purges expired keys and returns them for erasure.

### 16.3 Request path (one shard)

```
get_or_load(key, loader):
  id = XXH3_64(key); now = clock.now()
  lock(mu)
    res = engine.lookup(id, now)
    erase res.expired / res.replaced from storage
    if res.hit:
        v = storage.get(id, key)
        if v: unlock; return v                                        ← hit: no allocation
        else: hash collision → stats.collisions++; treat as miss, mark no-admit
    if inflight[id] exists: fut = inflight[id].future; unlock; return fut.get()   (coalesced)
    inflight[id] = {promise, generation = res.generation}
  unlock
  t0 = clock.now(); result = loader()   [exception → lock; fulfil waiters with exception;
                                         erase inflight; stats.loader_errors++; rethrow]
  t1 = clock.now()
  cost_ms = result.cost_hint_ms.value_or(ms(t1 − t0))
  value = make_shared<const Bytes>(move(result.value)); size = storage.charged_size(key, *value)
  lock(mu)
    adm = (no-admit) ? reject(RejectHashCollision)
                     : engine.complete_miss(id, size, cost_ms, result.ttl, inflight[id].generation, t1)
    erase adm.evicted from storage
    if adm.admitted: storage.put(id, key, value)
    fulfil inflight[id] with value; erase inflight[id]
  unlock
  return value
```

* **Coalescing:** concurrent misses on the same key wait on the leader's `shared_future`. Their accesses are recorded by `lookup` (frequency counts them), but no extra cost is observed. Counter: `coalesced_total`.
* **Invalidation during load:** `invalidate` takes the lock, calls `engine.invalidate` (bumping the generation), and erases from storage. The leader's `complete_miss` then sees a generation mismatch and does not admit. Waiters still receive the loaded value; it was valid when it was loaded, the same guarantee as a load that finished just before the invalidation.
* **TTL check on hit:** expiry is purged in `lookup` under the same lock using the same `now`, so an expired value is never returned [D§16.1].
* **Locking:** one `std::mutex` per shard (v0 model, [D§9.2]). With `shards > 1`, `shard = id % shards`, each shard has its own engine with `capacity / shards`, and policy domains are independent. Lock wait time is measured: `try_lock` first; on failure, time the blocking `lock()` into the `lock_wait_ns` histogram.

### 16.4 CacheStats

`CacheStats` = the engine stats summed over shards, plus `coalesced_total`, `loader_errors_total`, `collisions_total`, `bytes_resident`, `metadata_bytes`, `lock_wait_ns` (p50/p99), `lookup_ns` and `loader_ms` histograms.

---

## 17. Observability

### 17.1 MetricsSink

```cpp
class MetricsSink {
 public:
  virtual ~MetricsSink() = default;
  virtual void counter_add(std::string_view name, std::uint64_t delta) = 0;
  virtual void histogram_record(std::string_view name, double value) = 0;
};
```

* The engine never calls the sink on the hot path. `Cache` flushes engine counter deltas every 4096 operations or on `stats()`.
* Histograms (`metrics.h`) are log-linear: 64 power-of-two major buckets × 8 linear sub-buckets, with `uint64_t` counts. Quantiles come from bucket midpoints.
* Required names follow [D§11.1–11.2]: `clache_requests_total`, `clache_hits_total`, …, `clache_policy_decision_ns`, `clache_victim_set_size`, `clache_resident_age_at_eviction_s`, `clache_cache_value`.
* A Prometheus adapter is deferred (Phase 5).

### 17.2 Decision records [D§11.3]

```cpp
struct DecisionRecord {
  std::uint64_t seq;           TimePoint now;      KeyId key;
  Decision decision;           const char* policy;
  double candidate_value;      double threshold_value;
  double frequency_rps;        double miss_cost_ms;     std::uint64_t size_bytes;
  std::uint64_t bytes_needed;  std::uint32_t victim_count;
  std::array<KeyId, 8> victims_head;   // first ≤ 8 victims
};
class DecisionSink { public: virtual ~DecisionSink() = default; virtual void record(const DecisionRecord&) = 0; };
```

* **Sampling is deterministic:** record if `XXH3_64(seq ^ seed) < rate · 2⁶⁴`. Every non-`Hit` decision is eligible; hits are eligible only if `telemetry.sample_hits = true`.
* `decision_log.cpp` writes NDJSON with one object per line. For example:

```json
{"seq":81234,"t_ns":40123000000,"key":"report:82","decision":"REJECT","reason":"CANDIDATE_VALUE_NOT_HIGHER_THAN_REQUIRED_VICTIMS","value":0.00184,"frequency_rps":0.0167,"miss_cost_ms":1200,"size_bytes":10854400,"bytes_needed":5242880,"threshold_value":0.00310,"victims":[]}
```

`Cache` resolves `KeyId` back to the string key when it is still in storage; otherwise it prints the hex id.

---

## 18. Configuration

### 18.1 File format

A restricted YAML subset:

* nested maps with 2-space indentation
* scalar values: int, float, bool, string, size with suffix (`64MiB`)
* `#` comments

Lists, anchors and multi-line strings are not supported; they cause a parse error with a line number. `--set a.b.c=value` overrides any key. Unknown keys are **errors**.

### 18.2 Full key reference

```yaml
clache:
  capacity_bytes: 1GiB                 # required in Cache; sim uses --capacity
  max_object_bytes: 64MiB              # 0 → capacity
  seed: 0
  frequency:
    mode: rolling_buckets              # rolling_buckets | forward_decay
    window_seconds: 300
    bucket_seconds: 60                 # window % bucket == 0; window/bucket in [1,8]
    decay_tau_seconds: 300             # forward_decay only
  miss_cost:
    ewma_alpha: 0.20                   # (0,1]
    min_ms: 0.01                       # > 0
    max_ms: 30000                      # >= min_ms
    default_ms: 0.01                   # estimate before any sample
    min_samples: 1
  value:
    size_exponent: 1.0                 # [0, 2]
  admission:
    min_observations: 1                # >= 1; 1 = admit on first miss
    max_victims: 0                     # 0 = unlimited (Clache only)
  observation_table:
    max_entries: 1000000               # 0 disables ghosts
    ttl_seconds: 1200
  telemetry:
    decision_sample_rate: 0.001        # [0,1]
    sample_hits: false
policy:                                # per-policy settings; ignored by others
  clache: { use_cost: true, admission_rule: strict }       # strict | always
  gdsf:   { frequency_source: resident_count, size_exponent: 1.0 }
  tinylfu: { window_fraction: 0.01, protected_fraction: 0.80 }
  s3fifo: { small_fraction: 0.10, move_to_main_threshold: 1 }
  redis:  { samples: 5, pool_size: 16, lfu_log_factor: 10, lfu_decay_time_min: 1 }
```

The one-line `{ … }` flow maps shown under `policy:` are the only flow syntax the parser accepts. All defaults are experimental placeholders [D§13]. Phase 2 sets the documented defaults.

### 18.3 Validation

`validate(const EngineConfig&)` returns all errors at once (`std::vector<std::string>`). Both binaries and the `Cache` constructor refuse invalid config. The ranges are those annotated above.

---

## 19. Testing

### 19.1 Unit tests (tests/unit)

| Component | Must cover |
|---|---|
| `IndexedMinHeap` | push/update/erase at random positions vs. a `std::multiset` model; `for_each_ascending` order equals the sorted order; `heapify`; `-inf` keys; tie order |
| `FrequencyEstimator` buckets | first record = 1; counts across epoch boundaries; gap ≥ B clears; partial-window semantics; saturation; `rate` is const and pure |
| `FrequencyEstimator` decay | `rate` against brute-force Σ exp; `logaddexp(-inf, x)`; ranking by key equals ranking by rate at random t |
| `MissCostEstimator` | first sample initialises; EWMA math; clamping; `min_samples` prior |
| `MetadataStore` | observation LRU order; count bound drops tail only; TTL reset; slot recycling is LIFO and deterministic; ghosts keep telemetry; invalidate resets cost |
| Config parser | every key; unknown key error; `--set` precedence; size suffixes; range errors |
| Trace reader | CSV/bin round trip; every validation error has the right line number |
| Each policy | plan contract (§9.1) on a hand-built small state; LRU/LFU/S3-FIFO/TinyLFU victim order on textbook examples |

### 19.2 Golden scenarios (tests/golden)

These scenarios are computed by hand and run through `CacheEngine` with policy `clache`, bucket mode (W = 300 s, b = 60 s), α = 1, EWMA α = 0.2, `per_entry_overhead = 0`, and all requests inside epoch 0 unless stated. Values are shown as **V × 300 = count · C / S** (the common `1/300` factor is omitted). Each test asserts the exact decision, the victims and their order, the residents, and the counters.

**G1 — cost beats recency.** Capacity 100 B.

| t (s) | Request (key, bytes, cost ms) | Event | Outcome |
|---|---|---|---|
| 1 | A, 50, 1 | miss | AdmitFreeSpace |
| 2 | B, 50, 100 | miss | AdmitFreeSpace |
| 3 | C, 50, 10 | miss; needed 50; V: A = 0.02, B = 2, C = 0.2 | AdmitWithEviction, victims [A] |
| 4 | A, 50, 1 | miss; A count 2 → 0.04; lowest resident C = 0.2 ≥ 0.04 | RejectCandidateNotHigher |
| 5 | B | hit | Hit |

Total miss cost = 1 + 100 + 10 + 1 = **112 ms**. The same trace under `lru` evicts B at t = 4 and misses B at t = 5, for **212 ms**. The test asserts both totals.

**G2 — atomic multi-victim admit.** Capacity 100 B. X(30 B, 1 ms) at t = 1, Y(30 B, 100 ms) at t = 2, Z(40 B, 1 ms) at t = 3 are all admitted (V: X = 0.0333, Y = 3.333, Z = 0.025).

* t = 4: W(60 B, 5 ms), V = 0.0833, needed 60.
* Ascending walk: Z (0.025, 40 B, total 40), then X (0.0333, 30 B, total 70 ≥ 60).
* Both are lower → AdmitWithEviction, victims **[Z, X]**. Residents {Y, W}, 90 B.

**G2b — atomic multi-victim reject.** Same as G2, but W costs 1.8 ms (V = 0.03).

* Walk: Z (0.025 < 0.03, total 40 < 60), then X (0.0333 ≥ 0.03), which blocks.
* RejectCandidateNotHigher, victims [], **Z is still resident**, `bytes_resident` = 100.

**G3 — exact tie rejects.** Capacity 60 B. P(30 B, 1 ms) at t = 1 and Q(30 B, 1 ms) at t = 2 are admitted. At t = 3, R(30 B, 1 ms) has a `K` bit-identical to P and Q → RejectCandidateNotHigher.

**G4 — oversize.** Capacity 100, `max_object_bytes` 80. O(90 B) at t = 1 and t = 2 → RejectOversized both times; 2 misses; no residents.

**G5 — TTL.** Capacity 100.

* t = 1: T(50 B, 10 ms, ttl 5 s) is admitted with expiry 6 s.
* t = 3: hit.
* t = 6: purged (expiry ≤ now), `expirations` = 1, then a miss → AdmitFreeSpace with expiry 11 s.
* The test also asserts `lookup().expired == [T]` at t = 6.

**G6 — observation history earns admission.** Capacity 50.

* t = 1: A(50 B, 10 ms) is admitted.
* t = 2: B(50 B, 10 ms) is a bit-identical tie with A → reject.
* t = 3: B again; count 2 → 0.4 > A 0.2 → AdmitWithEviction, victims [A].
* Running the same trace with `clache_noghost` gives reject at t = 3 as well (B's count resets to 1). The test asserts that difference.

**G7 — window aging and heap rebuild.** Capacity 50.

* t = 0: A(50 B, 10 ms) is admitted.
* t = 1, 2: A hits; count 3 → 0.6.
* t = 400 (epoch 6; window epochs 2–6): B(50 B, 10 ms). The rebuild sets A's V to 0 (K = −inf); V_B = 0.2 → AdmitWithEviction, victims [A]. `heap_rebuilds` ≥ 1.

**G8 — invalidation during load** (engine-level). Call `lookup(K)` → miss with generation g; then `invalidate(K)`; then `complete_miss(K, …, g)` → RejectInvalidatedDuringLoad.

**G9 — size change.** A(50 B) is admitted; a later request for A with size 60 → `replaced = [A]`, a miss, `size_changes = 1`, and normal admission of the 60-byte version.

### 19.3 Property and differential tests (tests/property)

* **Reference model:** `ReferenceClachePolicy` (test-only) stores the full access timestamp list per key and recomputes every resident's exact `V` at plan time by brute force. It sorts by `(V, seq, key)` and walks.
* **Generator:** random op sequences of 10⁴ ops over 50 keys, using a seeded RNG. They include random sizes `[1, 40]` B with capacity 200 B, costs, TTLs, `del` ops, and time steps that often cross bucket boundaries and sometimes jump past a whole window.
* **Assertion:** heap-based `clache` and the reference produce **identical** decision sequences (decision and victim list) in bucket mode.
* **Decay mode:** decisions are identical unless the reference's top-two values differ by less than 1e−9 relative. Such near-ties are skipped and counted, and must be < 0.1% of decisions.
* 1000 seeds run in CI Release. `CLACHE_PROPERTY_SEEDS` overrides this locally.
* The same harness runs every baseline policy against generic invariants (below), plus `lru` against a trivial list model.

### 19.4 Invariants (`check_invariants`, run after every op when `CLACHE_CHECK_INVARIANTS`)

1. `bytes_resident == Σ charged_bytes` over residents, and `≤ capacity_bytes`.
2. Resident count == the policy's tracked count (heap size, list length, …).
3. Heap property holds. In decay mode, or bucket mode within an epoch, every heap key equals a fresh `score(slot, now)`.
4. Every resident with a TTL is in the expiry heap with `expiry > now` (after purge).
5. `observed_count ≤ max_entries`. No slot is both resident and in the observation list. The index and the slots agree.
6. `policy.check_invariants()`.

### 19.5 Integration tests (tests/integration)

* **Determinism:** generate a `mixed` trace (2·10⁵ requests, seed 7), run `clache_sim` with all policies twice, with `--jobs 1` and `--jobs 8`. `results.csv` (minus the wall-time columns) and the `decisions.ndjson` files must be byte-identical.
* **Sanity ordering:** on `cost_inversion`, `clache` total miss cost < `lru` total miss cost at 1%, 5% and 10% capacity (a smoke check of the thesis, not the research claim).
* **libCacheSim cross-check** (optional job, Phase 1b): see §14.6.
* **Phase 3:** `Cache` with a fake clock and a fake loader. Covers hits, admits, rejects, TTL, invalidation during load, loader exceptions, coalescing, and a hash collision forced through an injected hash function. Includes a TSan stress test: 16 threads × 10⁵ ops on 1000 keys with random invalidations, checking that no returned value is stale relative to the last completed invalidation.

---

## 20. Benchmarks and experiment protocol

### 20.1 Micro benchmarks (benchmarks/micro, Google Benchmark)

| Benchmark | Parameters |
|---|---|
| `BM_HeapUpdate` | n ∈ {10⁴, 10⁵, 10⁶, 10⁷} |
| `BM_HeapForEachAscending` | k ∈ {1, 8, 64} victims |
| `BM_FrequencyRecord` / `BM_FrequencyRate` | buckets vs decay |
| `BM_EngineHit/<policy>` | residents ∈ {10⁵, 10⁶}; steady state; reports ns/op and allocations/op (custom counter via an allocation-counting hook in the bench binary) |
| `BM_EngineMissAdmit/<policy>` | same |
| `BM_BucketRebuild` | residents ∈ {10⁵, 10⁶, 10⁷} |
| `BM_CacheGetHit` (Phase 3) | threads ∈ {1, 2, 4, 8, 16, 32} × shards ∈ {1, 16, 64} |

Phase 1 targets (Release, a modern x86-64 or Apple M-series core, 10⁶ residents):

* `clache` hit ≤ 250 ns/op and 0 allocations/op.
* Miss + admit with ≤ 4 victims ≤ 1 µs/op.
* Bucket rebuild at 10⁶ residents ≤ 50 ms.

If a target is missed, the shortfall is recorded with a profile. It does not block Phase 1 exit; it does block Phase 3 exit.

### 20.2 Experiment protocol [D§10.6]

* **Matrix:** 8 workloads (§15.3) × all registered policies × capacity fractions {0.1%, 0.5%, 1%, 2%, 5%, 10%, 20%} × 5 seeds (trace seeds 1–5).
* **Scale:** 10⁷ requests per trace, 10⁶ keys (`variable_size` uses 2·10⁵ keys).
* **Warm-up:** the first 20% of requests. The sensitivity to {10%, 30%} is reported once.
* **Pinning:** the compiler version, flags, git SHA, and trace and config fingerprints are pinned and written to every CSV row. `benchmarks/scripts/run_matrix.sh` captures the CPU model and OS in `env.json`.
* **Reporting** (`report.py`):
  * Per workload × capacity: median and min/max across seeds of total miss cost **normalised to the best of {lru, lfu, lfu_da}**, cost hit rate, hit rate, and byte hit rate.
  * Miss-cost-vs-capacity curves per workload.
  * Metadata bytes per resident key.
  * ns/request.
* **Go/no-go** [D§16.3]: Phase 2 passes if `clache` (or a tuned variant) gives ≥ 10% lower total miss cost than the best of LRU/LFU/LFU-DA at ≥ 3 capacity points on ≥ 3 cost-heterogeneous workloads (`cost_inversion`, `variable_size`, `mixed`, `regional_split`, `scan`). The report must also state, without editorialising, how it compares with `gdsf`, `gdsf_window`, `wtinylfu` and `s3fifo`, including where it loses.

---

## 21. Redis comparison (Phase 4) outline

* `RedisAdapter : StorageBackend` uses hiredis with a connection pool.
  * Redis runs with `maxmemory` set **above** the experiment budget and `maxmemory-policy noeviction`, so only Clache evicts.
  * Admits become `SET key value PX ttl`. Victims and expired keys become a pipelined `DEL`. A hit is `GET`.
* **Baselines:** Redis with `maxmemory = budget` and `allkeys-lru` / `allkeys-lfu`, with the same trace replayed by `clache_bench --redis`.
  * The budget is matched as Redis `used_memory` at steady state. Clache's capacity is calibrated so its Redis `used_memory` matches; the calibration is reported.
* The same client concurrency, pipelining depth, persistence (off), hardware and trace are used in all arms. Policy time and transport time are reported separately [D§10.5, §10.6].
* `redis_lru` / `redis_lfu` simulator approximations are validated here (§11.4).

---

## 22. Phase plan and task breakdown

Each task ends with merged code, tests passing in CI, and a short note in `docs/`.

### Phase 1 — Simulator (MUST)

| # | Task | Deliverable / done when |
|---|---|---|
| 1.1 | Repo skeleton | CMake targets/options (§4.2), CI matrix (§4.4), clang-format/clang-tidy configs, empty test passes |
| 1.2 | `types.h`, `rng.h`, `IndexedMinHeap` | unit tests §19.1 for heap and RNG (golden first-10 outputs of xoshiro for seed 1) |
| 1.3 | Estimators | bucket and decay frequency, EWMA cost, unit tests |
| 1.4 | `MetadataStore` + observation table | unit tests |
| 1.5 | Config structs, parser, validation | unit tests |
| 1.6 | `CacheEngine` + `lru` policy | engine algorithm §12; invariant checker; LRU unit tests; G4/G5/G8/G9 pass with `lru` where applicable |
| 1.7 | Trace reader/writer (CSV + bin) | round-trip tests |
| 1.8 | `clache_sim` | CLI §14.1, pre-pass, runs, results/timeseries CSV, `--jobs` |
| 1.9 | `CostValuePolicy` + ablations | G1–G9 pass; differential test §19.3 passes on 1000 seeds |
| 1.10 | `lfu`, `lfu_da`, `gds`, `gdsf`, `gdsf_window` | unit tests + generic property invariants |
| 1.11 | `clache_tracegen` | all 8 workloads, `.meta.json`, deterministic output test |
| 1.12 | Decision log | NDJSON sink, deterministic sampling |
| 1.13 | `report.py`, `run_matrix.sh` | produces the tables/plots of §20.2 |
| 1.14 | Determinism integration test | §19.5 passes in CI |

**Exit:** reproducible result tables for LRU/LFU/LFU-DA/GDS/GDSF/Clache across all synthetic workloads [D§14 Phase 1].

### Phase 1b — Remaining baselines (MUST before any claim beyond LRU/LFU)

| # | Task |
|---|---|
| 1b.1 | `wtinylfu` |
| 1b.2 | `s3fifo` |
| 1b.3 | `redis_lru`, `redis_lfu` |
| 1b.4 | `--export libcachesim` + cross-check job (§14.6) |

### Phase 2 — Policy tuning (MUST)

Run the experiment matrix of §24 and pick defaults. Write `docs/results-v0.md` containing the go/no-go verdict (§20.2). **Exit:** a repeatable target improvement, or a documented revision of the design [D§14 Phase 2].

### Phase 3 — In-process cache (MUST)

| # | Task |
|---|---|
| 3.1 | `StorageBackend`, `InMemoryStore` |
| 3.2 | `Cache` single shard: request path §16.3, coalescing, generation check, loader errors |
| 3.3 | Sharding, lock-wait measurement |
| 3.4 | `MetricsSink`, histograms, `CacheStats`, `explain()` |
| 3.5 | Phase 3 tests incl. TSan stress (§19.5) |
| 3.6 | `BM_CacheGetHit` + `clache_bench` load driver; report throughput vs threads for `clache`, `lru`, `s3fifo` |

**Exit:** correctness tests pass under ASan/UBSan/TSan; micro benchmark targets (§20.1) are met or have a written waiver [D§14 Phase 3].

### Phases 4–8 (outline only; detailed specs written when reached)

| Phase | Scope | Pointer |
|---|---|---|
| 4 | Redis comparison | §21 |
| 5 | Real app: instrumented web service; Prometheus adapter | [D§14] |
| 6 | Distributed: shard/region-local domains (builds on `--domain-by`, `shards`) | [D§7.3] |
| 7 | Hierarchical L1/L2: L1 cost = measured L2 latency | [D§7.4] |
| 8 | ML predictors for F / P(reuse) / C, only after Phase 2 baselines are tuned | [D§4.7] |

---

## 23. Coding standards

* **Style:** `.clang-format` based on Google style, 100 columns. Types are `PascalCase`; functions and variables `snake_case`; members have a trailing underscore; constants are `kName`. Everything is in `namespace clache`.
* **Errors:** exceptions only at boundaries (config, trace IO, the public `Cache` API for loader errors). The engine hot path does not throw except for contract violations (`std::logic_error`/assertions).
* `CLACHE_ASSERT(cond, msg)` is active in Debug and with `CLACHE_CHECK_INVARIANTS`.
* **Determinism rules:** no iteration over `unordered_*` containers where the order affects decisions or output; sort first. No wall-clock reads in the engine. No `std::*_distribution`. No `-ffast-math`.
* **Hot-path rules** [D§9.1]: no allocation on hits; scratch vectors are reused; `std::function` is not used inside the engine (policies are virtual, one call per hook).
* **Headers** in `include/clache` are the public API. Anything else stays in `src/` with internal headers next to its `.cpp`.
* **Every policy** gets a doc comment block that cites its source paper or documentation [D App. C].

---

## 24. Experiment matrix for open research questions

These are Phase 2 runs, mapped to [D§15]. Each produces a section in `docs/results-v0.md`.

| Question | Experiment | Policies / knobs |
|---|---|---|
| Q2: does Clache beat GDSF with equivalent telemetry? | full matrix | `clache` vs `gdsf` vs `gdsf_window` vs `clache_noadmit` (isolates admission vs scoring) |
| Q3: frequency estimator | full matrix on `hotset_shift`, `burst`, `mixed`; window ∈ {60, 300, 900} s | `clache` vs `clache_decay`; τ ∈ {60, 300, 900} |
| Q4: large objects / many victims | `variable_size`, `mixed` | `size_exponent` ∈ {0.5, 0.75, 1.0, 1.25}; `max_victims` ∈ {0, 4, 16, 64} |
| Q5: probation / one-hit wonders | `scan`, `burst` | `min_observations` ∈ {1, 2, 3}; compare with `wtinylfu`, `s3fifo` |
| Q6: metadata budget | all workloads | `observation_table.max_entries` ∈ {0, 0.5×, 1×, 4×} residents at capacity; report metadata bytes/key and Δ miss cost; also charge metadata against capacity (flag `value.charge_metadata`, added in Phase 2 if needed) |
| Q8: cost refresh for hot keys | `cost_inversion` with a cost drift variant (tracegen param `cost.drift_every_s`) | measured-only vs `--cost-oracle` (upper bound on refresh benefit) |
| Q1/Q9: cost definitions | deferred to Phase 5 (needs a real app) | — |
| Q7: sampled victim selection | Phase 3 throughput study | add `clache_sampled` (sample k residents, take the min) only if the global heap limits throughput |

---

## 25. Glossary

| Term | Meaning |
|---|---|
| Cache domain | one independent engine instance (a node, shard or region) |
| Charged bytes | bytes counted against capacity for one object |
| Epoch | a bucket-sized slice of engine time, `floor((t − T0)/b)` |
| Forward decay | exponential decay expressed relative to a fixed origin, so stored keys never need rescaling |
| Ghost / observation entry | metadata for a nonresident key; never holds a value |
| `K` | heap key; log-domain Cache Value (§10.1) |
| Slot | dense metadata index; recycled after a key leaves the observation table |
| Total miss cost | Σ trace miss cost over post-warm-up misses; the primary metric |
| Working-set bytes | Σ over unique keys of the maximum size seen in the trace |
