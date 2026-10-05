# Policy diagnosis and validation plan

## Starting evidence

The reduced run (source 378c716, 480 replays, trace seeds 1–2) has real
large-capacity losses: cost inversion at 20% uses 1.7612 times GDSF's
median miss cost. Both report about 170.44 MiB metadata there; unequal
reported metadata does not explain this loss. Default Clache still beats
Redis LFU approximations on all non-scan workloads at every measured size.

Audits found no default-path Clache/GDSF algorithm defect against the
specified hooks. GDSF-window uses the engine estimator but does not refresh
untouched resident scores each bucket boundary; Clache does. This is a
specified distinction, not a clean frequency-estimator-only comparison.

Hypotheses to test:
1. Rolling 300-second frequency drops sparse expensive objects too early.
2. Strict admission adds losses beyond the frequency score itself.
3. First-observation admission lets expensive one-hit scan keys displace
   reusable objects; probation may help.
4. Miss-only cost estimation with jitter contributes stale estimates.

## Discovery

Use trace seed 1, all eight workloads, 10 million requests each, 20% warmup,
capacity fractions 1%, 5%, 20%. Run the explicit cases in
`benchmarks/configs/diagnostics.json`, preserving distinct configuration
labels and fingerprints. Rolling window cases use five buckets each.
Cost-oracle results are diagnostic upper-bound information and cannot be
selected as a deployable default. Retain one-million-request interval data.

Select at most one global configuration using the geometric mean of its
miss-cost ratios to default Clache over all 24 workload/capacity cells.
Report the full per-cell table, worst regression, and best-baseline ratios;
a better average alone is insufficient to claim universal improvement.
Prefer a simpler configuration if improvements are negligible. If combining
individually promising knobs, confirm that combination on seed 1 before
locking it. Do not select different knobs per workload.

## Validation

After locking the candidate, run all eight workloads, all seven spec
capacity fractions, trace seeds 1–5, and all registered baseline/ablation
policies plus the candidate. Seeds 3–5 are fresh validation data; report
their results separately from discovery seeds. Preserve candidate settings
before viewing those seeds. Apply the existing spec go/no-go criterion,
and show losses to stronger baselines even if it passes. No algorithm or
default changes solely to improve known trace cases.

## Memory and timing limits

Payload capacity excludes metadata. Current metadata reporting omits some
containers/allocator overhead and samples peaks. It cannot establish equal
actual process memory or enforce a hard total budget. Any calibration using
payload capacity plus reported peak must be explicitly called a modeled
memory sensitivity experiment, not an equal-RAM production benchmark.

An independent simulator timing fix is on fix/simulator-measured-timing:
whole-replay time must be divided by all replayed records, including warmup
and DEL. This does not change eviction-quality metrics. Cloud timing also
includes concurrent-worker contention and is not a microbenchmark.

Before inspecting diagnostic outcomes, predeclare two interaction cases:
window900 + min_observations2 and decay900 + min_observations2. These are
recorded in benchmarks/configs/confirmation.json. Candidate eligibility
requires no discovery cell more than 10% worse than default Clache; among
eligible cases choose lowest equal-cell geometric mean cost ratio.
The 1200-second window is a diagnostic near-cumulative-history control
for these roughly 1000-second traces, not evidence of long-run adaptation.

## Candidate locked before fresh-seed validation

Discovery runs 37348790147 and 37349126393 completed. Matched trace
fingerprints, byte capacities, request counts, and engine seeds across all
24 seed-1 cells. Selected `clache_decay` with tau=900 seconds, default
min_observations=1, strict admission, default cost and size weighting and
ghost budget. No per-workload settings are selected.

Its equal-cell geometric mean cost ratio to default Clache is 0.9284
(7.16% reduction); worst cell is 1.0267 (2.67% regression). It passes the
predeclared 10% per-cell regression guard. Existing decay tau300 is also
strong (0.9348 mean, 1.0079 worst) and remains in the full comparison.
The oracle and window1200 control are ineligible for selection. Two-hit
probation combinations did not improve the aggregate objective.

Fresh seeds3–5 have not been inspected. The candidate above is now frozen
for all workloads/capacities/seeds. Full matrix: 16 existing policies plus
this separately labelled candidate, 8 workloads × 7 capacities × 5 seeds
= 4,760 replays. The standard library defaults are not changed by tuning.
