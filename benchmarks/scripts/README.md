# Experiment tools

Run from the repository root (Python 3.9+ and Bash 3.2+):

```sh
DRY_RUN=1 bash benchmarks/scripts/run_matrix.sh
WORKLOADS='zipf mixed' SEEDS='1 2' REQUESTS=10000 KEYS=1000 \
  JOBS=2 bash benchmarks/scripts/run_matrix.sh
python3 benchmarks/scripts/report.py benchmarks/results/RUN/*.csv --out report --no-plots
python3 -m unittest discover benchmarks/scripts/tests -v
```

The matrix requires `clache_tracegen` and `clache_sim` in `build/release/apps`.
`BIN_DIR` selects another location. Defaults live in `benchmarks/configs/matrix.env`;
existing environment variables override them. `MATRIX_CONFIG` selects a replacement
shell configuration. Set `POLICIES` to the registered policies available in a partial
build. Defaults include all Phase 1 policies and Clache ablations.

`TRACE_DIR`, `RESULT_DIR`, `JOBS`, `DOMAIN_BY`, `WORKLOADS`, `SEEDS`, `REQUESTS`,
`KEYS`, `VARIABLE_SIZE_KEYS`, `CAPACITY_FRACTIONS`, and `WARMUP_FRACTION` are
configurable. Regional runs default to the shared-domain experiment; set
`DOMAIN_BY=region` for separate regions. Comparing equal total capacity with the
shared domain requires a separate capacity-scaled run as described in Spec §14.5.

Dry runs print commands without creating directories, traces, results, or env.json.
Existing traces are reused: select a fresh `TRACE_DIR` when changing generation
parameters. Failed generation files are renamed `.failed` so they cannot be reused.
Failures are logged, subsequent experiments continue, and the script ultimately
returns nonzero. Only successful simulator outputs enter the report. Each run
captures CPU, OS, Git SHA, CMake compiler/flags (when available), and parameters in
`env.json`. Simulator outputs retain trace/config fingerprints and build identity.

## Report inputs and outputs

`report.py` validates every results.csv column from Spec §14.4. It obtains workload
and trace seed from `<trace_path>.meta.json`, searching the recorded path and then
relative to the results file. Without a sidecar it accepts
`<workload>-seed<N>.<extension>`. The CSV `seed` is the engine RNG seed; it is not
used as the trace seed. Duplicate workload/domain/capacity/policy/trace-seed rows
are rejected: report distinct configurations in separate invocations.

Outputs:

- `report.md`: tables, per-variant go/no-go evidence, modern-policy comparisons,
  explicit missing policies/workloads and unmatched seeds.
- `summary.csv`: deterministic sorted summaries; numeric values have six decimals.
- PNG cost curves per workload/domain, if optional matplotlib is installed.
  Filenames hex-encode workload/domain names. Missing matplotlib only skips plots.

For each seed, normalized cost divides by the best available LRU/LFU/LFU-DA cost
on that seed. The report then computes median/min/max of those ratios. Raw miss
cost has its own median/min/max; other metrics are medians. A missing matched
baseline produces `NA`, never a fabricated zero. Zero divided by zero is 1;
a positive cost divided by zero is `NA`. The normalized-seed count exposes
omitted comparisons.

Go/no-go uses the ratio of **median raw costs**, on the common seeds of a variant
and available classic baselines, choosing the best baseline median. PASS requires
one fixed Clache variant to improve by at least 10% at three distinct capacity
fractions on three of the five specified heterogeneous workloads, within one
domain. Different variants or regions cannot be combined to manufacture a pass.
A zero baseline never establishes improvement. FAIL requires measurements at
three or more capacities on every heterogeneous workload; otherwise the verdict
is INSUFFICIENT EVIDENCE. Missing policies are listed; verdicts apply only to the
available baselines. Five trace seeds remain the protocol target; the measured
seed counts are explicit. Modern comparison ratios greater than 1 mean Clache
loses. Neither report nor matrix exit status treats a measured FAIL as a tool error.

`--timeseries PATH` optionally validates the interval CSV header and records its
row count; aggregate statistics always come from results.csv. `--no-plots` avoids
loading matplotlib. Identical inputs produce byte-identical Markdown and summary
CSV. Plot image bytes and environment capture are not covered by that guarantee.
