#!/usr/bin/env python3
"""A12 dead-code sweep: struct fields nothing reads.

Scans src/ and include/ (and tests/, tools/ for reads) with comments
stripped. For every member of a struct defined there, counts member accesses
(`.f` or `->f`) as writes (`=`, `op=`, `++`, `--` follows), frees
(`free(x->f)`) or reads (anything else). Prints the fields with no read:

  FILE  struct NAME  FIELD  writes=N frees=M

A field with writes and no read is written for nothing; one with only frees
is never set. Names are matched across structs, so a field whose name is
read on another struct is not reported (a miss, never a false report of a
read field). Wire and on-disk layouts (include/efs/protocol.h and the like)
appear too and must keep their fields. Candidates only: check each.

  python3 scripts/unread_fields.py [TREE]"""
import collections, re, sys
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1])
strip = lambda t: re.sub(r'/\*.*?\*/|//[^\n]*', ' ', t, flags=re.S)
srcs = sorted(root.glob('src/**/*.[ch]')) + sorted(root.glob('include/**/*.h'))
text = {p: strip(p.read_text(errors='replace')) for p in srcs}
more = sorted(root.glob('tests/**/*.c')) + sorted(root.glob('tools/**/*.c'))
corpus = '\n'.join(text.values()) + '\n'.join(strip(p.read_text(errors='replace')) for p in more)

fields = []
for p, t in text.items():
    for m in re.finditer(r'struct\s+(\w+)\s*\{', t):
        depth, i = 0, m.end() - 1
        start = i
        while i < len(t):
            depth += {'{': 1, '}': -1}.get(t[i], 0)
            if depth == 0:
                break
            i += 1
        body = t[start + 1:i]
        for _ in range(2):
            body = re.sub(r'\{[^{}]*\}', ' ', body)
        for decl in body.split(';'):
            decl = re.sub(r'\(\s*\*\s*(\w+)\s*\)\s*\(.*', r' \1', decl, flags=re.S)
            decl = re.sub(r'\[[^\]]*\]', '', decl)
            decl = re.sub(r':\s*\w+', '', decl)
            for part in decl.split(','):
                ids = re.findall(r'[A-Za-z_]\w*', part)
                if ids and ids[-1] not in ('struct', 'union', 'const', 'void', 'unsigned', 'int', 'char'):
                    fields.append((str(p.relative_to(root)), m.group(1), ids[-1]))

reads, writes, frees = collections.Counter(), collections.Counter(), collections.Counter()
acc = re.compile(r'(free\(\s*\w+\s*)?(?:\.|->)\s*([A-Za-z_]\w*)(\s*(?:\[[^\]]*\]\s*)*)'
                 r'(\)|=(?!=)|\+\+|--|[-+|&^*/%]=|<<=|>>=)?')
for m in acc.finditer(corpus):
    f, op = m.group(2), m.group(4)
    if m.group(1) and op == ')':
        frees[f] += 1
    elif op and op != ')':
        writes[f] += 1
    else:
        reads[f] += 1
seen = set()
for p, s, f in fields:
    if (s, f) in seen or reads[f]:
        continue
    seen.add((s, f))
    print(f'{p}\tstruct {s}\t{f}\twrites={writes[f]} frees={frees[f]}')
