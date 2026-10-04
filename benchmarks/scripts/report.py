#!/usr/bin/env python3
"""Deterministic experiment summaries; Python standard library, optional plots."""
import argparse
import csv
import json
import math
from pathlib import Path
import re
import statistics
import sys
from collections import defaultdict

COLUMNS = """clache_version git_sha trace_path trace_fingerprint config_fingerprint policy domain
capacity_bytes capacity_fraction working_set_bytes seed warmup_requests requests hits misses
hit_rate byte_hit_rate total_miss_cost_us cost_hit_rate admissions rejections evictions expirations
invalidations size_changes mean_victims max_victims metadata_bytes_peak metadata_bytes_per_resident
ns_per_request wall_seconds""".split()
BASELINES = ('lru', 'lfu', 'lfu_da')
MODERN = ('gdsf', 'gdsf_window', 'wtinylfu', 's3fifo')
WORKLOADS = ('cost_inversion', 'variable_size', 'mixed', 'regional_split', 'scan')
METRICS = ('total_miss_cost_us', 'cost_hit_rate', 'hit_rate', 'byte_hit_rate',
           'metadata_bytes_per_resident', 'ns_per_request')


def trace_identity(row, source):
    trace = Path(row['trace_path'])
    candidates = [trace, source.parent / trace]
    for path in candidates:
        sidecar = Path(str(path) + '.meta.json')
        if sidecar.is_file():
            data = json.loads(sidecar.read_text())
            if not isinstance(data.get('workload'), str) or not isinstance(data.get('seed'), int):
                raise ValueError(f'{sidecar}: workload and integer seed required')
            return data['workload'], data['seed']
    match = re.fullmatch(r'(.+)-seed(\d+)\.[^.]+', trace.name)
    if not match:
        raise ValueError(f'{source}: missing trace sidecar and unrecognised trace filename: {trace}')
    return match[1], int(match[2])


