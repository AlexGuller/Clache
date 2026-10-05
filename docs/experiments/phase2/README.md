# Phase 2 experiment data

Discovery data: GitHub Actions runs 37348790147 (source 3a409b1) and
37349126393 (source 3b22b88). All eight workloads, trace seed1, 10M requests,
20% warmup, capacity fractions .01/.05/.2. Source changes between these runs
are experiment configuration/workflow/documentation only; policy and
simulator code match. `discovery-details.csv` preserves labelled per-cell
miss costs and paired ratios; `discovery-summary.csv` uses equal-cell
geometric means (a supplemental aggregate, not the spec go/no-go test).
Source labels distinguish oracle controls from deployable configurations.

Default sampled modeled-memory data: run37349856868, source6e1b9ab,
trace seed2, workloads cost_inversion/variable_size/mixed. Each target is
GDSF payload capacity at5% or20% plus its reported sampled metadata peak.
Selection considers budget slack only; final selected capacities are
replayed. All30 rows satisfy the target within1%; this is not exact RAM:
allocation omissions and unsampled peaks remain. Full method is on
feat/modeled-memory-evaluation in docs/modeled-memory-evaluation.md.

Policies use engine RNG seed0; trace seeds control generator randomness.
The candidate is frozen in docs/policy-diagnostics-plan.md. Fresh-seed
validation outputs will be added separately; discovery alone is not proof
of generalization. Cloud throughput timings are not controlled latency
measurements. Original artifacts are backed up under build/ on the local
machine and expire on GitHub after one day.
