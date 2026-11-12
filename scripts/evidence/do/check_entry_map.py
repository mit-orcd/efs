#!/usr/bin/env python3
"""Task DO: fail when the ReadIndex entry map names something the tree lacks.

usage: python3 scripts/evidence/do/check_entry_map.py [PAGE]
(default docs/status/2026-11/readindex-entry-map-20261108.md)

Reads the `## Entry map` section of PAGE and checks, against the tree:
1. the first column of the function table is exactly the set of `host_read_*`
   and `host_wait_*` functions defined in src/server/raft_host.c and
   src/server/read_index_pair_remote.h (a renamed, removed or new one fails);
2. the first column of the support table names functions defined in
   src/server/raft_host.c, src/server/group_view.c/.h, src/server/read_proof.c/.h or src/raft/raft.c;
3. every other backticked name in the section is a path that exists or a C
   expression whose identifiers all occur in the ReadIndex sources;
4. the test table is exactly `scripts/tests_for.py server/raft_host.read_index`.
Line numbers in the page are a snapshot of the commit it names and are not
checked. Exits 1 and lists every stale name.
"""
import re, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[3]
page = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'docs/status/2026-11/readindex-entry-map-20261108.md'
# the group view owner (Task V3) holds host_view_get, the wait predicates and the sleepers
host = (root / 'src/server/raft_host.c').read_text() + '\n' + (root / 'src/server/group_view.c').read_text() + '\n' + (root / 'src/server/group_view.h').read_text() + '\n' + (root / 'src/server/group_view_impl.h').read_text() + '\n' + (root / 'src/server/read_proof.c').read_text() + '\n' + (root / 'src/server/read_proof.h').read_text()
pair = (root / 'src/server/read_index_pair_remote.h').read_text()
raft = (root / 'src/raft/raft.c').read_text()
corpus = '\n'.join([host, raft] + [p.read_text() for p in sorted((root / 'src/server').glob('*.h'))]
                   + [p.read_text() for p in sorted((root / 'include/efs').glob('*.h'))])
words = set(re.findall(r'\b[A-Za-z_]\w*\b', corpus))
KEYWORDS = {'struct', 'static', 'const', 'int', 'void', 'unsigned', 'uint64_t', 'uint8_t'}


def defined(text):
    """Names of functions defined (not just declared) at file scope."""
    return set(re.findall(r'^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w \t\*]*?\b([a-z_]\w*)\s*\([^;{]*\)\s*\{',
                          text, re.M))


section = page.read_text().split('\n## Entry map', 1)[1].split('\n## ', 1)[0]
tables, cur = [], None
for line in section.split('\n'):
    if line.startswith('|'):
        if cur is None:
            cur = []
            tables.append(cur)
        cells = [c.strip() for c in line.strip('|').split('|')]
        if not set(''.join(cells)) <= set('-: '):
            cur.append(cells)
    else:
        cur = None
bad = []
want = {n for n in defined(host) | defined(pair) if re.match(r'host_(read|wait)_|read_proof_begin$', n)}
first = lambda t: [re.sub(r'[`()]', '', r[0]) for r in t[1:]]
by_head = {t[0][0]: t for t in tables if t}
funcs = set(first(by_head.get('Function', [[]])))
for n in sorted(want - funcs):
    bad.append(f'function table misses {n}')
for n in sorted(funcs - want):
    bad.append(f'function table names {n}, not a host_read_/host_wait_ definition')
support = defined(host) | defined(raft)
for n in first(by_head.get('Support function', [[]])):
    if n not in support:
        bad.append(f'support table names {n}, not defined in raft_host.c or raft.c')
for tok in re.findall(r'`([^`]+)`', section):
    if '/' in tok:
        if not (root / tok.split(':')[0]).exists():
            bad.append(f'path {tok} does not exist')
        continue
    for w in re.findall(r'[A-Za-z_]\w*', tok):
        if w not in KEYWORDS and w not in words:
            bad.append(f'name {w} (in `{tok}`) not in the ReadIndex sources')
routed = subprocess.run([sys.executable, str(root / 'scripts/tests_for.py'), 'server/raft_host.read_index'],
                        cwd=root, capture_output=True, text=True, check=True).stdout.split()
routed = {w for w in routed if w.startswith('tests/')}
listed = {re.sub(r'`', '', r[0]) for r in by_head.get('Routed test', [[]])[1:]}
for t in sorted(routed ^ listed):
    bad.append(f'test table and tests_for.py differ on {t}')
for b in bad:
    print('STALE:', b)
print(f'entry map: {len(funcs)} functions, {len(listed)} routed tests, {len(bad)} stale names')
sys.exit(1 if bad else 0)
