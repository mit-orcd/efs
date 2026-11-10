#!/usr/bin/env python3
"""fi_summarize.py RUNDIR... — Task FI: one TSV row per fi_run.sh output
directory: ior write/read MiB/s, load average before/after, write-phase window
seconds, and over the write window the average cores used by each sampled
process (efsd1-3, fuse), whole-host busy cores and md0 write MiB/s, plus each
daemon's last iostats line (put ops, avg_us). Run on the host that made the
runs (ior prints local time). stdlib only."""
import re, sys, time
from pathlib import Path

CLK = 100.0

def ior(path):
    r = {}
    for l in path.read_text(errors='replace').splitlines():
        m = re.match(r'(write|read)\s+([\d.]+)\s', l)
        if m and m.group(1) not in r:
            r[m.group(1)] = float(m.group(2))
    return r

def epoch(l):
    s = l.split(': ', 1)[1].strip()
    return time.mktime(time.strptime(s, '%a %b %d %H:%M:%S %Y'))

def row(d):
    d = Path(d).resolve(); out = dict(set=d.parent.name, run=d.name)
    if not (d / 'ior.txt').exists():
        out['note'] = 'no ior.txt: cluster did not start'
        return out
    out.update({k + '_MiBs': v for k, v in ior(d / 'ior.txt').items()})
    for l in (d / 'ior.txt').read_text(errors='replace').splitlines():
        f = l.split()
        if len(f) >= 10 and f[0] == 'write' and f[7].replace('.', '').isdigit():
            out['wr_s'], out['close_s'] = f[7], f[8]   # ior: wr/rd(s) includes the close(s) shown beside it
            break
    meta = (d / 'meta.txt').read_text()
    m = re.search(r'load_before=(\S+ \S+ \S+)', meta); out['load_before'] = m.group(1) if m else '?'
    m = re.search(r'load_after=(\S+ \S+ \S+)', meta); out['load_after'] = m.group(1) if m else '?'
    w = [epoch(l) for l in (d / 'window.txt').read_text().splitlines()] if (d / 'window.txt').exists() else []
    if len(w) >= 2:
        t0, t1 = w[0], w[1]
        rows = [l.rstrip('\n').split('\t') for l in (d / 'sample.tsv').read_text().splitlines()]
        head, rows = rows[0], [r for r in rows[1:] if len(r) == len(rows[0])]
        sel = [r for r in rows if t0 <= float(r[0]) <= t1]
        if len(sel) >= 2:
            a, b = sel[0], sel[-1]; dt = float(b[0]) - float(a[0])
            out['write_s'] = round(dt, 1)
            for i, n in enumerate(head):
                if n in ('efsd1', 'efsd2', 'efsd3', 'fuse'):
                    out[n + '_cores'] = round((int(b[i]) - int(a[i])) / CLK / dt, 2)
            tot = int(b[1]) - int(a[1]); busy = tot - (int(b[2]) - int(a[2]))
            out['host_busy_cores'] = round(busy / CLK / dt, 1)
            out['md0_MiBs'] = round((int(b[3]) - int(a[3])) * 512 / 1048576 / dt)
            out['load1_mean'] = round(sum(float(r[4]) for r in sel) / len(sel), 2)
    th = d / 'sample.tsv.threads'
    if th.exists() and len(w) >= 2:
        first, last = {}, {}
        for l in th.read_text().splitlines():
            t, n, tid, comm, tk = l.split('\t')
            if w[0] <= float(t) <= w[1] and n in ('efsd1', 'fuse'):
                first.setdefault((n, tid), (float(t), comm, int(tk))); last[(n, tid)] = (float(t), comm, int(tk))
        for n in ('efsd1', 'fuse'):
            cpu = sorted(((last[k][2] - first[k][2]) / CLK / max(last[k][0] - first[k][0], 1), first[k][1])
                         for k in first if k[0] == n and k in last)[::-1][:4]
            out[n + '_top_threads'] = ' '.join(f'{c}:{v:.2f}' for v, c in cpu)
    if (d / 'rs-before-n1.txt').exists():
        def commits(f):
            return {int(m.group(1)): int(m.group(2)) for m in
                    re.finditer(r'group (\d+) hosted=\d+ .*? commit=(\d+)', (d / f).read_text(errors='replace'))}
        b, a = commits('rs-before-n1.txt'), commits('rs-after-n1.txt')
        mib = 16384.0   # 8 ranks x 2048 MiB, the fi_run.sh default
        for g in sorted(b):
            if g in a:
                out[f'g{g}_entries/MiB'] = round((a[g] - b[g]) / mib, 2)
    if (d / 'rs-before-n1.txt').exists():
        lead = {}
        for i in (1, 2, 3):
            for m in re.finditer(r'group (\d+) hosted=\d+ role=LEADER', (d / f'rs-before-n{i}.txt').read_text(errors='replace')):
                lead[int(m.group(1))] = i
        out['leaders_g0/g2'] = f"n{lead.get(0, '?')}/n{lead.get(2, '?')}"
    for i in (1, 2, 3):
        last = [l for l in (d / f'efsd-n{i}.log').read_text(errors='replace').splitlines() if 'iostats:' in l]
        m = re.search(r'put ops=(\d+) .*?avg_us=(\d+) .*?wdisk ops=(\d+) bytes=\d+ errors=\d+ avg_us=(\d+)', last[-1]) if last else None
        out[f'n{i}_put_ops/avg_us'] = f'{m.group(1)}/{m.group(2)}' if m else '-'
    return out

rows = [row(d) for d in sys.argv[1:]]
keys = []
for r in rows:
    for k in r:
        if k not in keys: keys.append(k)
print('\t'.join(keys))
for r in rows: print('\t'.join(str(r.get(k, '')) for k in keys))
