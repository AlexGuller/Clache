"""Compare labelled diagnostic cases without merging different configurations."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def summarize(roots):
    groups = {}
    provenance = {}
    for root in roots:
        for manifest_path in Path(root).glob('*/manifest.json'):
            folder = manifest_path.parent
            manifest = json.loads(manifest_path.read_text())
            sidecars = list(folder.glob('*.ctrace.meta.json'))
            if len(sidecars) != 1:
                raise ValueError(f'Expected one trace sidecar: {folder}')
            meta = json.loads(sidecars[0].read_text())
            for case in manifest['cases']:
                for path in (folder / case['name']).glob('*-seed*.csv'):
                    for row in csv.DictReader(path.open()):
                        if row['trace_fingerprint'] != str(meta['fingerprint']):
                            raise ValueError(f'Trace fingerprint mismatch: {path}')
                        variant = row['policy'] if case['name'] == 'baseline' else case['name']
                        if len(case['policies'].split(',')) > 1 and case['name'] != 'baseline':
                            variant += ':' + row['policy']
                        key = (meta['workload'], float(row['capacity_fraction']), meta['seed'])
                        if key in groups.setdefault(variant, {}):
                            raise ValueError(f'Duplicate result: {variant}, {key}')
                        groups[variant][key] = row
                        provenance.setdefault(variant, set()).add(row['git_sha'])
    if 'clache' not in groups:
        raise ValueError('Missing default Clache reference')
    reference = groups['clache']
    summary = []
    details = []
    for variant, cells in sorted(groups.items()):
        if set(cells) != set(reference):
            raise ValueError(f'Unmatched experiment grid: {variant}')
        ratios = []
        for key, row in sorted(cells.items()):
            for field in ('trace_fingerprint', 'capacity_bytes', 'requests', 'warmup_requests', 'seed'):
                if row[field] != reference[key][field]:
                    raise ValueError(f'Unmatched {field}: {variant}, {key}')
            base = float(reference[key]['total_miss_cost_us'])
            cost = float(row['total_miss_cost_us'])
            if base <= 0 or cost <= 0:
                raise ValueError('Geometric comparison requires positive costs')
            ratio = cost / base
            ratios.append(ratio)
            gdsf = float(groups['gdsf'][key]['total_miss_cost_us'])
            details.append({'variant': variant, 'workload': key[0], 'capacity_fraction': key[1],
                            'trace_seed': key[2], 'cost_us': cost, 'ratio_to_default': ratio,
                            'ratio_to_gdsf': cost / gdsf})
        summary.append({'variant': variant, 'cells': len(cells),
                        'geomean_ratio_to_default': math.exp(statistics.mean(map(math.log, ratios))),
                        'worst_ratio_to_default': max(ratios),
                        'passes_10pct_cell_guard': max(ratios) <= 1.10,
                        'source_shas': sorted(provenance[variant])})
    return {'summary': sorted(summary, key=lambda r: r['geomean_ratio_to_default']), 'details': details}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('roots', nargs='+')
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    report = summarize(args.roots)
    Path(args.out).write_text(json.dumps(report, indent=2) + '\n')
    for row in report['summary']:
        print(f"{row['variant']:28s} mean={row['geomean_ratio_to_default']:.4f} "
              f"worst={row['worst_ratio_to_default']:.4f} guard={row['passes_10pct_cell_guard']}")


if __name__ == '__main__':
    main()
