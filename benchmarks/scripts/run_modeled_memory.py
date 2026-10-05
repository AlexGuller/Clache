#!/usr/bin/env python3
"""Bounded sampled-modeled-memory sensitivity; this does not enforce actual RAM."""
import argparse
import csv
import json
import math
from pathlib import Path
import subprocess

POLICIES = ['clache', 'gdsf', 'wtinylfu', 's3fifo', 'redis_lfu']
LIMITATIONS = [
    'Budget model is payload capacity plus sampled reported metadata peak, not actual RAM.',
    'Reported metadata omits resident-set allocations, free-list capacity, some scratch space and allocator overhead.',
    'Sampling can miss warm-up and transient peaks. Payload capacity is reserved allowance, not measured resident payload.',
    'Calibration is bounded exploration; allocator discontinuities and nonmonotonic metadata mean the largest observed feasible capacity need not be globally optimal.',
    'Capacity selection uses only metadata and budget slack; miss cost is reported only after selection.',
    'Verification repeats the same trace and configuration; it checks reproducibility, not out-of-sample generalization.',
]


def calibrate(budget, initial_capacity, replay, initial=None, max_replays=6):
    """Reserve one of at most six calls for verification. Never inspect cost."""
    if budget < 1 or not 2 <= max_replays <= 6:
        raise ValueError('positive budget and 2..6 replays required')
    attempts = []
    seen = set()
    capacity = min(budget, max(1, initial_capacity))
    for index in range(max_replays - 1):
        row = initial if index == 0 and initial is not None else replay(capacity, 'calibration')
        if int(row['capacity_bytes']) != capacity:
            raise ValueError('simulator returned the wrong capacity')
        metadata = int(row['metadata_bytes_peak'])
        if metadata < 0:
            raise ValueError('negative metadata')
        slack = budget - capacity - metadata
        attempts.append(dict(capacity=capacity, metadata=metadata, slack=slack, row=row))
        seen.add(capacity)
        feasible = [a for a in attempts if a['slack'] >= 0]
        best = max(feasible, key=lambda a: a['capacity']) if feasible else None
        if best and best['slack'] <= budget * .01:
            break
        # A local slack correction is a proposal, never a monotonicity assumption.
        proposal = max(1, min(budget, capacity + slack))
        if proposal in seen:
            anchors = sorted({0, budget + 1, *seen})
            gaps = [(right-left, left, right) for left, right in zip(anchors, anchors[1:])
                    if right-left > 1 and (best is None or right-1 > best['capacity'])]
            if not gaps:
                break
            _, left, right = max(gaps)
            proposal = (left + right) // 2
        capacity = proposal
    feasible = [a for a in attempts if a['slack'] >= 0]
    if not feasible:
        return dict(status='no_observed_feasible_capacity', attempts=attempts, selected=None, verification=None)
    selected = max(feasible, key=lambda a: a['capacity'])
    verification = replay(selected['capacity'], 'verification')
    if int(verification['capacity_bytes']) != selected['capacity']:
        raise ValueError('verification returned the wrong capacity')
    if int(verification['metadata_bytes_peak']) < 0:
        raise ValueError('negative verification metadata')
    slack = budget - selected['capacity'] - int(verification['metadata_bytes_peak'])
    status = 'verification_infeasible' if slack < 0 else ('verified_within_1pct' if slack <= budget*.01 else 'verified_slack_above_1pct')
    return dict(status=status, attempts=attempts, selected=selected,
                verification=dict(capacity=selected['capacity'], metadata=int(verification['metadata_bytes_peak']), slack=slack, row=verification))


class Simulator:
    def __init__(self, binary, trace, output, metadata, source_sha):
        self.binary, self.trace, self.output = binary, trace, output
        self.metadata, self.source_sha = metadata, source_sha
        self.commands = []
        self.serial = 0

    def replay(self, policy, capacity, phase, overrides=()):
        self.serial += 1
        path = self.output / f'{self.serial:03d}-{phase}-{policy}-{capacity}.csv'
        command = [str(self.binary), '--trace', str(self.trace), '--policies', policy,
                   '--capacity', str(capacity), '--warmup-fraction', '0.2', '--jobs', '1', '--out', str(path)]
        for override in overrides:
            command.extend(['--set', override])
        subprocess.run(command, check=True)
        self.commands.append(command)
        with path.open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        if len(rows) != 1:
            raise ValueError(f'{path}: expected exactly one shared-domain result')
        row = rows[0]
        expected_warmup = int(self.metadata['request_count']) // 5
        if (row['policy'] != policy or row['domain'] != 'ALL' or int(row['capacity_bytes']) != capacity
                or int(row['trace_fingerprint']) != self.metadata['fingerprint']
                or row['git_sha'] != self.source_sha
                or int(row['warmup_requests']) != expected_warmup
                or int(row['requests']) != self.metadata['request_count'] - expected_warmup):
            raise ValueError(f'{path}: result identity/count mismatch')
        row['artifact'] = str(path)
        return row


