import csv
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('report', SCRIPT / 'report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def row(workload='mixed', policy='lru', seed=1, cost=100, capacity=.01, domain='ALL'):
    r = {name: '0' for name in report.COLUMNS}
    r.update(trace_path=f'{workload}-seed{seed}.ctrace', policy=policy, domain=domain,
             capacity_fraction=capacity, total_miss_cost_us=cost, workload=workload,
             trace_seed=seed, hit_rate=.5, byte_hit_rate=.5, cost_hit_rate=.5,
             metadata_bytes_per_resident=64., ns_per_request=100.)
    return r


class ReportTests(unittest.TestCase):
    def test_normalization_and_seed_statistics(self):
        rows = [row(policy='lru', seed=1, cost=100), row(policy='lru', seed=2, cost=200),
                row(policy='clache', seed=1, cost=50), row(policy='clache', seed=2, cost=150)]
        summary, _ = report.summarize(rows)
        c = next(r for r in summary if r['policy'] == 'clache')
        self.assertEqual(c['normalized_median'], .625)
        self.assertEqual((c['normalized_min'], c['normalized_max']), (.5, .75))
        self.assertEqual(c['total_miss_cost_us'], 100)
        self.assertEqual(c['missing_baselines'], 'lfu,lfu_da')

    def matrix(self, cost=90, points=3, workloads=3):
        return [row(w, p, cost=(100 if p == 'lru' else cost), capacity=c / 100)
                for w in report.WORKLOADS[:workloads] for c in range(1, points+1)
                for p in ('lru', 'clache')]

    def verdict(self, rows):
        return report.evidence(report.summarize(rows)[1])[0][0][2]

    def test_pass_exact_boundary(self):
        self.assertEqual(self.verdict(self.matrix()), 'PASS')

    def test_missing_points_is_insufficient(self):
        self.assertEqual(self.verdict(self.matrix(points=2)), 'INSUFFICIENT EVIDENCE')
        self.assertEqual(self.verdict(self.matrix(workloads=2)), 'INSUFFICIENT EVIDENCE')

    def test_complete_failure(self):
        self.assertEqual(self.verdict(self.matrix(cost=91, workloads=5)), 'FAIL')

    def test_zero_cost_is_not_improvement(self):
        rows = self.matrix(workloads=5)
        for r in rows: r['total_miss_cost_us'] = 0
        self.assertEqual(self.verdict(rows), 'FAIL')
        self.assertEqual(report.ratio(0, 0), 1)
        self.assertIsNone(report.ratio(1, 0))

    def test_no_variant_or_domain_cherry_picking(self):
        rows = self.matrix()
        for r in rows:
            if r['policy'] == 'clache' and r['workload'] == 'mixed':
                r['policy'] = 'clache_decay'
        self.assertNotIn('PASS', [r[2] for r in report.evidence(report.summarize(rows)[1])[0]])

    def test_unmatched_seeds_cannot_win(self):
        rows = [row(policy='lru', seed=1), row(policy='clache', seed=2, cost=1)]
        self.assertEqual(report.evidence(report.summarize(rows)[1])[1], [])

    def test_sidecar_precedes_filename(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            trace = path / 'wrong-seed2.ctrace'
            Path(str(trace)+'.meta.json').write_text(json.dumps({'workload': 'mixed', 'seed': 7}))
            self.assertEqual(report.trace_identity({'trace_path': str(trace)}, path/'result.csv'), ('mixed', 7))
            self.assertEqual(report.trace_identity({'trace_path': 'scan-seed4.ctrace'}, path/'result.csv'), ('scan', 4))

    def test_schema_errors_and_duplicate_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'bad.csv'
            path.write_text('policy\nlru\n')
            with self.assertRaisesRegex(ValueError, 'missing columns'):
                report.load_results([path])
            with path.open('w', newline='') as stream:
                writer = csv.DictWriter(stream, report.COLUMNS, extrasaction='ignore')
                writer.writeheader(); writer.writerow(row()); writer.writerow(row())
            with self.assertRaisesRegex(ValueError, 'duplicate experiment'):
                report.load_results([path])

    def test_rejects_incompatible_experiment_comparisons(self):
        for field in ('trace_fingerprint', 'requests', 'warmup_requests',
                      'capacity_bytes', 'working_set_bytes', 'seed',
                      'git_sha', 'clache_version'):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'mismatched.csv'
                baseline = row(policy='lru')
                candidate = row(policy='clache', cost=1)
                candidate[field] = '99'
                with path.open('w', newline='') as stream:
                    writer = csv.DictWriter(stream, report.COLUMNS, extrasaction='ignore')
                    writer.writeheader()
                    writer.writerows([baseline, candidate])
                with self.assertRaisesRegex(ValueError, 'incompatible experiments: ' + field):
                    report.load_results([path])

    def test_allows_named_ablations_with_distinct_config_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'ablations.csv'
            baseline, candidate = row(policy='lru'), row(policy='clache_decay')
            candidate['config_fingerprint'] = '99'
            with path.open('w', newline='') as stream:
                writer = csv.DictWriter(stream, report.COLUMNS, extrasaction='ignore')
                writer.writeheader()
                writer.writerows([baseline, candidate])
            self.assertEqual(len(report.load_results([path])), 2)

    def test_deterministic_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = Path(directory)/'a', Path(directory)/'b'
            rows = self.matrix()
            report.write_report(rows, a, plots=False)
            report.write_report(list(reversed(rows)), b, plots=False)
            for filename in ('report.md', 'summary.csv'):
                self.assertEqual((a/filename).read_bytes(), (b/filename).read_bytes())

    def test_fixture(self):
        rows = report.load_results([SCRIPT/'tests/fixtures/results.csv'])
        self.assertEqual(len(rows), 4)

    def test_matrix_dry_run_has_no_filesystem_effects(self):
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, DRY_RUN='1', WORKLOADS='zipf variable_size', SEEDS='1',
                       TRACE_DIR=directory+'/traces', RESULT_DIR=directory+'/results', BIN_DIR='/absent', JOBS='2')
            run = subprocess.run(['bash', str(SCRIPT/'run_matrix.sh')], env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('--keys 200000', run.stdout)
            self.assertIn('--capacity-fractions', run.stdout)
            self.assertIn('report.py', run.stdout)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_matrix_missing_binary_is_clear(self):
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, DRY_RUN='0', BIN_DIR=directory)
            run = subprocess.run(['bash', str(SCRIPT/'run_matrix.sh')], env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 2)
            self.assertIn('missing executable', run.stderr)

    def test_matrix_continues_after_generation_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            gen = path/'clache_tracegen'
            gen.write_text('#!/usr/bin/env bash\necho "$*" >> "'+str(path/'calls')+'"\nexit 1\n')
            sim = path/'clache_sim'
            sim.write_text('#!/usr/bin/env bash\nexit 1\n')
            gen.chmod(0o755); sim.chmod(0o755)
            env = dict(os.environ, DRY_RUN='0', BIN_DIR=directory, WORKLOADS='zipf scan', SEEDS='1',
                       TRACE_DIR=directory+'/traces', RESULT_DIR=directory+'/results')
            run = subprocess.run(['bash', str(SCRIPT/'run_matrix.sh')], env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(len((path/'calls').read_text().splitlines()), 2)
            self.assertTrue((path/'results/env.json').exists())


if __name__ == '__main__':
    unittest.main()
