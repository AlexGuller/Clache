"""Run explicit, separately labelled policy experiments on one shared trace."""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess


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
    keys = args.keys or (200000 if args.workload == "variable_size" else 1000000)
    bins = Path(args.bin_dir)
    generate = [str(bins / "clache_tracegen"), "--workload", args.workload,
                "--seed", str(args.seed), "--requests", str(args.requests),
                "--keys", str(keys), "--format", "bin", "--out", str(trace)]
    subprocess.run(generate, check=True)
    shutil.copyfile(str(trace) + ".meta.json", output / (trace.name + ".meta.json"))
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
        if len(rows) != len(case["policies"].split(",")) * len(args.capacities.split(",")):
            raise ValueError(f"Incomplete case: {case['name']}")
        manifest["cases"].append({**case, "command": command})
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    trace.unlink()  # Preserve small provenance files, not regenerable trace binaries.


if __name__ == "__main__":
    main()
