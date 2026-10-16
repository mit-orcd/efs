#!/usr/bin/env python3
"""Summarize completed ORCD rounds; run on a host that sees the shared artifacts."""
import argparse
import json
from pathlib import Path
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('artifacts', type=Path)
a = p.parse_args()
results = {}
for directory in sorted(a.artifacts.iterdir()):
    files = sorted(directory.glob('fcstor*/workload.json')) if directory.is_dir() else []
    if not files:
        continue
    rows, windows, exits = [], [], {}
    for f in files:
        d = json.loads(f.read_text())
        rows.extend(d['rows'])
        finish = f.stat().st_mtime
        windows.append((finish - d['elapsed'], finish))
        exits[f.parent.name] = (f.parent / 'workload.exit').read_text().strip()
    start = statistics.median(w[0] for w in windows)
    finish = statistics.median(w[1] for w in windows)
    summary = {}
    for op in sorted({r['op'] for r in rows}):
        entries = [r for r in rows if r['op'] == op]
        values = sorted(r['ms'] for r in entries)
        summary[op] = dict(count=len(values), errors=sum(r['error'] is not None for r in entries),
                           p50=values[int(.5 * (len(values) - 1))],
                           p95=values[int(.95 * (len(values) - 1))],
                           p99=values[int(.99 * (len(values) - 1))], maximum=max(values))
    resources = {}
    for f in directory.glob('fcstor*/resource.jsonl'):
        identity = json.loads(f.with_suffix('.identity.json').read_text())
        data = [json.loads(line) for line in f.read_text().splitlines()]
        data = [r for r in data if start <= r['time'] <= finish]
        if len(data) < 2:
            raise RuntimeError('Missing resource window: ' + str(f))
        first, last = data[0], data[-1]
        env = identity['env']
        assert identity['verbs'] and env['EFS_TRANSPORT'] == 'rdma'
        assert env['EFS_EXPIRY'] == '0'
        assert env['EFS_COMMANDS'] == '1' and env['EFS_COMMANDS_SERVER'] == '1'
        if f.parent.name in ('fcstor003', 'fcstor004', 'fcstor005', 'fcstor006'):
            assert '--direct-io' in identity['args']
        def io(record):
            return {k: int(v) for k, v in (line.split(':') for line in record['io'].splitlines())}
        before, after = io(first), io(last)
        resources[f.parent.name] = dict(
            cpu_cores=(last['ticks'] - first['ticks']) / identity['hz'] / (last['time'] - first['time']),
            peak_rss_mib=max(r['rss_pages'] for r in data) * 4096 / 2**20,
            read_bytes=after['read_bytes'] - before['read_bytes'],
            read_syscalls=after['syscr'] - before['syscr'],
            read_chars=after['rchar'] - before['rchar'],
            write_bytes=after['write_bytes'] - before['write_bytes'],
            sha256=identity['sha256'], environment=env)
    posix, failures = {}, []
    for f in directory.glob('fcstor*/posix.tsv'):
        for line in f.read_text().splitlines():
            columns = line.split('\t')
            if len(columns) > 1 and columns[1] in ('PASS', 'FAIL', 'SKIP', 'NOTRUN'):
                status = columns[1]
                posix[status] = posix.get(status, 0) + 1
                if status != 'PASS':
                    failures.append(dict(host=f.parent.name, row=columns))
    results[directory.name] = dict(summary=summary, exits=exits,
                                    resource_window=[start, finish], resources=resources,
                                    posix=posix, failures=failures)
print(json.dumps(results, indent=2))
