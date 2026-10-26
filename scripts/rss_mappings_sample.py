#!/usr/bin/env python3
"""Sidecar RSS breakdown for the D4 plateau runs (F1; method of
docs/status/efsd-rss-plateau-20261025.md). Read-only: /proc only, no
ptrace, so it does not pause the daemons.

Usage: rss_mappings_sample.py TAG OUT.jsonl [INTERVAL_S=30]

Every INTERVAL_S seconds, for each efsd and efs-fuse whose argv contains
TAG (for example the tree path), appends one JSON line: per process its
threads, open sockets, VmRSS and an smaps breakdown in KiB: thread stacks
(1 MiB rw anonymous mappings after a ---p guard), large anonymous
mappings (>= 256 KiB: glibc arena heaps and mmapped blocks, also by
mapping size in `sizes`), all anonymous and all file-backed RSS, and the
file-backed RSS by file name (digits folded to N) in `files`. The key of
a process is its executable and its loopback endpoints and store path.
rss_mappings_split.py turns a series into growth per component."""
import json, os, re, sys, time
tag, outp, interval = sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 30


def smaps(pid):
    res = {'stack_n': 0, 'stack_rss': 0, 'stack_rss_max': 0, 'big_n': 0, 'big_rss': 0,
           'anon_rss': 0, 'file_rss': 0, 'sizes': {}, 'files': {}}
    prev_guard = False
    cur = None
    with open(f'/proc/{pid}/smaps') as f:
        for line in f:
            parts = line.split()
            if '-' in parts[0] and len(parts) >= 5 and ':' in parts[3]:
                if cur: _close(res, cur)
                a, b = (int(x, 16) for x in parts[0].split('-'))
                cur = {'size': b - a, 'perm': parts[1], 'path': parts[5] if len(parts) > 5 else '',
                       'guard': prev_guard, 'rss': 0}
                prev_guard = parts[1] == '---p'
            elif parts[0] == 'Rss:':
                cur['rss'] = int(parts[1])
        if cur: _close(res, cur)
    return res


def _close(res, m):
    anon = m['path'] in ('', '[heap]', '[stack]')
    if anon:
        res['anon_rss'] += m['rss']
    else:
        res['file_rss'] += m['rss']
        if m['rss']:
            k = re.sub(r'\d+', 'N', os.path.basename(m['path']))
            res['files'][k] = res['files'].get(k, 0) + m['rss']
    if not anon or m['perm'] == '---p':
        return
    if m['guard'] and m['size'] in (1 << 20, (1 << 20) + 4096, 8 << 20) and m['path'] == '':
        res['stack_n'] += 1
        res['stack_rss'] += m['rss']
        res['stack_rss_max'] = max(res['stack_rss_max'], m['rss'])
    elif m['size'] >= 256 << 10 and m['path'] == '':
        res['big_n'] += 1
        res['big_rss'] += m['rss']
        k = str(m['size'] >> 10)
        res['sizes'][k] = res['sizes'].get(k, 0) + m['rss']


def sockets(pid):
    n = 0
    for fd in os.listdir(f'/proc/{pid}/fd'):
        try:
            if os.readlink(f'/proc/{pid}/fd/{fd}').startswith('socket:'): n += 1
        except OSError:
            pass
    return n


with open(outp, 'a') as out:
    while True:
        t = time.time()
        rec = {'t': round(t, 1), 'p': {}}
        for d in os.listdir('/proc'):
            if not d.isdigit(): continue
            try:
                argv = open(f'/proc/{d}/cmdline', 'rb').read().split(b'\0')
                if tag.encode() not in b' '.join(argv): continue
                exe = os.path.basename(argv[0].decode())
                if exe not in ('efsd', 'efs-fuse'): continue
                st = open(f'/proc/{d}/status').read()
                f = {l.split(':')[0]: l.split(':', 1)[1].strip() for l in st.splitlines()}
                key = exe + ' ' + ' '.join(a.decode() for a in argv[1:6] if a.startswith(b'127.') or a.endswith(b'/n1') or a.startswith(b'/'))
                r = {'pid': int(d), 'thr': int(f['Threads']), 'rss': int(f['VmRSS'].split()[0]),
                     'sock': sockets(d)}
                r.update(smaps(d))
                rec['p'][key] = r
            except (OSError, KeyError, ValueError):
                continue
        out.write(json.dumps(rec) + '\n')
        out.flush()
        time.sleep(max(1, interval - (time.time() - t)))
