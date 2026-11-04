#!/usr/bin/env python3
"""Summarize closed AN cells; three repeats are required for comparison.

Writes compact JSON to stdout. It never changes captures or promotes failed,
missing, diagnostic or counterless cells into an accepted measurement.
"""
import argparse
import itertools
import json
from pathlib import Path
import re
import statistics


def observation_max(root, doc, node):
    before = doc['before'][node]['t_us']
    after = doc['after'][node]['t_us']
    active = False
    values = {}
    with (root / f'raft-observation-n{node}.log').open() as source:
        for line in source:
            match = re.search(r'an-proof: t_us=(\d+)', line)
            if match:
                active = before < int(match[1]) <= after
            if active and 'raft-obs:' in line:
                for name, value in re.findall(r'(pump_hold_max|lock_wait_max|drain_max|tick_max|apply_max|persist_max)=(\d+)us', line):
                    values[name] = max(values.get(name, 0), int(value))
            if active and 'an-pump:' in line:
                for name, value in re.findall(r'(active_cycle_max_us|mutex_hold_max_us)=(\d+)', line):
                    key = name.removesuffix('_us')
                    values[key] = max(values.get(key, 0), int(value))
    assert 'pump_hold_max' in values and 'lock_wait_max' in values, (root, node)
    return values


def summarize(root):
    assert (root / 'controller-exit').exists(), 'wait until the pipeline closes'
    controller_exit = int((root / 'controller-exit').read_text())
    cells, rejected, timing_gaps = {}, [], []
    for repeat, writers, transport, down in itertools.product(range(3), (1,8,32), ('tcp','rdma'), (False,True)):
        name = f'r{repeat}-{transport}-w{writers}-'+('down' if down else 'healthy')
        file = root / name / 'result.json'
        if not file.exists():
            rejected.append(dict(cell=name, reason='NOTRUN')); continue
        d = json.loads(file.read_text())
        if d['verdict'] != 'PASS':
            rejected.append(dict(cell=name, reason='FAIL', error=d.get('error'), cleanup=d.get('cleanup_errors'))); continue
        assert not d.get('errors') and not d.get('cleanup_errors')
        assert not d.get('purpose') and d.get('counters')
        assert (d['writers'], d['transport'], d['down']) == (writers, transport, down)
        assert d['verified_files'] == 4 * writers and d['seconds'] == 60
        assert d['counters']['fragment_error'] == 0
        assert int((root / (name+'-exit')).read_text()) == 0
        row = {n:d[n] for n in ('write_p95_s','write_p99_s','stat_rpc_p50_us','stat_rpc_p99_us','throughput_Bps','stat_missed_slots','fuse_cpu_seconds')}
        for counter in ('status_wire','owner_wire','proof_admissions','quorum_rounds'):
            row[counter+'_per_fragment'] = d['per_fragment'][counter]
        for node, value in d['cpu_seconds'].items(): row[f'n{node}_cpu_s'] = value
        for node in d['before']:
            observed = observation_max(file.parent, d, node)
            if not {'active_cycle_max', 'mutex_hold_max'} <= observed.keys():
                timing_gaps.append(dict(cell=name, node=node))
            for metric, value in observed.items():
                row[f'n{node}_{metric}_us'] = value
        cells[name] = row
    coverage = []
    with (root / 'ownership.jsonl').open() as source:
        for line in source:
            d = json.loads(line)
            if d['foreign_jobs'] or d['background_jobs']: coverage.append(d['t'])
    comparisons = []
    for writers, transport, down in itertools.product((1,8,32), ('tcp','rdma'), (False,True)):
        names = [f'r{r}-{transport}-w{writers}-'+('down' if down else 'healthy') for r in range(3)]
        if not all(n in cells for n in names): continue
        assert all(cells[n].keys() == cells[names[0]].keys() for n in names)
        metrics = {}
        for key in cells[names[0]]:
            values = [cells[n][key] for n in names]
            metrics[key] = dict(median=statistics.median(values), minimum=min(values), maximum=max(values))
        comparisons.append(dict(writers=writers, transport=transport, down=down, metrics=metrics))
    matrix_complete=controller_exit==0 and len(cells)==36 and not rejected and not coverage
    return dict(accepted_cells=len(cells), rejected=rejected, host_interference_samples=coverage,
                controller_exit=controller_exit,
                counter_latency_matrix_complete=matrix_complete,
                precise_pump_timing_gaps=timing_gaps,
                complete=matrix_complete and not timing_gaps,
                counter_scope='aggregate bracketing snapshots including stat/background reads; not exclusive STATUS causality',
                cells=cells, comparisons=comparisons)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    result = summarize(args.root)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['complete'] else 1)