def run(args):
    if args.requests <= 0 or args.requests % 5 or args.seed < 0:
        raise ValueError('requests must be positive and divisible by five; seed nonnegative')
    fractions = [float(f) for f in args.fractions.split(',')]
    if len(set(fractions)) != len(fractions) or any(not math.isfinite(f) or not 0 < f <= 1 for f in fractions):
        raise ValueError('fractions must be unique and in (0,1]')
    output = args.out.resolve() / f'{args.workload}-seed{args.seed}'
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError(f'{output} is not empty; choose a fresh output directory')
    binaries = args.bin_dir.resolve()
    trace = output / f'{args.workload}-seed{args.seed}.ctrace'
    keys = args.keys if args.keys is not None else (200000 if args.workload == 'variable_size' else 1000000)
    if keys < 1:
        raise ValueError('keys must be positive')
    generate = [str(binaries/'clache_tracegen'), '--workload', args.workload, '--seed', str(args.seed),
                '--requests', str(args.requests), '--keys', str(keys), '--format', 'bin', '--out', str(trace)]
    subprocess.run(generate, check=True)
    metadata = json.loads(Path(str(trace)+'.meta.json').read_text())
    source_sha = args.expected_source_sha or metadata['git_sha']
    if metadata['git_sha'] != source_sha or metadata['request_count'] != args.requests or metadata['seed'] != args.seed or metadata['workload'] != args.workload:
        raise ValueError('trace sidecar does not match requested source/parameters')
    simulator = Simulator(binaries/'clache_sim', trace, output, metadata, source_sha)
    manifest = dict(workload=args.workload, seed=args.seed, source_sha=source_sha,
                    generator_command=generate, trace_metadata=metadata, limitations=LIMITATIONS,
                    max_replays_per_policy_budget=6, results=[], commands=simulator.commands)
    script_root = Path(__file__).resolve().parents[2]
    manifest['runner_source_sha'] = subprocess.check_output(['git', '-C', str(script_root), 'rev-parse', 'HEAD'], text=True).strip()
    records = []
    variants = [(p,p,[]) for p in POLICIES]
    if args.tuned_set:
        variants.append(('clache_tuned','clache',args.tuned_set))
    for fraction in fractions:
        reference_capacity = int(metadata['working_set_bytes'] * fraction)
        if reference_capacity < 1:
            raise ValueError('reference capacity rounds to zero')
        reference = simulator.replay('gdsf', reference_capacity, 'reference')
        budget = reference_capacity + int(reference['metadata_bytes_peak'])
        for label, policy, overrides in variants:
            replay = lambda capacity, phase: simulator.replay(policy, capacity, phase, overrides)
            result = calibrate(budget, reference_capacity, replay, initial=reference if label=='gdsf' else None)
            verification = result['verification']
            row = verification['row'] if verification else None
            record = dict(workload=args.workload,trace_seed=args.seed,policy=label,engine_policy=policy,
                          reference_capacity_fraction=fraction,reference_payload_bytes=reference_capacity,
                          reference_metadata_bytes=int(reference['metadata_bytes_peak']),target_budget_bytes=budget,
                          status=result['status'],payload_bytes=verification['capacity'] if verification else '',
                          metadata_bytes_peak=verification['metadata'] if verification else '',
                          modeled_total_bytes=verification['capacity']+verification['metadata'] if verification else '',
                          slack_bytes=verification['slack'] if verification else '',
                          slack_fraction=verification['slack']/budget if verification else '',
                          calibration_attempts=len(result['attempts']),verification_replays=int(verification is not None),
                          total_miss_cost_us=row['total_miss_cost_us'] if row else '',
                          hit_rate=row['hit_rate'] if row else '',cost_hit_rate=row['cost_hit_rate'] if row else '',
                          source_sha=source_sha,trace_fingerprint=metadata['fingerprint'],
                          config_fingerprint=row['config_fingerprint'] if row else '',overrides=json.dumps(overrides))
            records.append(record)
            manifest['results'].append(dict(label=label,fraction=fraction,budget=budget,overrides=overrides,**result))
            (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
            with (output/'modeled_memory.csv').open('w',newline='') as stream:
                writer=csv.DictWriter(stream,fieldnames=list(record),lineterminator='\n')
                writer.writeheader();writer.writerows(records)
            print(f'{label} fraction={fraction}: {result["status"]}',flush=True)
    lines=['# Sampled modeled-memory sensitivity','','This experiment does not enforce actual RAM.','']
    lines += ['- '+text for text in LIMITATIONS]
    lines += ['', '| Policy | Reference fraction | Target bytes | Payload bytes | Metadata bytes | Slack fraction | Miss cost µs | Status |', '|---|---:|---:|---:|---:|---:|---:|---|']
    for r in records:
        lines.append('| '+' | '.join(str(r[k]) for k in ['policy','reference_capacity_fraction','target_budget_bytes','payload_bytes','metadata_bytes_peak','slack_fraction','total_miss_cost_us','status'])+' |')
    (output/'modeled_memory.md').write_text('\n'.join(lines)+'\n')
    return any(r['status'] in ('verification_infeasible','no_observed_feasible_capacity') for r in records)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workload',required=True,choices=['cost_inversion','variable_size','mixed'])
    parser.add_argument('--seed',type=int,default=3)
    parser.add_argument('--requests',type=int,default=10000000)
    parser.add_argument('--keys',type=int)
    parser.add_argument('--fractions',default='0.05,0.2')
    parser.add_argument('--bin-dir',type=Path,default=Path('build/release/apps'))
    parser.add_argument('--out',type=Path,default=Path('build/modeled-memory'))
    parser.add_argument('--expected-source-sha')
    parser.add_argument('--tuned-set',action='append',default=[],help='Additional labeled Clache variant; repeat dotted key=value overrides')
    args=parser.parse_args()
    try:
        failed=run(args)
    except (OSError,ValueError,KeyError,subprocess.CalledProcessError) as exc:
        parser.exit(2,f'error: {exc}\n')
    if failed:
        parser.exit(1,'Some budgets could not be verified; see labeled results.\n')


if __name__=='__main__':main()
