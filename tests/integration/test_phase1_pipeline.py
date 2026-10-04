#!/usr/bin/env python3
"""Exercise the assembled Phase 1 binaries; intentionally uses real engine code.

python3 tests/integration/test_phase1_pipeline.py --bin-dir build/release/apps \
    --report-script benchmarks/scripts/report.py --work-dir /tmp/clache-pipeline

Requires tracegen, simulator, config/trace IO, all policies, and real engine
implementations in one build. Output data is retained for inspection.
"""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys

WORKLOADS = (
    "zipf", "cost_inversion", "variable_size", "scan", "hotset_shift", "burst",
    "regional_split", "mixed",
)
POLICIES = (
    "lru", "lfu", "lfu_da", "gds", "gdsf", "gdsf_window", "wtinylfu", "s3fifo",
    "redis_lru", "redis_lfu", "clache", "clache_noadmit", "clache_nocost",
    "clache_nosize", "clache_decay", "clache_noghost",
)


def run(*command):
    result = subprocess.run([str(x) for x in command], text=True, capture_output=True)
    if result.returncode:
        raise AssertionError(
            f"Command failed ({result.returncode}): {command}\n"
            f"{result.stdout}\n{result.stderr}"
        )
    return result


def rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def stable_results(path):
    return [
        {key: value for key, value in row.items()
         if key not in {"wall_seconds", "ns_per_request"}}
        for row in rows(path)
    ]


def check_results(path, expected_policies, expected_rows):
    data = rows(path)
    assert len(data) == expected_rows, (path, len(data), expected_rows)
    assert {r["policy"] for r in data} == set(expected_policies)
    for row in data:
        assert int(row["requests"]) == int(row["hits"]) + int(row["misses"])
        assert int(row["requests"]) > 0
        for column in ("hit_rate", "byte_hit_rate", "cost_hit_rate"):
            assert 0 <= float(row[column]) <= 1, (column, row)
        assert float(row["total_miss_cost_us"]) >= 0
        assert row["trace_fingerprint"] and row["config_fingerprint"]
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--report-script", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--determinism-requests", type=int, default=200_000)
    args = parser.parse_args()
    if args.determinism_requests < 1:
        parser.error("--determinism-requests must be positive")
    out = args.work_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    gen = args.bin_dir.resolve() / "clache_tracegen"
    sim = args.bin_dir.resolve() / "clache_sim"
    for binary in (gen, sim):
        if not binary.is_file():
            parser.error(f"Missing binary: {binary}")
    files = []
    for workload in WORKLOADS:
        for seed in (1, 2):
            trace = out / f"{workload}-seed{seed}.ctrace"
            run(gen, "--workload", workload, "--requests", 2000, "--keys", 100,
                "--seed", seed, "--format", "bin", "--out", trace)
            meta = json.loads(Path(str(trace) + ".meta.json").read_text())
            assert meta["workload"] == workload
            assert int(meta["request_count"]) == 2000
            assert int(meta["seed"]) == seed
            assert int(meta["working_set_bytes"]) > 0
            result = out / f"{workload}-seed{seed}.csv"
            run(sim, "--trace", trace, "--policies", ",".join(POLICIES),
                "--capacity-fractions", "0.01,0.05,0.1", "--jobs", 4,
                "--warmup-fraction", "0.2", "--out", result)
            check_results(result, POLICIES, len(POLICIES) * 3)
            files.append(result)
    report = out / "report"
    run(sys.executable, args.report_script.resolve(), *files, "--out", report, "--no-plots")
    assert (report / "report.md").is_file()
    summary = rows(report / "summary.csv")
    assert len(summary) == len(WORKLOADS) * len(POLICIES) * 3
    assert all(int(row["seeds"]) == 2 for row in summary), "trace seeds failed to aggregate"
    first_md = (report / "report.md").read_bytes()
    first_csv = (report / "summary.csv").read_bytes()
    run(sys.executable, args.report_script.resolve(), *reversed(files),
        "--out", report, "--no-plots")
    assert first_md == (report / "report.md").read_bytes()
    assert first_csv == (report / "summary.csv").read_bytes()

    trace = out / "determinism-mixed-seed7.ctrace"
    run(gen, "--workload", "mixed", "--requests", args.determinism_requests,
        "--keys", 1000, "--seed", 7, "--format", "bin", "--out", trace)
    # Regenerate at a different path: trace bytes must not depend on the filename.
    duplicate = out / "determinism-copy.ctrace"
    run(gen, "--workload", "mixed", "--requests", args.determinism_requests,
        "--keys", 1000, "--seed", 7, "--format", "bin", "--out", duplicate)
    assert trace.read_bytes() == duplicate.read_bytes()
    determinism_meta = json.loads(Path(str(trace) + ".meta.json").read_text())
    assert int(determinism_meta["request_count"]) == args.determinism_requests
    for jobs in (1, 8):
        result = out / f"determinism-jobs{jobs}.csv"
        run(sim, "--trace", trace, "--policies", ",".join(POLICIES),
            "--capacity-fractions", "0.05", "--jobs", jobs,
            "--decisions", out / f"decisions-jobs{jobs}.ndjson",
            "--decision-sample-rate", "0.1", "--out", result)
        check_results(result, POLICIES, len(POLICIES))
    assert stable_results(out / "determinism-jobs1.csv") == stable_results(
        out / "determinism-jobs8.csv")
    assert (out / "decisions-jobs1.ndjson").read_bytes() == (
        out / "decisions-jobs8.ndjson").read_bytes()
    evidence = {
        "workloads": list(WORKLOADS), "policies": list(POLICIES),
        "smoke_requests_per_trace": 2000, "trace_seeds": [1, 2],
        "capacity_fractions": [0.01, 0.05, 0.1], "result_rows": 768,
        "determinism_requests": args.determinism_requests,
        "jobs_compared": [1, 8], "status": "passed",
        "scope": "Implementation smoke checks; not the full research matrix",
    }
    (out / "validation.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"PASS: 768 smoke rows, 16-policy determinism, stable report; artifacts: {out}")


if __name__ == "__main__":
    main()
