#!/usr/bin/env python3
"""CLI integration checks; run with path to the built clache_sim."""
import csv
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()
HEADER = 'timestamp_ns,key_id,size_bytes,miss_cost_us,ttl_s,region,op\n'
RESULT_COLUMNS = 'clache_version,git_sha,trace_path,trace_fingerprint,config_fingerprint,policy,domain,capacity_bytes,capacity_fraction,working_set_bytes,seed,warmup_requests,requests,hits,misses,hit_rate,byte_hit_rate,total_miss_cost_us,cost_hit_rate,admissions,rejections,evictions,expirations,invalidations,size_changes,mean_victims,max_victims,metadata_bytes_peak,metadata_bytes_per_resident,ns_per_request,wall_seconds'.split(',')

class SimulatorCLI(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.trace = self.root / 'trace.csv'
        self.trace.write_text(HEADER + '0,1,1,10,0,1,get\n1,1,1,100,0,2,get\n2,1,1,20,0,1,get\n3,2,1,200,0,2,get\n')

    def tearDown(self):
        self.tmp.cleanup()

    def invoke(self, *args, ok=True):
        p = subprocess.run([str(BINARY), '--trace', str(self.trace), *map(str, args)], capture_output=True, text=True, cwd=self.root)
        self.assertEqual(p.returncode == 0, ok, p.stdout + p.stderr)
        return p

    def rows(self, path):
        with path.open(newline='') as f:
            reader = csv.DictReader(f)
            self.assertEqual(reader.fieldnames, RESULT_COLUMNS)
            return list(reader)

    def test_regions_and_parallel_determinism(self):
        outputs = []
        for jobs in (1, 8):
            out = self.root / f'results-{jobs}.csv'
            dec = self.root / f'decisions-{jobs}.ndjson'
            ts = self.root / f'ts-{jobs}.csv'
            self.invoke('--capacity', '2,3', '--policies', 'lru,lfu_da,wtinylfu,s3fifo,redis_lru,redis_lfu,clache', '--domain-by', 'region', '--warmup-fraction', '0', '--jobs', jobs, '--out', out, '--decisions', dec, '--decision-sample-rate', '1', '--set', 'clache.telemetry.sample_hits=true', '--timeseries', ts, '--timeseries-every', '2')
            rows = self.rows(out)
            self.assertEqual(len(rows), 42)
            all_lru = next(r for r in rows if r['policy'] == 'lru' and r['domain'] == 'ALL')
            self.assertEqual(all_lru['requests'], '4')
            self.assertEqual(all_lru['hits'], '1')
            self.assertEqual(all_lru['total_miss_cost_us'], '310')
            self.assertTrue(Path(str(ts) + '.region-1.csv').exists())
            self.assertTrue(Path(str(ts) + '.region-2.csv').exists())
            for row in rows:
                row.pop('wall_seconds'); row.pop('ns_per_request')
            records = [json.loads(line) for line in dec.read_text().splitlines()]
            self.assertTrue(records)
            outputs.append((rows, records, ts.read_bytes()))
        self.assertEqual(outputs[0], outputs[1])

    def test_stats_and_configuration_precedence(self):
        out = self.root / 'stats.json'
        response = self.invoke('--stats-only', '--out', out)
        stats = json.loads(out.read_text())
        self.assertEqual(stats, json.loads(response.stdout))
        self.assertEqual(stats['working_set_bytes'], 2)
        self.assertEqual(stats['total_potential_cost_us'], 330)
        config = self.root / 'settings.yaml'
        config.write_text('clache:\n  seed: 5\n  capacity_bytes: 2\n')
        result = self.root / 'results.csv'
        self.invoke('--set', 'clache.seed=7', '--config', config, '--policies', 'lru', '--warmup-fraction', '0', '--out', result)
        self.assertEqual(self.rows(result)[0]['seed'], '7')
        self.assertEqual(json.loads(Path(str(result) + '.stats.json').read_text()), stats)

    def test_export_packed_layout_and_next_access(self):
        self.trace.write_text(HEADER + '1500000000,123,10,1,0,0,get\n2000000000,456,20,1,0,0,get\n3000000000,123,30,1,0,0,get\n4000000000,123,30,1,0,0,get\n')
        out = self.root / 'oracle.bin'
        self.invoke('--export', 'libcachesim', '--out', out)
        data = out.read_bytes()
        self.assertEqual(len(data), 96)
        self.assertEqual(list(struct.iter_unpack('<IQIq', data)), [(1,123,10,3),(2,456,20,-1),(3,123,30,4),(4,123,30,-1)])
        self.trace.write_text(HEADER + '0,1,1,0,1,0,get\n')
        self.invoke('--export', 'libcachesim', '--out', out, ok=False)
        self.assertEqual(out.read_bytes(), data)
        self.trace.write_text(HEADER + '0,1,0,0,0,0,del\n')
        self.invoke('--export', 'libcachesim', '--out', out, ok=False)

    def test_rejects_invalid_options_and_preserves_input(self):
        for args in [('--capacity','0'), ('--jobs','0'), ('--warmup-fraction','nan'), ('--capacity','2','--policies','no-such-policy'), ('--set','policy.redis.samples=0','--capacity','2'), ('--capacity','2','--per-entry-overhead','18446744073709551615'), ('--capacity-fractions','0.001'), ('--capacity','2','--out',self.trace)]:
            original = self.trace.read_bytes()
            self.invoke(*args, ok=False)
            self.assertEqual(self.trace.read_bytes(), original)

    def test_requested_fraction_survives_capacity_rounding(self):
        fractions = []
        for size in (10, 11):
            self.trace.write_text(HEADER + f'0,1,{size},1,0,0,get\n')
            out = self.root / f'fraction-{size}.csv'
            self.invoke('--policies', 'lru', '--capacity-fractions', '0.3', '--out', out)
            r = self.rows(out)[0]
            self.assertEqual(r['capacity_bytes'], '3')
            fractions.append(r['capacity_fraction'])
        self.assertEqual(fractions[0], fractions[1])
        self.assertEqual(float(fractions[0]), 0.3)

    def test_warmup_and_ttl_counter_deltas(self):
        self.trace.write_text(HEADER + '0,1,1,100,1,0,get\n1000000000,1,1,200,0,0,get\n1000000001,1,2,300,0,0,get\n1000000002,1,2,400,0,0,get\n')
        out = self.root / 'results.csv'
        self.invoke('--policies','lru','--capacity','3','--per-entry-overhead','1','--warmup-fraction','0.25','--out',out)
        r = self.rows(out)[0]
        self.assertEqual(r['warmup_requests'],'1')
        self.assertEqual(r['requests'],'3')
        self.assertEqual(r['hits'],'1')
        self.assertEqual(r['expirations'],'1')
        self.assertEqual(r['size_changes'],'1')
        self.assertEqual(r['total_miss_cost_us'],'500')

if __name__ == '__main__':
    unittest.main()
