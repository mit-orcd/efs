#!/usr/bin/env python3
"""Summarize external Intel profile captures without copying raw data into Git.

Run on a host: python3 SCRIPT /data1/efs/w100-private/results/profile-seven-...
Durations are syscall elapsed time, including tracing overhead; sums overlap
across threads. Unfinished rows are counted separately and paired with their
resumed rows where possible. Futex/socket time alone is not proof of a bug.
"""
import argparse
from collections import Counter, defaultdict
import heapq
import json
import os
from pathlib import Path
import re
import subprocess


def lock_symbols(target):
    """Read symbols/types and live pointer values; never attach a GDB inferior."""
    exe = '/proc/%d/exe' % target['pid']
    nm = subprocess.check_output(['nm', '-S', exe], text=True)
    ranges = []
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in ('b', 'B', 'd', 'D'):
            ranges.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
    # These Intel Makefile builds are ET_EXEC, so nm addresses are runtime
    # addresses. Refuse to guess an ASLR relocation for another binary type.
    header = subprocess.check_output(['readelf', '-h', exe], text=True)
    assert re.search(r'Type:\s+EXEC', header), 'expected Intel ET_EXEC binary'
    if target['label'].startswith('node-'):
        ptr = next(addr for addr, _size, name in ranges if name == 'g_host')
        fd = os.open('/proc/%d/mem' % target['pid'], os.O_RDONLY)
        try:
            host = int.from_bytes(os.pread(fd, 8, ptr), 'little')
        finally:
            os.close(fd)
        fields = ['mu', 'cv_mu', 'outbox_mu', 'inbox_mu', 'wait_mu',
                  'applied_cv', 'waiters', 'tx']
        cmd = ['gdb', '-q', '-nx', '-batch', '-iex', 'set auto-load off', exe]
        for field in fields:
            cmd += ['-ex', 'p/x (unsigned long)&((struct efs_raft_host *)0)->'+field,
                    '-ex', 'p/x sizeof(((struct efs_raft_host *)0)->'+field+')']
        text = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)
        values = [int(x, 16) for x in re.findall(r'\$\d+ = (0x[0-9a-f]+)', text)]
        assert len(values) == len(fields)*2, text
        for i, field in enumerate(fields):
            ranges.append((host+values[i*2], values[i*2+1], 'h->'+field))
    return ranges


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    a = ap.parse_args()
    root = a.capture
    result = {}
    manifest = json.loads((root/'manifest.json').read_text())
    symbols = {}
    symbol_errors = {}
    for target in manifest['targets']:
        try:
            symbols[target['label']] = lock_symbols(target)
        except (OSError, subprocess.SubprocessError, AssertionError, StopIteration) as exc:
            symbol_errors[target['label']] = str(exc)
    for p in sorted(root.glob('strace-*.log')):
        groups = defaultdict(list)
        longest = []
        errors = Counter()
        uncompleted = 0
        pending = {}
        addresses = defaultdict(list)
        paths = defaultdict(list)
        bounds = []
        for line in p.open(errors='replace'):
            stamp = re.match(r'\s*(\d+)\s+(\d{10}\.\d+)\s+', line)
            if stamp:
                bounds.append(float(stamp[2]))
                tid = stamp[1]
                if '<unfinished ...>' in line:
                    pending[tid] = line.strip()
                elif '<... ' in line and ' resumed>' in line and tid in pending:
                    line = pending.pop(tid) + ' || ' + line.strip()
            syscall = re.search(r'\b([a-z0-9_]+)\(|<\.\.\. ([a-z0-9_]+) resumed>', line)
            duration = re.search(r'<([0-9]+\.[0-9]+)>\s*$', line)
            if not syscall:
                continue
            name = syscall[1] or syscall[2]
            if ' = -1 ' in line:
                m = re.search(r' = -1 ([A-Z0-9_]+)', line)
                if m:
                    errors[name+'/'+m[1]] += 1
            if not duration:
                if '<unfinished ...>' in line:
                    uncompleted += 1
                continue
            seconds = float(duration[1])
            if name == 'futex':
                addr = re.search(r'futex\((0x[0-9a-f]+)', line)
                if addr and 'FUTEX_WAKE' not in line:
                    addresses[addr[1]].append(seconds)
                name += '-wake' if 'FUTEX_WAKE' in line else '-wait-or-resumed'
            if name in ('fdatasync', 'fsync', 'pwrite64', 'pread64'):
                path = re.search(r'\d+<(/[^>]*)>', line)
                if path:
                    paths[name+' '+path[1]].append(seconds)
            groups[name].append(seconds)
            if seconds >= .01:
                item = (seconds, line.strip())
                if len(longest) < 12:
                    heapq.heappush(longest, item)
                elif item > longest[0]:
                    heapq.heapreplace(longest, item)
        stats = {}
        for name, values in groups.items():
            values.sort()
            stats[name] = dict(count=len(values), seconds=sum(values),
                p50=values[(len(values)-1)//2], p95=values[int((len(values)-1)*.95)],
                p99=values[int((len(values)-1)*.99)], maximum=values[-1],
                over_100ms=sum(x >= .1 for x in values),
                over_1s=sum(x >= 1 for x in values))
        label = p.stem[len('strace-'):].rsplit('-', 1)[0]
        def symbolic(addr):
            matches = [(size, name, int(addr,16)-start) for start, size, name in symbols.get(label, [])
                       if start <= int(addr,16) < start+size]
            if not matches:
                return 'unmapped'
            size, name, offset = min(matches)
            return name+'+0x%x' % offset
        result[p.name] = dict(syscalls=stats, longest=sorted(longest, reverse=True),
            errors=dict(errors), unfinished_rows=uncompleted,
            bounds=[min(bounds), max(bounds)] if bounds else [],
            futex_addresses={addr: dict(count=len(v), seconds=sum(v), maximum=max(v), symbol=symbolic(addr))
                             for addr, v in addresses.items()},
            sync_paths={path: dict(count=len(v), seconds=sum(v), maximum=max(v))
                        for path, v in paths.items() if path.startswith(('fdatasync ', 'fsync '))})
    (root/'strace-summary.json').write_text(json.dumps(result, indent=2)+'\n')
    (root/'lock-symbols.json').write_text(json.dumps(dict(ranges=symbols, errors=symbol_errors), indent=2)+'\n')
    if 'clock' in manifest:
        start = manifest['clock']['monotonic'] + manifest['start_at'] - manifest['clock']['wall']
        for lo, hi in [(20, 38), (50, 68), (80, 110)]:
            cmd = ['perf', 'report', '--stdio', '-i', str(root/'perf.data'), '--no-children',
                   '--call-graph', 'none', '--percent-limit', '0.5', '--sort', 'comm,symbol',
                   '--time', '%.6f,%.6f' % (start+lo, start+hi)]
            with (root/('perf-untraced-%d-%d.txt' % (lo, hi))).open('w') as f:
                subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=90)
    for label in sorted({name.rsplit('-', 1)[0] for name in result}):
        totals = defaultdict(list)
        for name, data in result.items():
            if name.rsplit('-', 1)[0] != label:
                continue
            for syscall, stats in data['syscalls'].items():
                totals[syscall].append(stats)
        summary = {name: dict(count=sum(x['count'] for x in rows),
                   seconds=round(sum(x['seconds'] for x in rows), 3),
                   maximum=max(x['maximum'] for x in rows),
                   over_1s=sum(x['over_1s'] for x in rows))
                   for name, rows in totals.items()}
        print(label, json.dumps(summary, sort_keys=True))


if __name__ == '__main__':
    main()
