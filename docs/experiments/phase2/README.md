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
feat/modeled-memory-evaluation in benchmarks/scripts/MODELED_MEMORY.md.

Policies use engine RNG seed0; trace seeds control generator randomness.
The candidate is frozen in docs/policy-diagnostics-plan.md. Fresh-seed
validation outputs will be added separately; discovery alone is not proof
of generalization. Cloud throughput timings are not controlled latency
measurements. Original artifacts are backed up under build/ on the local
machine and expire on GitHub after one day.

Tuned sampled modeled-memory data: run37350899374, source885abb0,
seed2 and the same GDSF reference budgets. The frozen candidate uses
forward_decay with tau900; default GDSF is the comparator. All12 rows
verified within1% of budget. The underlying engine policy is `clache`
with mode explicitly configured, which has the same score/update semantics
as the `clache_decay` alias. Source changes are experiment tooling only.
This is a sensitivity check on a previously seen seed, not held-out proof.
