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
  `ns_per_request` divides this by all replayed records, including warm-up and DEL.
  ALL rows sum regional replay durations and record counts before division; they
  do not report pool elapsed time.
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

CMake supplies the project version and source Git revision at configure time to
both binaries. Modified tracked sources add a `-dirty` suffix; source archives
without Git report `unknown`. Reconfigure after source changes before measuring.
The simulator never reads runtime working-directory Git state. Matrix `env.json`
records the compiler, flags, operating system, CPU and experiment parameters.

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

## Engine integration

Every mutating engine operation now purges expired entries, including deletion,
cost hints and aborted misses. The `expired_keys()` accessor exposes expiry
removals from the latest operation to future storage adapters. Expiry takes
precedence over invalidation at the same timestamp.

The simulator supplies `max(1, floor(capacity / mean_charged_size))` as the policy
entry-count hint; mean charged size is the trace prepass mean object size plus
per-entry overhead. Empty traces retain the engine's unknown-hint fallback.
TinyLFU therefore sizes its sketch from the trace instead of assuming 4096-byte
objects. The hint participates in the effective config fingerprint.

Engine construction now uses the same public validation functions as the parser.
A finite nonnegative miss-cost default may be outside sample-clamping bounds;
observed samples are still clamped independently.

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

### Replay timing

`wall_seconds` times the complete replay, including warm-up, GET, and DEL
operations and simulator overhead. `ns_per_request` divides that time by all
replayed records in the row's domain. The CSV `requests` column remains the
post-warm-up GET count used for hit/cost metrics; it is not the timing denominator.
A domain containing only deletes can therefore report nonzero operation latency.
For regional `ALL` rows, elapsed times and replayed record counts are summed
across domains before division. This is the weighted per-operation replay time,
not the concurrent runs' elapsed makespan. An empty replay reports zero
`ns_per_request`.
