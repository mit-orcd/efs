#!/usr/bin/env python3
"""classify.py REPO OUT — classify the reachability.sh output.

REPO must be a git checkout of the reviewed commit (definitions are located
with git grep); copy OUT back from the build host if needed.

Prints one line per function that no production binary keeps:
  DEAD      no production link and no test link keeps it
  TESTONLY  kept only by a test binary
  HDR       a static inline from a header, unused in one translation unit
            (not a finding by itself)
Library objects count as production-dead only when all five production links
discard them. Server objects are judged against efsd (and the efs-bench copy
for the storage files it recompiles). Callbacks reached through function
pointer tables stay alive because their table is referenced; a table that is
itself unreachable takes its members with it, which is why the in-memory
KV/Raft/store/transport backends show up as TESTONLY.

This is triage. Every reported finding was checked by hand with git grep.
"""
import collections, os, re, subprocess, sys

repo, out = sys.argv[1], sys.argv[2]
if os.path.exists(os.path.join(out, 'INCOMPLETE')) or not os.path.exists(os.path.join(out, 'COMPLETE')):
    sys.exit('classify: %s is not a complete reachability.sh scan' % out)
pat = re.compile(r"removing unused section '\.text\.([^']+)' in file '([^']+)'")


def removed(name):
    s = set()
    for line in open(os.path.join(out, name + '.gc')):
        m = pat.search(line)
        if m:
            fn, f = m.groups()
            s.add((re.sub(r'^libefs\.a\((.*)\)$', r'\1', f), fn))
    return s


b = {n: removed(n) for n in ('efsd', 'fuse', 'bench', 'mgmt', 'query')}
bench_copy = {'store', 'store_nvme', 'storage_roots', 'writer', 'iostats', 'thread', 'reclaim'}
own = {'fuse': 'src/client/efs_fuse.o', 'bench': 'src/client/efs_bench.o',
       'mgmt': 'src/mgmt/efs_mgmt.o', 'query': 'src/query/efs_query.o'}
dead = set(set.intersection(*b.values()))
for f, fn in b['efsd']:
    if f.startswith('src/server/'):
        base = os.path.basename(f)[:-2]
        if base not in bench_copy or ('src/bench/%s.o' % base, fn) in b['bench']:
            dead.add((f, fn))
for n, obj in own.items():
    for f, fn in b[n]:
        if f == obj or (n == 'bench' and f.startswith('src/bench/')
                        and os.path.basename(f)[:-2] not in bench_copy):
            dead.add((f, fn))
dead = {(f, fn) for f, fn in dead if not f.startswith('/') and 'blake3' not in f}

tested = collections.defaultdict(set)
for line in open(os.path.join(out, 'testsyms.txt')):
    p = line.split()
    if len(p) >= 2:
        tested[p[0]].add(p[1].rsplit(':', 1)[0])

rows = set()
for f, fn in dead:
    base = os.path.basename(f)[:-2] + '.c'
    grep = subprocess.run(['git', 'grep', '-n', '-E', r'^[A-Za-z_].*[ *]' + re.escape(fn) + r'\(',
                           '--', 'src', 'include'], cwd=repo, capture_output=True, text=True).stdout
    defs = [':'.join(x.split(':')[:2]) for x in grep.splitlines() if not x.rstrip().endswith(';')]
    loc = next((d for d in defs if os.path.basename(d.split(':')[0]) == base),
               next((d for d in defs if d.split(':')[0].endswith('.h')), '?'))
    if '.h:' in loc:
        kind = 'HDR'
    elif any(os.path.basename(x) == base or x.endswith('.h') for x in tested.get(fn, ())):
        kind = 'TESTONLY'
    else:
        kind = 'DEAD'
    rows.add((kind, loc, fn))
for row in sorted(rows):
    print(*row)
