# Development guide

## Build

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DCLACHE_WERROR=ON
cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure
```

| Option | Default | Effect |
|---|---|---|
| `CLACHE_BUILD_TESTS` | ON | GoogleTest suite (`clache_tests`) and the header self-containment check |
| `CLACHE_BUILD_BENCHMARKS` | ON | Google Benchmark suite (`clache_micro`) |
| `CLACHE_SANITIZE` | "" | `address`, `undefined`, `address,undefined` or `thread` |
| `CLACHE_CHECK_INVARIANTS` | OFF | full invariant checks; always on in Debug |
| `CLACHE_WERROR` | OFF | warnings as errors (CI turns this on) |
| `CLACHE_WITH_REDIS` | OFF | Phase 4 Redis adapter |

Dependencies (GoogleTest, Google Benchmark, xxHash) are fetched by CMake at configure time, pinned by SHA-256.

Run benchmarks from a Release build:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
./build/release/benchmarks/clache_micro
```

**macOS note:** AddressSanitizer from Apple Clang 17 hangs during runtime initialisation on macOS 26, even for an empty program. This is a toolchain bug, not a Clache bug. Use `CLACHE_SANITIZE=undefined` locally (or Homebrew LLVM for ASan); CI runs ASan+UBSan and TSan on Linux.

## Formatting

`.clang-format` (Google style, 100 columns) is enforced in CI with clang-format 18:

```sh
clang-format -i $(git ls-files '*.h' '*.cpp')
```

## Where code goes (no build-file edits needed)

Every directory is globbed with `CONFIGURE_DEPENDS`, so adding a file never requires touching CMake:

| Put a file in | It becomes part of |
|---|---|
| `src/{core,estimators,index,policy,storage,telemetry}/*.cpp` | `clache_core` (linked whole via `clache::core`) |
| `src/sim/*.cpp` | `clache_sim_lib` (`clache::sim`), created once a file exists |
| `apps/<name>.cpp` | executable `<name>` (e.g. `apps/clache_sim.cpp` → `clache_sim`) |
| `tests/{unit,golden,property,integration}/*.cpp` | `clache_tests` |
| `benchmarks/micro/*.cpp` | `clache_micro` |
| `include/clache/*.h` | public API; automatically checked to compile on its own |

Policies register themselves with `CLACHE_REGISTER_POLICY("name", factory)` in their own `.cpp`. No central list to edit.

## Parallel branches

The public headers in `include/clache/` are the contract between branches. Wave 0 (`feat/foundation`) fixed them; wave-1+ branches implement them.

Rules:

1. Implement what a header declares; don't change public signatures in a feature branch. If an interface must change, make the change in a small separate PR first, and have other branches rebase on it.
2. Private members of a class (e.g. `MetadataStore`'s fields, `CacheEngine::Impl`) belong to the branch that implements it and may change freely.
3. Each branch owns its directory / files:

| Branch | Owns |
|---|---|
| `feat/estimators` | `src/estimators/`, tests for estimators |
| `feat/metadata-store` | `src/core/metadata_store.cpp`, `src/core/observation_table.cpp` |
| `feat/config-parser` | `include/clache/config_parser.h` (new), `src/core/config_*.cpp`, `validate()` |
| `feat/trace-io` | `src/telemetry/trace_*.cpp`, new `include/clache/trace.h` |
| `feat/tracegen` | `apps/clache_tracegen.cpp`, workload sources |
| `feat/policies-classic` | `src/policy/{lru,lfu,gdsf}_policy.cpp` |
| `feat/policies-modern` | `src/policy/{tinylfu,s3fifo,redis_sampled}_policy.cpp` |
| `feat/report-scripts` | `benchmarks/scripts/` |
| `feat/engine` | `src/core/cache_engine.cpp`, decision log |
| `feat/clache-policy` | `src/policy/cost_value_policy.cpp` |
| `feat/simulator` | `src/sim/`, `apps/clache_sim.cpp` |

Until the engine lands, policy branches test their policies directly against a hand-built `MetadataStore`/`PolicyContext` (or a small test fixture of their own under `tests/unit/`).

## Interface decisions made in wave 0 (beyond Spec v0.2)

These refine the implementation spec; the headers are authoritative.

| Where | Decision | Why |
|---|---|---|
| `CacheEngine::lookup` / `complete_miss` / `abort_miss` | A missing `lookup()` **pins** the key's metadata slot until `complete_miss()` or the new `abort_miss()`. | In `Cache`, the loader runs without the lock. Without pinning, other requests could trim the observation entry and `complete_miss()` would find no slot. |
| `MetadataStore::trim()` | `lookup_or_observe()` never drops the slot it returns; the engine calls `trim()` at the end of each public op to enforce the strict bound. | With `max_entries = 0`, the just-created candidate slot would otherwise be dropped mid-request. |
| `FrequencyEstimator::set_origin()` | Estimators are constructed with the engine, and the engine sets `T0` on its first operation. | Policies hold pointers to the estimators from construction. |
| `FrequencyEstimator::time_offset()` | Returns `u(now)` (0 in bucket mode) so callers can turn a stored heap key back into `ln F`. | Explanations / `Policy::value()`. |
| `FrequencyState::recorded` | Explicit "has ever been recorded" flag. | Distinguishes a first record from epoch 0. |
| `AdmissionResult::expired` | `complete_miss()` reports keys it purged. | It purges expired keys first, which the caller must erase from storage. |
| `CacheEngine` | pImpl. | Lets `feat/engine` change internals without touching the shared header. |
| `check.h` | `CLACHE_REQUIRE` (always on, throws `std::logic_error`) and `CLACHE_ASSERT` (Debug / invariants, aborts). | Spec §23. |
| `hash.h` | `hash64`, `hash64_u64`, streaming `Hasher64` over XXH3. | Key ids, sketches, sampling, fingerprints. |
| `telemetry.h` | `should_sample(seq, seed, rate)` implements the deterministic sampling rule. | Spec §17.2. |
| `rng.h` | Adds `uniform_below` (Lemire, unbiased), `bernoulli`, and a Fisher–Yates `shuffle`. | `std::shuffle` / distributions are implementation-defined. |
| apps | Source file name = binary name (`apps/clache_tracegen.cpp`). | Lets `apps/CMakeLists.txt` glob. |
