"""Validate the frozen full matrix and prepare separately labelled reports."""
import argparse
import csv
import itertools
import json
from pathlib import Path
import subprocess
import sys

WORKLOADS = 'zipf cost_inversion variable_size scan hotset_shift burst regional_split mixed'.split()
BASELINES = 'lru lfu lfu_da gds gdsf gdsf_window wtinylfu s3fifo redis_lru redis_lfu clache clache_decay clache_noghost clache_nocost clache_nosize clache_noadmit'.split()
FRACTIONS = (.001, .005, .01, .02, .05, .1, .2)


def assemble(source, output):
    expected = set(itertools.product(WORKLOADS, range(1, 6), FRACTIONS, BASELINES + ['clache_tuned']))
    seen, revisions = set(), set()
    prepared = {}
    for folder in sorted(source.glob('validation-*')):
        metadata_paths = list(folder.glob('*.ctrace.meta.json'))
        if len(metadata_paths) != 1:
            raise ValueError(f'Expected one trace sidecar: {folder}')
        meta = json.loads(metadata_paths[0].read_text())
        workload, seed = meta['workload'], meta['seed']
        if meta['request_count'] != 10000000:
            raise ValueError('Wrong trace length')
        if int(meta['params']['keys']) != (200000 if workload == 'variable_size' else 1000000):
            raise ValueError('Wrong key catalogue')
        rows = []
        for case in ('baseline', 'selected'):
            path = folder / case / f'{workload}-seed{seed}.csv'
            for raw in csv.DictReader(path.open()):
                row = dict(raw)
                if case == 'selected':
                    if row['policy'] != 'clache_decay':
                        raise ValueError('Wrong selected engine policy')
                    row['policy'] = 'clache_tuned'
                key = (workload, seed, float(row['capacity_fraction']), row['policy'])
                if key not in expected or key in seen:
                    raise ValueError(f'Unexpected/duplicate result: {key}')
                if row['trace_fingerprint'] != str(meta['fingerprint']) or row['git_sha'] != meta['git_sha']:
                    raise ValueError('Trace/build provenance mismatch')
                if int(row['requests']) != 8000000 or int(row['warmup_requests']) != 2000000:
                    raise ValueError('Incomplete replay')
                if int(row['hits']) + int(row['misses']) != 8000000:
                    raise ValueError('Hit/miss arithmetic mismatch')
                # These generated traces are all GETs. Correct the known timing
                # denominator defect in derived reports; leave raw CSVs intact.
                row['ns_per_request'] = format(float(row['wall_seconds']) * 1e9 / 10000000, '.17g')
                rows.append(row)
                seen.add(key)
                revisions.add(row['git_sha'])
        prepared[workload, seed] = rows
    if seen != expected or len(revisions) != 1:
        raise ValueError(f'Incomplete matrix: {len(seen)}/4760 rows; source SHAs {revisions}')
    output.mkdir(parents=True, exist_ok=True)
    for label, seeds in [('all-five', range(1, 6)), ('held-out', range(3, 6))]:
        inputs = output / label / 'inputs'
        inputs.mkdir(parents=True, exist_ok=True)
        paths = []
        for (workload, seed), rows in sorted(prepared.items()):
            if seed not in seeds:
                continue
            path = inputs / f'{workload}-seed{seed}.csv'
            with path.open('w') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
                writer.writeheader()
                writer.writerows(rows)
            paths.append(str(path))
        subprocess.run([sys.executable, str(Path(__file__).with_name('report.py')), *paths,
                        '--out', str(output / label / 'report')], check=True)
    note = {
        'rows': len(seen), 'source_sha': next(iter(revisions)),
        'selected_label': 'clache_tuned', 'engine_policy': 'clache_decay',
        'overrides': ['clache.frequency.decay_tau_seconds=900'],
        'discovery_seeds': [1], 'previously_seen_seeds': [2], 'held_out_seeds': [3, 4, 5],
        'timing_correction': 'Derived ns_per_request divides whole-replay wall time by all 10M records; raw CSVs unchanged.',
        'limitations': ['Shared-runner timings are not controlled microbenchmarks.',
                        'Equal payload capacity does not enforce equal total RAM.',
                        'Candidate tuned on discovery data; baselines use defaults.'],
    }
    (output / 'provenance.json').write_text(json.dumps(note, indent=2) + '\n')
    print('PASS: 4760 replays; all-five and held-out reports generated')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    assemble(args.source, args.out)
