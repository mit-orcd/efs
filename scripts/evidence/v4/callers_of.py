#!/usr/bin/env python3
"""Task V4 stage B: list the functions of a C file that call any of the named
functions. usage: callers_of.py FILE.c NAMES.txt [HEADER.h ...]
Prints one function name per line, sorted: the NAMES themselves that are defined
in FILE (or the headers), then the other functions whose body names one of them.
A function starts at a line `type name(` at column 0 and ends at the next line
that is exactly `}`."""
import re, sys
src = open(sys.argv[1]).read()
names = [l.strip() for l in open(sys.argv[2]) if l.strip() and not l.startswith('#')]
for h in sys.argv[3:]:
    src += open(h).read()
lines = src.split('\n')
fn, body, funcs = None, [], {}
sig = re.compile(r'^(?:static\s+|inline\s+)*[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$')
for i, l in enumerate(lines):
    if fn is None:
        m = sig.match(l)
        if m and not l.startswith((' ', '\t', '#', '/', '*')) and '=' not in l.split('(')[0]:
            fn, body = m.group(1), [l]
    else:
        body.append(l)
        if l == '}':
            funcs[fn] = '\n'.join(body)
            fn = None
out = set(n for n in names if n in funcs)
for f, b in funcs.items():
    if f in names:
        continue
    if any(re.search(r'\b%s\b' % re.escape(n), b) for n in names):
        out.add(f)
print('\n'.join(sorted(out)))
