"""Cloud report validation must distinguish trace seeds from policy RNG seeds."""
import csv
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "benchmarks/scripts/check_cloud_results.py"


class CloudResultsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for workload, seed in itertools.product(
            "zipf cost_inversion variable_size scan hotset_shift burst regional_split mixed".split(),
            (1, 2),
        ):
            folder = self.root / f"results-{workload}-{seed}"
            folder.mkdir()
            stem = f"{workload}-seed{seed}"
            (folder / f"{stem}.ctrace.meta.json").write_text(json.dumps({
                "workload": workload, "seed": seed, "fingerprint": 123,
                "request_count": 10000000,
            }))
            with (folder / f"{stem}.csv").open("w") as stream:
                writer = csv.writer(stream)
                writer.writerow([
                    "trace_path", "trace_fingerprint", "seed", "capacity_fraction",
                    "policy", "requests", "warmup_requests", "git_sha",
                ])
                for capacity, policy in itertools.product(
                    (0.01, 0.05, 0.2),
                    "clache lru lfu lfu_da gdsf gdsf_window wtinylfu s3fifo redis_lru redis_lfu".split(),
                ):
                    writer.writerow([
                        f"/remote/traces/{stem}.ctrace", 123, 0, capacity,
                        policy, 8000000, 2000000, "source-revision",
                    ])

    def check(self):
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(self.root)], capture_output=True, text=True,
        )

    def test_distinct_trace_seeds_with_same_policy_seed(self):
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_sidecar_fingerprint_rejected(self):
        path = next(self.root.glob("results-*/*.meta.json"))
        meta = json.loads(path.read_text())
        meta["fingerprint"] = 456
        path.write_text(json.dumps(meta))
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fingerprint mismatch", result.stderr)

    def test_missing_shard_rejected(self):
        next(self.root.glob("results-*/*.csv")).unlink()
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Incomplete matrix", result.stderr)


if __name__ == "__main__":
    unittest.main()
