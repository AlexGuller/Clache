"""Run explicit, separately labelled policy experiments on one shared trace."""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct


def trace_summary(trace, metadata, warmup=0.2):
    """Exact time range and compulsory scan cost for our generated workloads."""
    count = metadata["request_count"]
    cutoff = int(count * warmup)
    params = metadata["params"]
    every = int(params.get("scan_every", "0"))
    length = int(params.get("scan_len", "0"))
    summary = {"records": 0, "first_timestamp_ns": None, "last_timestamp_ns": None,
               "measured_potential_cost_us": 0, "measured_scan_requests": 0,
               "measured_compulsory_scan_cost_us": 0, "scan_intervals": {}}
    record = struct.Struct("<QQIIIHBB")
    with trace.open("rb") as stream:
        header = stream.read(64)
        if header[:8] != b"CLACHETR" or struct.unpack_from("<IIQ", header, 8) != (1, 32, count):
            raise ValueError("Unexpected binary trace format")
        while chunk := stream.read(32 * 65536):
            for timestamp, key, size, cost, ttl, region, op, reserved in record.iter_unpack(chunk):
                index = summary["records"]
                if summary["first_timestamp_ns"] is None:
                    summary["first_timestamp_ns"] = timestamp
                summary["last_timestamp_ns"] = timestamp
                if index >= cutoff and op == 0:
                    summary["measured_potential_cost_us"] += cost
                    if every and index >= every and index % every < length:
                        summary["measured_scan_requests"] += 1
                        summary["measured_compulsory_scan_cost_us"] += cost
                        interval = str((index - cutoff) // 1000000)
                        summary["scan_intervals"][interval] = summary["scan_intervals"].get(interval, 0) + cost
                summary["records"] += 1
    if summary["records"] != count:
        raise ValueError("Incorrect trace record count")
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", required=True)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--cases", default="benchmarks/configs/diagnostics.json")
    parser.add_argument("--bin-dir", default="build/release/apps")
    parser.add_argument("--out", default="diagnostics")
    parser.add_argument("--requests", type=int, default=10000000)
    parser.add_argument("--keys", type=int)
    parser.add_argument("--capacities", default="0.01,0.05,0.2")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    cases = json.loads(Path(args.cases).read_text())
    names = [c["name"] for c in cases]
    if len(names) != len(set(names)) or any(Path(n).name != n or n in (".", "..") for n in names):
        raise ValueError("Case names must be unique simple filenames")
    output = Path(args.out)
    output.mkdir(parents=True, exist_ok=True)
    trace_dir = Path(os.environ.get("RUNNER_TEMP", str(output))) / "diagnostic-traces"
    trace_dir.mkdir(parents=True, exist_ok=True)
    trace = trace_dir / f"{args.workload}-seed{args.seed}.ctrace"
    if (output / "manifest.json").exists():
        raise ValueError("Output already contains a run; choose a fresh directory")
    keys = args.keys or (200000 if args.workload == "variable_size" else 1000000)
    bins = Path(args.bin_dir)
    generate = [str(bins / "clache_tracegen"), "--workload", args.workload,
                "--seed", str(args.seed), "--requests", str(args.requests),
                "--keys", str(keys), "--format", "bin", "--out", str(trace)]
    subprocess.run(generate, check=True)
    shutil.copyfile(str(trace) + ".meta.json", output / (trace.name + ".meta.json"))
    metadata = json.loads(Path(str(trace) + ".meta.json").read_text())
    (output / "trace-summary.json").write_text(json.dumps(trace_summary(trace, metadata), indent=2) + "\n")
    manifest = {"generator_command": generate, "cases": [], "source_sha": subprocess.check_output(
        ["git", "rev-parse", "HEAD"], text=True).strip()}
    for case in cases:
        folder = output / case["name"]
        folder.mkdir(exist_ok=True)
        result = folder / f"{args.workload}-seed{args.seed}.csv"
        command = [str(bins / "clache_sim"), "--trace", str(trace),
                   "--policies", case["policies"], "--capacity-fractions", args.capacities,
                   "--warmup-fraction", "0.2", "--jobs", str(args.jobs), "--out", str(result),
                   "--timeseries", str(folder / "timeseries.csv")]
        for override in case.get("overrides", []):
            command.extend(["--set", override])
        command.extend(case.get("extra", []))
        print(f"Running {args.workload} seed {args.seed}: {case['name']}", flush=True)
        subprocess.run(command, check=True)
        rows = list(csv.DictReader(result.open()))
        expected = {(policy, float(capacity)) for policy in case["policies"].split(",")
                    for capacity in args.capacities.split(",")}
        actual = {(row["policy"], float(row["capacity_fraction"])) for row in rows}
        if actual != expected or len(rows) != len(expected):
            raise ValueError(f"Incomplete case: {case['name']}")
        for row in rows:
            if row["trace_fingerprint"] != str(metadata["fingerprint"]) or row["git_sha"] != metadata["git_sha"]:
                raise ValueError("Trace/build provenance mismatch")
            if int(row["requests"]) + int(row["warmup_requests"]) != args.requests:
                raise ValueError("Incomplete replay")
        manifest["cases"].append({**case, "command": command})
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    trace.unlink()  # Preserve small provenance files, not regenerable trace binaries.


if __name__ == "__main__":
    main()
