# Synthetic trace generator

Build normally, then run `build/release/apps/clache_tracegen --list` for every
workload and its complete defaults. For example:

```sh
build/release/apps/clache_tracegen --workload mixed --requests 200000 \
  --keys 1000 --seed 7 --param scan_every=10000 --param scan_len=2000 \
  --out mixed.ctrace --format bin
```

`--param key=value` overrides defaults. `--requests`, `--keys`, and `--seed`
are aliases; later assignments win. Size values accept integral byte counts or
KiB/MiB/GiB and KB/MB/GB suffixes. Output is CSV by default; `--format bin`
uses the version 1 little-endian trace format. Every output gets a `.meta.json`
sidecar with the resolved parameters, actual emitted key count, actual working
set (maximum emitted size per key), and file fingerprint. Zero requests produce
a valid empty trace. Invalid parameters are rejected before opening output.
I/O or generation failures may leave a partial file; no sidecar is written until
the trace is complete.

All randomness uses the repository RNG. Streams 1–8 cover arrivals, popularity,
jitter, hot-set changes, region selection, burst events, burst traffic, and region
permutations. Per-key streams 101–103 cover size, base cost, and TTL. Catalogue
ranks are zero based and key IDs use the Spec §15.1 bijection. Fresh scan and burst
ranks start beyond the catalogue and never overlap. Attribute generation is lazy
and stable per key. Reproducibility assumes the same math library, as documented
by `rng.h`. Arrival intervals are truncated to integer nanoseconds; timestamps
start at zero and may repeat.

Workload details where the spec leaves choices open:

- **Scans:** start after the first `scan_every` background requests, then occupy
  the first `scan_len` requests in each following period. Length must be at most
  the period; zero disables scans. Scan keys never recur.
- **Hot-set shifts:** partially shuffle the catalogue permutation at every shift
  boundary, selecting a new uniform hot set without duplicate IDs. A new set can
  overlap the previous one. `hot_keys` is capped at catalogue size, making defaults
  valid for small catalogues. Size and base cost follow key identity across shifts.
- **Bursts:** independent Poisson events may overlap. Active bursts collectively
  receive `burst_share`; a selected burst is uniform among active bursts, then a
  key is uniform within it. Expired bursts stop receiving traffic. Zero event rate
  disables bursts.
- **Regions:** `floor(overlap * keys)` leading popularity ranks map to the same
  IDs; each region independently shuffles the remaining ranks. Supply exactly one
  positive `cost_multipliers` value per region. Region IDs are zero based. The
  region/catalogue product is limited to 100 million permutation entries.
- **Variable size:** uses loguniform sizes, default 1 KiB–8 MiB, and computes cost
  from the integer emitted size. `size.min/max` may override bounds; `size.dist`
  does not change this workload's defining distribution.
- **Mixed:** combines cost-inversion base costs, variable-size distribution,
  scans, and hot-set shifts. Cost inversion takes precedence over the standalone
  variable-size cost formula. Scan cost takes precedence for fresh scan entries.
- **Cost bounds:** base costs and final jittered/region-scaled costs are clamped
  to `cost.min_us/max_us` (default 10–30,000,000). TTL choices are positive u32
  seconds; `ttl.fraction=0` emits no TTL. Size/cost fractions are truncated to the
  trace format's integral units.

The generator version is `clache-tracegen-1`. Build systems may define the quoted
C++ macro `CLACHE_GIT_SHA` for `src/core/tracegen.cpp`. Without build provenance it
honestly reports `unknown`; it never reads the runtime working directory's Git
state.

The library API is in `include/clache/tracegen.h`. `generate_trace_records` streams
records to a callback and returns observed statistics without a file fingerprint.
`generate_trace` writes both trace and sidecar. Memory is O(catalogue + emitted
unique keys + active bursts), plus per-region permutations when applicable.

CLI integration checks:

```sh
python3 tests/integration/test_tracegen.py build/debug/apps/clache_tracegen
```
