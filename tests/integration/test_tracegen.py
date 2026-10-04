#!/usr/bin/env python3
"""Run with the path to a built clache_tracegen."""
import csv
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
WORKLOADS = ['zipf', 'cost_inversion', 'variable_size', 'scan', 'hotset_shift',
             'burst', 'regional_split', 'mixed']


class TracegenCli(unittest.TestCase):
    def run_cli(self, *args, ok=True):
        result = subprocess.run([BINARY, *map(str, args)], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        return result

    def test_all_formats_and_sidecars(self):
        with tempfile.TemporaryDirectory() as directory:
            for workload in WORKLOADS:
                outputs = []
                for fmt in ['csv', 'bin']:
                    path = Path(directory) / (workload + '.' + fmt)
                    args = ['--workload', workload, '--keys', '32', '--requests', '256',
                            '--seed', '7', '--format', fmt, '--out', path]
                    self.run_cli(*args)
                    before = path.read_bytes()
                    sidecar = Path(str(path) + '.meta.json')
                    meta_before = sidecar.read_bytes()
                    self.run_cli(*args)
                    self.assertEqual(path.read_bytes(), before)
                    self.assertEqual(sidecar.read_bytes(), meta_before)
                    meta = json.loads(meta_before)
                    self.assertEqual(meta['request_count'], 256)
                    self.assertEqual(meta['seed'], 7)
                    self.assertEqual(meta['workload'], workload)
                    self.assertTrue(meta['git_sha'])
                    if fmt == 'csv':
                        with path.open() as stream:
                            rows = [[int(row[k]) for k in ['timestamp_ns', 'key_id', 'size_bytes',
                                                          'miss_cost_us', 'ttl_s']] +
                                    [int(row.get('region', 0)), 0]
                                    for row in csv.DictReader(stream)]
                    else:
                        self.assertEqual(before[:8], b'CLACHETR')
                        self.assertEqual(struct.unpack_from('<IIQ', before, 8), (1, 32, 256))
                        self.assertEqual(len(before), 64 + 256 * 32)
                        rows = [list(struct.unpack_from('<QQIIIHBB', before, offset))[:-1]
                                for offset in range(64, len(before), 32)]
                    sizes = {}
                    for row in rows:
                        sizes[row[1]] = max(sizes.get(row[1], 0), row[2])
                    self.assertEqual(meta['unique_keys'], len(sizes))
                    self.assertEqual(meta['working_set_bytes'], sum(sizes.values()))
                    outputs.append(rows)
                self.assertEqual(*outputs)

    def test_list_errors_and_precedence(self):
        listed = self.run_cli('--list').stdout
        for name in WORKLOADS:
            self.assertIn(name + '\n', listed)
        self.assertIn('scan_len=20000', listed)
        self.assertIn('requests=10000000', listed)
        for args in [[], ['--format', 'bad'], ['--requests'], ['--bogus'], ['--param', 'x']]:
            self.run_cli(*args, ok=False)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'output.csv'
            for args in [['--workload', 'bad'], ['--param', 'unknown=1'], ['--keys', '0'],
                         ['--param', 'size.max=0'], ['--param', 'rate_rps=nan']]:
                self.run_cli('--out', path, *args, ok=False)
                self.assertFalse(path.exists())
            self.run_cli('--out', path, '--requests', '4', '--param', 'requests=0', '--keys', '1')
            self.assertEqual(len(path.read_text().splitlines()), 1)
            meta = json.loads(Path(str(path) + '.meta.json').read_text())
            self.assertEqual(meta['unique_keys'], 0)
            self.assertEqual(meta['working_set_bytes'], 0)


if __name__ == '__main__':
    unittest.main()
