"""Validate the fixed reduced cloud benchmark matrix before reporting."""
import csv
import itertools
import sys
from pathlib import Path

workloads = "zipf cost_inversion variable_size scan hotset_shift burst regional_split mixed".split()
policies = "clache lru lfu lfu_da gdsf gdsf_window wtinylfu s3fifo redis_lru redis_lfu".split()
expected = set(itertools.product(workloads, (1, 2), (0.01, 0.05, 0.2), policies))
seen = set()
revisions = set()
for path in Path(sys.argv[1]).glob("results-*/*-seed*.csv"):
    workload = path.name.split("-seed")[0]
    with path.open() as stream:
        for row in csv.DictReader(stream):
            key = (workload, int(row["seed"]), float(row["capacity_fraction"]), row["policy"])
            if key not in expected or key in seen:
                raise SystemExit(f"Unexpected or duplicate result: {key}")
            if int(row["requests"]) != 8000000 or int(row["warmup_requests"]) != 2000000:
                raise SystemExit(f"Incomplete trace replay: {key}")
            seen.add(key)
            revisions.add(row["git_sha"])
if seen != expected or len(revisions) != 1:
    raise SystemExit(f"Incomplete matrix: {len(seen)}/480 rows, revisions={revisions}")
print("PASS: 480 full-length simulations from one source revision")
