# Simulator

`clache_sim` replays a shared in-memory trace through independent real `CacheEngine`
instances. Build with the trace I/O, configuration parser, engine and policy branches
integrated. This feature branch intentionally does not duplicate those implementations.

```sh
clache_sim --trace trace.ctrace --policies lru,lfu_da,gdsf,wtinylfu,s3fifo,clache \
  --capacity 1MiB,4MiB --jobs 4 --out results.csv
clache_sim --trace trace.csv --domain-by region --capacity 1MiB \
  --timeseries series.csv --timeseries-every 1000 \
  --decisions decisions.ndjson --decision-sample-rate 1
clache_sim --trace trace.csv --stats-only --out stats.json
clache_sim --trace trace.csv --export libcachesim --out trace.oracleGeneral
```

Use `--help` for all options. Configuration files are applied in command-line order,
then all `--set` overrides in command-line order, regardless of where each appears.
Explicit CLI capacities supersede the configured capacity. Without either, the
fraction sweep is 0.001, 0.01, 0.05, 0.1, 0.2. Fractions use uncharged working-set
bytes, round down, and must yield at least one byte. Absolute capacities may exceed
the working set. Fraction sweeps preserve the requested fraction in CSV, even when
byte rounding differs across trace seeds; explicit capacities report capacity / working set.
Default policies include LRU, LFU, LFU-DA, GDS, GDSF, GDSF-window,
W-TinyLFU, S3-FIFO, Redis LRU/LFU approximations and Clache. Ablation names can be
selected explicitly.

## Accounting conventions

- The warm-up cutoff is `floor(record_count * warmup_fraction)`, including delete
  records. `warmup_requests` counts only GETs before that cutoff. Regional runs use
  the same global cutoff. All records update engine state, including warm-up.
- GET counts, true costs and byte hit ratios exclude warm-up and exclude deletes.
  Byte ratios use the trace payload size; per-entry overhead affects only admission
  and capacity accounting. Engine counters are final minus the last warm-up snapshot.
- Working-set bytes sum each key's maximum GET size; delete-only keys are excluded.
  Mean object size is working-set bytes divided by distinct GET keys. One-hit fraction
  counts keys with exactly one GET. Regional prepass statistics use global key IDs,
  just as the unsplit trace does. All prepass statistics appear on stdout and in
  `<results.csv>.stats.json`, preserving the exact results CSV schema.
- Division by zero yields zero for reported ratios. Empty traces run with explicit
  capacity; fractional capacities cannot be derived from an empty working set.
- Metadata is sampled every 65,536 measured GETs and once at the end. The metadata
  ratio is the maximum sampled bytes/resident ratio, ignoring samples with no
  residents. An ALL row sums per-region metadata peaks (an upper bound on concurrent
  peak), and takes the maximum regional bytes/resident ratio. These choices make
  the memory summaries deterministic without synchronizing engines.
- Wall time covers replay including warm-up, deletes and simulator overhead.
  `ns_per_request` divides this by measured GET count. ALL wall time sums regional
  replay durations (CPU work approximation), rather than reporting pool elapsed time.
- Regional engines each receive the displayed full capacity. ALL rows retain that
  per-region capacity; total allocated cache capacity is capacity times region count.
  Ratios and mean victims aggregate numerators/denominators, never averages of ratios.
- Results order follows policy order, capacity order and lexical region name order,
  followed by ALL. Parallel workers publish into fixed slots; metrics, fingerprints,
  time-series and sampled decisions are identical for any `--jobs`. Timing fields
  naturally vary.
- Time-series intervals cover global post-warm-up record positions; `first_request`
  is a zero-based original trace record index. Empty intervals are omitted. The main
  file contains ALL; regional files are `<path>.region-N.csv`, all with the exact
  specified nine-column schema.
- Decision logs include warm-up and use engine-controlled deterministic sampling.
  Each JSON object adds policy, domain and capacity to the engine decision fields.
  Infinite/NaN policy values become JSON null; victim IDs are truncated to the engine's
  eight-entry head and `victim_count` retains the total. Logs buffer per run, then
  concatenate in results order, so large sampling rates need proportionally more RAM.

## Fingerprints and build provenance

The effective config applies engine and policy ablations before hashing. Sorted
canonical config lines are followed by sorted `sim.*` keys for cost oracle, domain
mode, entry overhead, policy name and warm-up fraction. Job count and output paths
are excluded. Trace fingerprints hash the original trace bytes.

Build provenance uses the string macros `CLACHE_BUILD_VERSION` (fallback `0.1.0`)
and `CLACHE_BUILD_GIT_SHA` (fallback `unknown`). The current shared CMake files do not
supply Git metadata. A release build can supply escaped string definitions through
`CMAKE_CXX_FLAGS`, e.g. `-DCLACHE_BUILD_GIT_SHA=\"<commit>\"`; automation should set
this from the actual integrated source revision when configuring. The simulator
never reads runtime working-directory Git state or invents a SHA for a source snapshot.
Adding automatic provenance extraction belongs in a separate shared CMake change.

## libCacheSim export

Export writes little-endian, headerless 24-byte records:
`uint32 seconds, uint64 key, uint32 size, int64 next_access_vtime`.
Time is truncated from nanoseconds to seconds; next access is the absolute one-based
record position, or -1 when absent. Size changes are retained. Cost and region are
not represented. Export requires GET-only, TTL-zero input and rejects timestamps
outside unsigned 32-bit seconds before touching the destination, rather than
silently losing deletes or TTL. Compare with unsplit engine runs using zero overhead.

The layout was checked against the upstream
[oracleGeneral reader](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/traceReader/customizedReader/oracle/oracleGeneralBin.h)
and [converter](https://github.com/1a1a11a/libCacheSim/blob/develop/libCacheSim/bin/traceUtils/traceConvOracleGeneral.cpp).
The automated CLI test independently decodes the output with Python `<IQIq` and
checks both repeated-key future positions and the -1 sentinel. Cross-tool miss-ratio
agreement still requires running a separately installed libCacheSim binary.

## Current engine limitations

These are properties of the supplied engine snapshot, not simulator substitutions:

1. `CacheEngine::invalidate()` does not purge expired objects. Reproduce with a GET
   at t=0 and TTL=1 second, followed only by DEL at t=2 seconds. The engine reports
   invalidation, not expiration, for that key; unrelated expired keys can remain
   resident until the next GET. The simulator forwards DEL directly, so those counters
   reflect the current engine. No artificial accesses are injected to hide this.
2. `CacheEngine` exposes no expected-entry-count constructor hint. The trace prepass
   computes mean object size, but cannot pass `capacity / mean_size` to W-TinyLFU.
   Its policy therefore uses its documented capacity/4096 fallback.
3. The engine's private validation requires miss-cost default_ms within min_ms/max_ms.
   Public config validation and the spec permit any nonnegative default. For example,
   `--set clache.miss_cost.default_ms=0` passes public validation but the engine rejects
   construction with `default_ms must be inside cost bounds`. The error propagates.

## Verification

C++ unit tests cover trace prepass, warm-up, true-cost/byte metrics, TTL, size changes,
overhead, multi-victim summaries, regions, parallel decisions, ablations, empty input,
fraction capacities and invalid inputs. Run CLI integration tests explicitly:

```sh
python3 tests/integration/test_simulator.py build/debug/apps/clache_sim
```

The Python checks exercise exact CSV schemas, regional time-series files, job-count
independence, config precedence, stats sidecar, export bytes, source-path protection,
and post-warm-up TTL/counter deltas. No CMake edits are needed for C++ source discovery.
