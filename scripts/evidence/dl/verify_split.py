#!/usr/bin/env python3
"""Task DL: check that src/meta/meta_apply/ is src/meta/meta_apply.c moved.

usage: python3 scripts/evidence/dl/verify_split.py [BASE]   (default 0c3e34618)

Compares the normalised line multiset of the unsplit file at BASE with every
file of the directory. Lines only in the original must be `static ...` lines
(a definition or forward prototype that lost `static`, or the five byte
helpers that became `static inline`) or the continuation of a dropped
forward prototype. Lines only in the split must be header guards, includes,
comment lines, `static inline` helpers and declarations (a definition line
without `static`, or a continuation line the original already has). Prints
the counts; exits 1 on any other difference.
"""
import collections, re, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[3]
base = sys.argv[1] if len(sys.argv) > 1 else '0c3e34618'
orig = subprocess.check_output(['git', '-C', str(root), 'show', base + ':src/meta/meta_apply.c'],
                               text=True).split('\n')
split = []
for f in sorted((root / 'src/meta/meta_apply').glob('*.[ch]')):
    split += f.read_text().split('\n')
norm = lambda ls: collections.Counter(l.strip() for l in ls if l.strip())
o, n = norm(orig), norm(split)
gone, added = o - n, n - o
orig_text = '\n'.join(orig)

bad = []
for l, c in gone.items():
    if l.startswith('static '):
        continue
    if l == 'uint64_t generation, uint64_t bits,':   # dropped forward prototype of fence_lane_bits
        continue
    bad.append(('only in original', l, c))
kinds = collections.Counter()
decl = re.compile(r'^(?:int|void|uint\d+_t|int64_t)\s+\*?\w+\(')
for l, c in added.items():
    if re.match(r'#(ifndef|define|endif)\b.*EFS_META_APPLY_|#endif$', l):
        kinds['guards'] += c
    elif l.startswith('#include "'):
        kinds['includes'] += c
    elif l.startswith(('/*', '*', '*/')):
        kinds['comment lines'] += c
    elif l.startswith('static inline '):
        kinds['static inline helpers'] += c
    elif decl.match(l) and ('static ' + l.rstrip(';')) in orig_text.replace('\n', ' ') or \
            decl.match(l) and ('static ' + l) in orig_text:
        kinds['definition or declaration without static'] += c
    elif o[l] or o[l.rstrip(';')] or o[l.rstrip(';') + ')']:
        kinds['declaration continuation (repeats the definition)'] += c
    else:
        bad.append(('only in split', l, c))
print('only in original: %d lines, all `static` lines: %s' % (sum(gone.values()), not any(b[0] == 'only in original' for b in bad)))
for k, v in sorted(kinds.items()):
    print('only in split: %-45s %d' % (k, v))
for b in bad:
    print('UNEXPECTED %s: %r x%d' % b)
sys.exit(1 if bad else 0)