def load_results(paths):
    rows, seen, experiments = [], set(), {}
    for path in sorted(map(Path, paths)):
        with path.open(newline='') as stream:
            reader = csv.DictReader(stream)
            missing = sorted(set(COLUMNS) - set(reader.fieldnames or []))
            if missing:
                raise ValueError(f'{path}: missing columns: {", ".join(missing)}')
            for line, row in enumerate(reader, 2):
                try:
                    if None in row or any(v is None for v in row.values()):
                        raise ValueError('wrong number of fields')
                    row['workload'], row['trace_seed'] = trace_identity(row, path)
                    for name in (*METRICS, 'capacity_fraction'):
                        row[name] = float(row[name])
                        if not math.isfinite(row[name]) or row[name] < 0:
                            raise ValueError(f'{name} must be finite and nonnegative')
                    if row['capacity_fraction'] <= 0:
                        raise ValueError('capacity_fraction must be positive')
                    for name in ('hit_rate', 'byte_hit_rate', 'cost_hit_rate'):
                        if row[name] > 1:
                            raise ValueError(f'{name} must be <= 1')
                    key = (row['workload'], row['domain'], row['capacity_fraction'],
                           row['policy'], row['trace_seed'])
                    if key in seen:
                        raise ValueError(f'duplicate experiment {key}; report config variants separately')
                    # Policy scores are comparable only for the same measured trace
                    # slice, byte budget, RNG seed and build. Effective config hashes
                    # legitimately differ for named ablations, so are not compared.
                    identity_fields = ('trace_fingerprint', 'clache_version', 'git_sha',
                                       'capacity_bytes', 'working_set_bytes', 'seed',
                                       'warmup_requests', 'requests')
                    integer_fields = {'capacity_bytes', 'working_set_bytes', 'seed',
                                      'warmup_requests', 'requests'}
                    identity = {}
                    for field in identity_fields:
                        value = int(row[field]) if field in integer_fields else row[field]
                        if field in integer_fields and value < 0:
                            raise ValueError(f'{field} must be nonnegative')
                        identity[field] = value
                    experiment = (row['workload'], row['domain'], row['capacity_fraction'],
                                  row['trace_seed'])
                    previous = experiments.get(experiment)
                    if previous is not None:
                        different = [field for field in identity_fields
                                     if previous[field] != identity[field]]
                        if different:
                            raise ValueError('incompatible experiments: ' + ', '.join(different)
                                             + f' differ for {experiment}')
                    experiments[experiment] = identity
                    seen.add(key)
                    rows.append(row)
                except (ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
                    raise ValueError(f'{path}:{line}: {exc}') from exc
    if not rows:
        raise ValueError('no experiment rows')
    return rows


def ratio(value, base):
    return value / base if base else (1.0 if value == 0 else None)


def summarize(rows):
    groups = defaultdict(lambda: defaultdict(dict))
    for row in rows:
        groups[row['workload'], row['domain'], row['capacity_fraction']][row['policy']][row['trace_seed']] = row
    summaries = []
    for key, policies in sorted(groups.items()):
        for policy, seeds in sorted(policies.items()):
            normalized = []
            for seed, row in sorted(seeds.items()):
                baseline = [policies[p][seed]['total_miss_cost_us'] for p in BASELINES
                            if p in policies and seed in policies[p]]
                value = ratio(row['total_miss_cost_us'], min(baseline)) if baseline else None
                if value is not None:
                    normalized.append(value)
            result = dict(zip(('workload', 'domain', 'capacity_fraction'), key))
            result.update(policy=policy, seeds=len(seeds), normalized_seeds=len(normalized),
                          missing_baselines=','.join(p for p in BASELINES if p not in policies))
            for metric in METRICS:
                values = [r[metric] for r in seeds.values()]
                result[metric] = statistics.median(values)
                if metric == 'total_miss_cost_us':
                    result['miss_cost_min'] = min(values)
                    result['miss_cost_max'] = max(values)
            result.update(normalized_median=statistics.median(normalized) if normalized else None,
                          normalized_min=min(normalized) if normalized else None,
                          normalized_max=max(normalized) if normalized else None)
            summaries.append(result)
    return summaries, groups


def evidence(groups):
    variants = sorted({p for policies in groups.values() for p in policies if p.startswith('clache')})
    domains = sorted({key[1] for key in groups})
    results, comparisons = [], []
    for variant in variants:
        for domain in domains:
            points = defaultdict(list)
            for (workload, dom, capacity), policies in sorted(groups.items()):
                if dom != domain or variant not in policies:
                    continue
                for baseline_set, label in [(BASELINES, 'classic'), *(([p], p) for p in MODERN)]:
                    available = [p for p in baseline_set if p in policies]
                    seeds = set(policies[variant])
                    for p in available:
                        seeds &= set(policies[p])
                    if not available or not seeds:
                        continue
                    candidate = statistics.median(policies[variant][s]['total_miss_cost_us'] for s in sorted(seeds))
                    baseline = min(statistics.median(policies[p][s]['total_miss_cost_us'] for s in sorted(seeds)) for p in available)
                    relative = ratio(candidate, baseline)
                    win = baseline > 0 and candidate <= baseline * 0.9
                    comparisons.append((variant, domain, workload, capacity, label, relative, len(seeds), ','.join(available)))
                    if label == 'classic' and workload in WORKLOADS:
                        points[workload].append(win)
            qualifying = sum(sum(points[w]) >= 3 for w in WORKLOADS)
            complete = all(len(points[w]) >= 3 for w in WORKLOADS)
            verdict = 'PASS' if qualifying >= 3 else ('FAIL' if complete else 'INSUFFICIENT EVIDENCE')
            results.append((variant, domain, verdict, points))
    return results, comparisons


def fmt(value):
    if value is None:
        return 'NA'
    return f'{value:.6f}' if isinstance(value, float) else str(value)


def cell(value):
    return fmt(value).replace('|', '\\|').replace('\n', ' ')


def write_report(rows, output, plots=True, timeseries=None):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    summary, groups = summarize(rows)
    verdicts, comparisons = evidence(groups)
    with (output / 'summary.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows({k: fmt(v) for k, v in row.items()} for row in summary)
    lines = ['# Clache experiment report', '',
             'Normalized costs use the best available classic baseline for each matched trace seed; '
             'the table reports their median and range. Raw costs and other metrics are seed medians.', '',
             'Go/no-go compares median raw costs on common seeds against the best baseline median. '
             'A single variant must win at three capacities on three workloads within one domain. '
             'Zero baseline cost cannot establish a 10% improvement. Domain rows are never pooled.', '',
             '| Workload | Domain | Capacity | Policy | Seeds / normalized | Miss cost µs (min–max) | Normalized (min–max) | Cost hit | Hit | Byte hit | Metadata B/key | ns/request |',
             '|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|']
    for r in summary:
        values = [r['workload'], r['domain'], r['capacity_fraction'], r['policy'],
                  f"{r['seeds']} / {r['normalized_seeds']}",
                  f"{fmt(r['total_miss_cost_us'])} ({fmt(r['miss_cost_min'])}–{fmt(r['miss_cost_max'])})",
                  f"{fmt(r['normalized_median'])} ({fmt(r['normalized_min'])}–{fmt(r['normalized_max'])})",
                  *(r[m] for m in METRICS[1:])]
        lines.append('| ' + ' | '.join(map(cell, values)) + ' |')
    lines += ['', '## Go/no-go evidence', '', '| Variant | Domain | Verdict | Workload | Winning / measured capacities |', '|---|---|---|---|---:|']
    if not verdicts:
        lines += ['No Clache policy is present: INSUFFICIENT EVIDENCE.']
    for variant, domain, verdict, points in verdicts:
        for workload in WORKLOADS:
            lines.append(f'| {cell(variant)} | {cell(domain)} | {verdict} | {workload} | {sum(points[workload])} / {len(points[workload])} |')
    lines += ['', '## Comparisons and missing evidence', '',
              'Ratios below 1 favor Clache; ratios above 1 are Clache losses. NA denotes an undefined positive/zero ratio.', '',
              '| Variant | Domain | Workload | Capacity | Comparator | Ratio | Common seeds | Available policies |', '|---|---|---|---:|---|---:|---:|---|']
    for row in comparisons:
        lines.append('| ' + ' | '.join(map(cell, row)) + ' |')
    for (workload, domain, capacity), policies in sorted(groups.items()):
        missing = [p for p in (*BASELINES, *MODERN) if p not in policies]
        if missing:
            lines.append(f'- Missing policies at {cell(workload)}/{cell(domain)}/{fmt(capacity)}: {", ".join(missing)}.')
        sets = {tuple(sorted(seeds)) for seeds in policies.values()}
        if len(sets) > 1:
            lines.append(f'- Unmatched seeds at {cell(workload)}/{cell(domain)}/{fmt(capacity)}; comparisons use common seeds only.')
    for workload in WORKLOADS:
        if not any(k[0] == workload for k in groups):
            lines.append(f'- Missing workload: {workload}.')
    lines += ['', 'W-TinyLFU omits randomized warm-candidate admission and its doorkeeper. '
              'Simulator ns/request includes replay overhead; it is not policy-only latency.', '']
    if timeseries:
        required = set('policy capacity_bytes interval_index first_request requests misses miss_cost_us hit_rate cost_hit_rate'.split())
        with Path(timeseries).open(newline='') as stream:
            reader = csv.DictReader(stream)
            if not required.issubset(reader.fieldnames or []):
                raise ValueError('timeseries: missing required columns')
            count = sum(1 for _ in reader)
        lines += [f'Timeseries input validated: {count} intervals. Aggregate tables use results.csv.', '']
    (output / 'report.md').write_text('\n'.join(lines))
    if plots:
        plot(summary, output)
    return summary


def plot(summary, output):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print('warning: matplotlib unavailable; tables produced, plots skipped', file=sys.stderr)
        return
    for workload, domain in sorted({(r['workload'], r['domain']) for r in summary}):
        fig, ax = plt.subplots(figsize=(10, 5))
        subset = [r for r in summary if (r['workload'], r['domain']) == (workload, domain)]
        for policy in sorted({r['policy'] for r in subset}):
            points = sorted((r['capacity_fraction'], r['total_miss_cost_us']) for r in subset if r['policy'] == policy)
            ax.plot([p[0] for p in points], [p[1] for p in points], marker='o', label=policy)
        ax.set(xscale='log', xlabel='Capacity / working set', ylabel='Median total miss cost (µs)', title=f'{workload} / {domain}')
        ax.legend(fontsize='small', loc='upper left', bbox_to_anchor=(1.02, 1), borderaxespad=0)
        fig.tight_layout()
        # Hex encoding makes arbitrary domain/workload names safe and collision-free.
        filename = f'{workload.encode().hex()}-{domain.encode().hex()}.png'
        fig.savefig(output / filename)
        plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results', nargs='+', type=Path)
    parser.add_argument('--out', type=Path, default=Path('report'))
    parser.add_argument('--timeseries', type=Path)
    parser.add_argument('--no-plots', action='store_true')
    args = parser.parse_args()
    try:
        write_report(load_results(args.results), args.out, not args.no_plots, args.timeseries)
    except (OSError, ValueError) as exc:
        parser.exit(2, f'error: {exc}\n')


if __name__ == '__main__':
    main()
