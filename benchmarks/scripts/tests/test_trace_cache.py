import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('validate_trace_cache', SCRIPT / 'validate_trace_cache.py')
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class TraceCacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.trace = self.root / 'zipf-seed1.ctrace'
        self.trace.write_bytes(b'preserve existing trace')
        self.sidecar = Path(str(self.trace) + '.meta.json')
        self.meta = dict(workload='zipf', seed=1, request_count=100,
                         params=dict(seed='1', requests='100', keys='20'),
                         git_sha='old-source-sha', fingerprint=42)
        self.save()

    def save(self):
        self.sidecar.write_text(json.dumps(self.meta))

    def validate(self):
        cache.validate(self.trace, 'zipf', '1', '100', '20')

    def test_matching_identity_accepts_different_source_sha(self):
        self.validate()
        self.meta['git_sha'] = 'another-sha'
        self.save()
        self.validate()

    def test_each_mismatch_is_named_and_data_preserved(self):
        for field, bad in [('workload', 'scan'), ('seed', 2), ('request_count', 101),
                           ('params.seed', '2'), ('params.requests', '101'), ('params.keys', '21')]:
            with self.subTest(field=field):
                original = json.loads(json.dumps(self.meta))
                if field.startswith('params.'):
                    self.meta['params'][field[7:]] = bad
                else:
                    self.meta[field] = bad
                self.save()
                before = (self.trace.read_bytes(), self.sidecar.read_bytes())
                with self.assertRaisesRegex(ValueError, field.replace('.', r'\.')):
                    self.validate()
                self.assertEqual(before, (self.trace.read_bytes(), self.sidecar.read_bytes()))
                self.meta = original
                self.save()

    def test_missing_malformed_and_wrong_types_fail(self):
        self.sidecar.unlink()
        with self.assertRaisesRegex(ValueError, 'sidecar'):
            self.validate()
        for text in ['{', '[]', '{}', '{"params":[]}', '{"params":{},"workload":"zipf","seed":true}']:
            self.sidecar.write_text(text)
            with self.assertRaises(ValueError):
                self.validate()
        for invalid in [True, -1, 1.5, '1e2', '', 1 << 64]:
            with self.assertRaises(ValueError):
                cache.unsigned(invalid, 'field')

    def matrix(self, workload='zipf', dry=False):
        binary = self.root / 'bin'
        binary.mkdir(exist_ok=True)
        # Deliberately fail after logging simulator invocation; no report or heavy work.
        for name, event in [('clache_tracegen', 'generated'), ('clache_sim', 'simulated')]:
            path = binary / name
            path.write_text('#!/bin/sh\nprintf "%s\\n" ' + event + ' >> "$EVENTS"\nexit 7\n')
            path.chmod(0o755)
        env = dict(os.environ, MATRIX_CONFIG=str(SCRIPT.parent / 'configs/matrix.env'),
                   BIN_DIR=str(binary), TRACE_DIR=str(self.root), RESULT_DIR=str(self.root / 'results'),
                   WORKLOADS=workload, SEEDS='1', REQUESTS='100', KEYS='20', VARIABLE_SIZE_KEYS='7',
                   CAPACITY_FRACTIONS='0.1', WARMUP_FRACTION='0.2', POLICIES='lru', JOBS='1',
                   DRY_RUN='1' if dry else '0', EVENTS=str(self.root / 'events'))
        return subprocess.run(['bash', str(SCRIPT / 'run_matrix.sh')], env=env,
                              capture_output=True, text=True)

    def events(self):
        path = self.root / 'events'
        return path.read_text().splitlines() if path.exists() else []

    def test_matrix_reuses_valid_cache_without_generator(self):
        result = self.matrix()
        self.assertEqual(self.events(), ['simulated'])
        self.assertIn('simulation failed', result.stderr)

    def test_matrix_stops_before_simulator_on_incompatible_cache(self):
        self.meta['request_count'] = 99
        self.save()
        before = (self.trace.read_bytes(), self.sidecar.read_bytes())
        result = self.matrix()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('request_count mismatch', result.stderr)
        self.assertIn('different TRACE_DIR', result.stderr)
        self.assertEqual(self.events(), [])
        self.assertEqual(before, (self.trace.read_bytes(), self.sidecar.read_bytes()))

    def test_matrix_missing_sidecar_and_orphan_are_preserved(self):
        self.sidecar.unlink()
        self.assertIn('sidecar', self.matrix().stderr)
        self.assertEqual(self.events(), [])
        self.save()
        before = self.sidecar.read_bytes()
        self.trace.unlink()
        self.assertIn('orphan sidecar', self.matrix().stderr)
        self.assertEqual(self.events(), [])
        self.assertEqual(self.sidecar.read_bytes(), before)
        self.assertFalse(self.trace.exists())

    def test_variable_size_uses_its_own_key_count(self):
        new_trace = self.root / 'variable_size-seed1.ctrace'
        self.trace.rename(new_trace)
        self.sidecar.unlink()
        self.trace = new_trace
        self.sidecar = Path(str(new_trace) + '.meta.json')
        self.meta['workload'] = 'variable_size'
        self.meta['params']['keys'] = '7'
        self.save()
        self.matrix('variable_size')
        self.assertEqual(self.events(), ['simulated'])
        (self.root / 'events').unlink()
        self.meta['params']['keys'] = '20'
        self.save()
        result = self.matrix('variable_size')
        self.assertIn('params.keys mismatch', result.stderr)
        self.assertEqual(self.events(), [])

    def test_dry_run_prints_validation_without_reading_metadata(self):
        self.sidecar.write_text('malformed')
        result = self.matrix(dry=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('validate_trace_cache.py', result.stdout)
        self.assertEqual(self.events(), [])
        self.assertFalse((self.root / 'results').exists())


if __name__ == '__main__':
    unittest.main()
