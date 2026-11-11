#!/usr/bin/env python3
"""check_failure_routes.py: validate docs/how-it-works/failure-routes.md.

Each `## ` section has the six labels in order; a heading key `word-word` is a
key of diagnostics.md; a function named on an "Emitted by" line is defined in
the file named there. Exit 1 with a message otherwise. No git, no arguments.
"""
import re, sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
LABELS = ['Line', 'Emitted by', 'Establishes', 'Does not establish', 'Next check', 'Seen in']
errs = []
diag = (root / 'docs/how-it-works/diagnostics.md').read_text()
keys = set(re.findall(r'^\| `([a-z][a-z0-9]*(?:-[a-z0-9]+)+)` \|', diag, re.M))
text = (root / 'docs/how-it-works/failure-routes.md').read_text()
for sec in re.split(r'^(?=## )', text, flags=re.M)[1:]:
    head = sec.split('\n', 1)[0]
    found = re.findall(r'^- (Line|Emitted by|Establishes|Does not establish|Next check|Seen in):', sec, re.M)
    if found != LABELS:
        errs.append('%s: labels %s, want %s' % (head, found, LABELS))
    m = re.match(r'## `([^`]*)`', head)
    k = re.match(r'[a-z][a-z0-9]*-[a-z0-9]+(?=$|[ :])', m.group(1)) if m else None
    if k and k.group(0) not in keys:
        errs.append('%s: %s is not a key in diagnostics.md' % (head, k.group(0)))
    for em in re.findall(r'^- Emitted by:(.*)$', sec, re.M):
        ticks = re.findall(r'`([^`]*)`', em)
        names = ticks[:ticks.index(next(t for t in ticks if '/' in t))] if any('/' in t for t in ticks) else []
        files = [t for t in ticks if '/' in t and '.' in t.rsplit('/', 1)[1]]
        if not names or not files:
            errs.append('%s: Emitted by lacks function or file' % head)
            continue
        src = (root / files[0]).read_text(errors='replace') if (root / files[0]).is_file() else None
        if src is None:
            errs.append('%s: file %s missing' % (head, files[0]))
        elif not re.search(r'\b%s\s*\(' % re.escape(names[0]), src):
            errs.append('%s: %s is not defined in %s' % (head, names[0], files[0]))
if errs:
    sys.exit('check_failure_routes: ' + '\n'.join(errs))
