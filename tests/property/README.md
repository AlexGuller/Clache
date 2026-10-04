# Property and determinism tests

Build and run the full Release suite (the reference comparison defaults to 1000
seeds per frequency mode in Release):

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCLACHE_WERROR=ON
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

For local Debug validation on macOS, use UBSan. Debug defaults to ten seeds per
mode, and the engine checks its invariants after every operation:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DCLACHE_WERROR=ON -DCLACHE_SANITIZE=undefined
cmake --build build/debug -j
CLACHE_PROPERTY_SEEDS=10 ctest --test-dir build/debug --output-on-failure
```

`CLACHE_PROPERTY_SEEDS` overrides either default and must be positive. To capture
comparison counts in a GoogleTest XML report:

```sh
CLACHE_PROPERTY_SEEDS=1000 ./build/release/tests/clache_tests \
  --gtest_filter=ReferenceComparison.ClacheBucketsAndDecayAcross1000Seeds \
  --gtest_output=xml:reference-comparison.xml
```

## Independent reference

The test policy retains complete access timestamp histories and recomputes
frequencies from those histories. It maintains its own cost EWMA from raw miss
samples, then sorts all residents at admission time. It does not use the
production estimators, indexed heap, or Clache policy to compute its decisions.
Both policies run through the same engine, so the comparison checks policy
behavior rather than providing an independent engine implementation.

Comparisons use the prescribed log key `K = log(F) + log(C) - alpha*log(S)` from
Spec §10.1 and §10.4. This matters for exact bucket decisions: direct `F*C/S`
arithmetic can round two values identically while their log keys differ. Seed 8,
operation 3075 exposed `F=1/300, S=13` versus `F=2/300, S=26`, both with `C=.01`.
A focused regression computes the expected decision from the platform's log keys;
it does not assume a particular libm rounding result. Bucket comparisons have no
near-tie exemption. G3 continues to verify rejection of bit-identical keys.

Decay frequencies are recomputed as sums of decayed timestamps. Decisions with
relative value ambiguity below `1e-9` among the leading victims or between a
visited victim and the candidate are counted and synchronized to preserve equal
resident sets for subsequent comparisons. The count must remain below 0.1% of
decay decisions. XML properties report both that count and its denominator.

Each seed includes 10,000 operations over 50 keys, varying sizes, costs, TTLs,
invalidations, bucket crossings, and jumps beyond a full window. Separate coverage
runs every available production policy through generic invariants and checks LRU
against an independent list model.

## Determinism scope

A 200,000-operation mixed trace with seed 7 is replayed serially and twice using
batches of up to eight independent policy engines. Every registered production
policy participates. Exact comparisons cover decision bytes, sampled decision-log
records, counters, resident totals, and trace/config fingerprints.

This is engine-level coverage. The simulator CLI, trace generator, and baseline
policy branches are not present in this feature stack. The exact §19.5
`clache_sim --jobs 1/8` comparison of `results.csv` and `decisions.ndjson`, plus the
cost-inversion capacity checks, remain integration work when those branches land.
The engine replay test does not claim to validate those files or CLI behavior.

## Local validation

On Apple Clang 17, Release with Werror passed 1000 seeds in each frequency mode:
20,000,000 generated operations and 17,896,419 admission decisions, including
8,946,246 decay decisions and **zero** skipped near ties. Debug with Werror and
UBSan passed all 72 tests with ten seeds per mode, including the 200,000-operation
determinism test. The full Release suite also passes with all tests in one process,
covering registration interactions between golden and property tests.
