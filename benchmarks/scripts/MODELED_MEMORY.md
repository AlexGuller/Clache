# Sampled modeled-memory sensitivity

This experiment compares `payload capacity + sampled reported metadata peak`.
It does **not** enforce actual RAM or claim complete memory accounting.

Run separately per workload, using existing Release binaries:

```sh
python3 benchmarks/scripts/run_modeled_memory.py \
  --workload cost_inversion --seed 3 --bin-dir build/release/apps \
  --out build/modeled-memory
```

Other supported workloads are `variable_size` and `mixed`. Defaults are ten million
requests, one million catalogue keys (200,000 for variable_size), 20% warm-up,
and reference capacity fractions 0.05 and 0.2. `--requests`, `--keys`, `--fractions`,
and `--expected-source-sha` are available. Output is namespaced by workload/seed;
existing nonempty experiment directories are rejected to prevent accidental mixing.
The generated trace remains in that directory and is reused by every replay.

Comparators are default `clache`, `gdsf`, `wtinylfu`, `s3fifo`, and `redis_lfu`.
After a tuned config has been locked independently, repeat `--tuned-set key=value`
to add a separate `clache_tuned` row; default Clache remains present.

## Calibration

For each fraction, run GDSF at `floor(working_set_bytes * fraction)`. Define the
budget as that capacity plus GDSF's sampled metadata peak. Every comparator starts
at the reference payload capacity. Calibration then proposes a capacity adjustment
using only its signed budget slack. If that would repeat a capacity, explore the
midpoint of an unvisited interval. This is bounded exploration, not a claim that
metadata changes monotonically with capacity.

There are at most five calibration observations and one verification replay per
policy/budget, including the reused GDSF reference observation. Retain the largest
**observed** feasible capacity. An observed feasible point within 1% of the budget
can stop exploration early. No miss-cost field is consulted when proposing or
selecting capacities. Six replays are a hard limit; calibration does not silently
continue until a favorable performance result appears.

Replay the selected capacity once more. Status explicitly distinguishes:

- `verified_within_1pct`: feasible and slack at most 1% of budget.
- `verified_slack_above_1pct`: feasible, but the bounded search left more slack.
- `verification_infeasible`: the selected capacity failed verification.
- `no_observed_feasible_capacity`: calibration found no feasible capacity.

The latter two cause a nonzero final exit status while preserving results. Do not
present their miss costs as successfully memory-matched comparisons. Points with
more than 1% slack also need their unused budget shown. The largest observed
feasible capacity need not be the globally largest feasible capacity.

## Artifacts and limits

`modeled_memory.csv` and `modeled_memory.md` show target, payload, sampled metadata,
modeled total, slack, verified miss cost/hit rates, status and replay counts.
`manifest.json` retains every attempt, commands, trace sidecar, binary source SHA,
runner source SHA, tuned overrides and limitations. Original simulator CSVs and
stats sidecars are retained for each replay; verification supplies the summary
metrics. CSV identity checks match policy, byte capacity, shared domain, trace
fingerprint, source revision, warm-up and measured request counts.

Current metadata estimates omit resident-set allocations, free-list capacity,
some scratch space and allocator overhead. Sampling can miss warm-up and transient
peaks. A fixed payload capacity is an allowance, not a measurement of actual
resident payload. Neither the 1% search tolerance nor a hypothetical safety margin
bounds these missing costs. Verification is the same deterministic trace and
configuration; it is not a held-out performance test. These are approximate
modeled-memory sensitivity results only.

Do not feed these CSVs into the ordinary equal-payload-capacity report: group by
`target_budget_bytes` and reference fraction instead. Timings from repeated local
or shared-runner replays are not isolated policy microbenchmarks.

Run lightweight tests with:

```sh
python3 -B -m unittest discover benchmarks/scripts/tests -p 'test_modeled_memory.py' -v
```
