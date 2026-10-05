# Phase 2 results v0 — 2026-10-05

## Conclusion

The full synthetic matrix passes the Spec §20.2 go/no-go criterion for both
**default Clache** and the frozen **decay-900 candidate**. On fresh trace
seeds 3–5, the candidate has lower median miss cost than default GDSF in
**51 of 56 workload/capacity cells**. Its five GDSF losses are all scans,
with a worst loss of **0.239%**. This measures eviction quality on generated
traces, not Redis server performance or end-to-end application latency.

The candidate is `clache_decay` with `decay_tau_seconds=900`, retaining
strict admission, min_observations=1, cost/size weighting, and the default
ghost budget. Library defaults are unchanged. The configuration is supplied
in [clache-decay900.yaml](../benchmarks/configs/clache-decay900.yaml).

## Reproducibility and scope

- Full run: [37350550654](https://github.com/AlexGuller/Clache/actions/runs/37350550654).
- Published reports: [37354924179](https://github.com/AlexGuller/Clache/actions/runs/37354924179).
- Source: `0c46e288` (the full SHA is in every CSV and provenance JSON).
- Eight workloads × seven capacities (.001, .005, .01, .02, .05, .1, .2)
  × five trace seeds × 17 configurations = **4,760 complete replays**.
- Each trace: 10M requests, 1M key catalogue (200k for variable_size),
  first 20% warm-up. Policies share exact traces, byte capacities and
  engine RNG seed0. Generator seeds1–5 differ independently.
- The 17 configurations are all16 existing policies/ablations plus the
  separately labelled candidate. Raw candidate engine policy is
  `clache_decay`; derived reports label it `clache_tuned`.
- Selection used only seed1. Seeds3–5 were not inspected before the global
  candidate was frozen. Seed2 had been observed in the earlier reduced run.
  Primary evidence below uses seed medians over **held-out seeds3–5**;
  all-five-seed tables are provided separately.
- Builds used GCC13, Release, warnings as errors, invariants/sanitizers off.
  Exact CMake flags, CPU descriptions, commands and trace metadata remain
  in the run artifacts and local downloaded copies.

Data: [all replay rows](experiments/phase2/full-results.csv),
[held-out summary](experiments/phase2/held-out-summary.csv),
[all-five summary](experiments/phase2/all-five-summary.csv),
[provenance](experiments/phase2/validation-provenance.json).

## Go/no-go

The criterion requires at least10% lower median miss cost than the best of
LRU/LFU/LFU-DA at at least three capacities on at least three designated
cost-heterogeneous workloads. It does not require beating GDSF everywhere.

| Workload | Default: qualifying capacities | Decay300 | Candidate900 |
|---|---:|---:|---:|
| cost_inversion | 7/7 | 7/7 | 7/7 |
| variable_size | 4/7 | 4/7 | 4/7 |
| mixed | 6/7 | 7/7 | 7/7 |
| regional_split | 7/7 | 7/7 | 7/7 |
| scan | 0/7 | 0/7 | 0/7 |
| **Verdict** | **PASS: 4/5 workloads** | **PASS: 4/5** | **PASS: 4/5** |

All-five-seed qualifying counts are the same. The earlier reduced-grid
FAIL used only three capacities: variable_size and mixed each won at two
of those three. The full grid supplies additional qualifying points.
Default Clache passing now is a consequence of fuller measurement, not a
change to its implementation or to the criterion.

## Held-out comparison

Positive percentages below mean candidate miss-cost savings, computed as
`1 - median(candidate cost) / median(comparator cost)`.

| Workload, capacity5% | vs default Clache | vs GDSF | vs simulated Redis LFU |
|---|---:|---:|---:|
| zipf | 3.84% | 12.62% | 32.77% |
| cost_inversion | 20.14% | 11.90% | 65.87% |
| variable_size | 0.37% | 11.32% | 13.43% |
| scan | -0.12% | -0.21% | -1.14% |
| hotset_shift | 1.30% | 10.01% | 30.96% |
| burst | 3.54% | 11.93% | 31.98% |
| regional_split | 3.22% | 10.40% | 29.66% |
| mixed | 6.25% | 1.07% | 21.81% |

| Comparator | Candidate wins / losses across56 cells | Worst candidate loss |
|---|---:|---:|
| GDSF | 51 / 5 | 0.239%, scan10% |
| GDSF-window | 49 / 7 | 0.756%, scan1% |
| W-TinyLFU approximation | 51 / 5 | 0.787%, scan10% |
| S3-FIFO | 49 / 7 | 1.507%, scan5% |
| Redis LFU approximation | 49 / 7 | 1.142%, scan5% |
| Redis LRU approximation | 51 / 5 | 0.776%, scan10% |

Against default Clache, candidate wins46/56 cells and loses10/56. Its worst
regression is **3.393%**, hotset_shift at0.1% capacity. Its geometric mean
ratio over the56 ratios of seed medians is0.95168 (4.83% lower); this is a
supplemental equally weighted aggregate, not an overall application saving.
No statistical significance claim is made from three held-out seeds.

Decay300 is a competitive simpler alternative:49/56 GDSF wins and only a
1.224% worst regression against default Clache. Candidate900 was selected
by the predeclared discovery objective; longer history has an adaptation
tradeoff, and tuned-vs-default comparisons are not equal-tuning-budget
comparisons against GDSF or the other baselines.

## What caused the large GDSF loss?

Independent code reviews found no default-path Clache or GDSF correctness
violation of the specified hooks. The strongest causal evidence is the
frequency-history ablation on the same seed1 trace at20% capacity:

| Setting | Miss-cost ratio to default Clache |
|---|---:|
| Default rolling300s | 1.00000 |
| Always admit | 0.99998 |
| No ghosts | 0.99998 |
| Cost oracle | approximately0.99992 |
| Decay300s | 0.55585 |
| Decay900s | 0.54877 |
| Rolling900s, five buckets | 0.55760 |
| Rolling1200s, five buckets | 0.54816 |

Rolling frequency zeros objects idle beyond roughly240–300 seconds.
Many valuable cold objects in this trace recur less often. GDSF retains
resident counts with eviction-driven aging; decay retains a smoothly
shrinking history. The controlled history changes remove most of the gap,
while changing admission or supplying hit-time cost observations does not.

On held-out seeds at20% cost_inversion capacity:

- Default Clache median miss cost:62,181,321,244 µs, **75.52% above GDSF**.
- Candidate900:34,306,398,430 µs, **3.16% below GDSF** and **44.83% below default**.

Two-hit probation helps scans but prevents retaining sparse expensive
objects after their first miss. Both predeclared long-history/probation
combinations failed to improve the discovery aggregate and were not selected.
Window1200 is a diagnostic control: roughly1000-second traces scarcely
exercise its expiration horizon. It is not evidence of steady-state adaptation.
GDSF-window shares telemetry but refreshes scores only on its specified
hooks; it does not perform Clache's bucket-wide resident rebuilds.

## Scan and mixed decomposition

Each full scan/mixed trace has exactly **1.6M measured fresh scan requests**.
These keys never recur, so their miss costs are compulsory. The exact
20%-warm-up scan floor is271.894–272.407B µs across the scan seeds and
271.646–272.351B µs for mixed. Observed scan/mixed durations are about1000s.
The floor is91.84–93.06% of scan's potential request cost and51.06–52.54%
of mixed's potential cost.

Subtracting each seed's exact floor before taking five-seed medians:

| At5% capacity | Candidate/GDSF total-cost ratio | Ratio after removing scan floor |
|---|---:|---:|
| scan | 1.00212 | 1.06359 |
| mixed | 0.98928 | 0.92729 |

Thus scan's small0.21% total-cost loss still represents a6.36% loss on the
residual; mixed's1.07% total improvement represents7.27% on the residual.
The residual still includes other compulsory first touches, so it is not
wholly avoidable cost. The scan weakness remains and is not hidden by the
large compulsory floor. Exact trace summaries are preserved in
[trace-summaries.csv](experiments/phase2/trace-summaries.csv).

## Warm-up sensitivity

At5% capacity, seed3, all8 workloads were rerun with10% and30% warm-up and
compared with the20% full run. **No candidate-vs-GDSF or candidate-vs-default
comparison changed direction.** Cost-inversion candidate/GDSF ratios were
0.8966 /0.8817 /0.8709 at10% /20% /30%; scan ratios remained approximately
1.0021 /1.0021 /1.0020. Changing warm-up also changes the measured time
segment, so this is not a pure initialization experiment. This sensitivity
check covers one capacity and one seed, not the entire full matrix.
[Data](experiments/phase2/warmup-sensitivity.csv).

## Memory sensitivity

Current metadata counters omit some containers/allocator overhead and
sample peaks after warm-up. They do not establish actual total RAM usage.
Separate bounded calibration matched **payload capacity + reported sampled
metadata peak** to default GDSF's reference budgets at5% and20%, on seed2.
Calibration selected by memory/slack only, never by miss cost, then replayed
the chosen capacity. Every reported comparison was within1% of its target.

| Workload | Candidate saving vs GDSF,5% reference budget | 20% reference budget |
|---|---:|---:|
| cost_inversion | 13.18% | 3.12% |
| variable_size | 11.36% | 11.29% |
| mixed | 1.05% | 0.08% |

This supports gains under approximately matched modeled memory, not a hard
RAM-budget claim. Default Clache's large20% cost-inversion loss persists at
matched modeled memory; its reported metadata was already essentially equal
to GDSF's. Data and limitations are in
[modeled-memory-default.csv](experiments/phase2/modeled-memory-default.csv)
and [modeled-memory-tuned.csv](experiments/phase2/modeled-memory-tuned.csv).
The calibration tool lives on `feat/modeled-memory-evaluation`.

## Tooling fixes and remaining work

- `fix/simulator-measured-timing`: whole-replay time divided by all replayed
  records, including warm-up and DEL;15 simulator and6 CLI tests passed.
- `fix/trace-cache-validation`: reject incompatible/missing trace sidecars
  before reuse without overwriting inputs;24 tests passed.
- These fixes are pushed, not merged. Full replay source predates the timing
  fix. Derived CSVs correct the known denominator using10M all-GET records;
  raw artifacts are untouched. Cloud timings include shared-worker/replay
  overhead and cannot support CPU-latency claims.
- Longer-duration traces with repeated shifts are still needed to evaluate
  the adaptation tradeoff of900-second decay. This run typically sees one
  hotset shift near600s. No production default was changed.
- Real Redis integration, real database/application measurements, full
  allocation accounting and enforced total-memory budgets remain future work.
- Broader §24 questions (victim caps, metadata-budget sweeps, cost drift,
  equally tuned baselines) are not claimed complete by these experiments.
