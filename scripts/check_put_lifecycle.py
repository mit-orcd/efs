#!/usr/bin/env python3
"""check_put_lifecycle.py: validate docs/how-it-works/put-lifecycle.md.

The page has the seven sections "## 1." .. "## 7."; each ends with a
`Functions:` line of the form
  Functions: `src/a.c`: `f1` `f2`; `src/b.c`: `g1`
A backticked token with a '/' names a file; the names after it belong to it
until the next file. Each name must be defined (name followed by '(' at the
start of a line or after a type) in that file. Exit 1 with a message otherwise.
No git, no arguments.
"""
import re, sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
page = root / 'docs/how-it-works/put-lifecycle.md'
text = page.read_text()
errs = []
secs = {m.group(1): m.group(0) for m in re.finditer(r'^## (\d)\. .*?(?=^## |\Z)', text, re.M | re.S)}
for n in '1234567':
    sec = secs.get(n)
    if sec is None:
        errs.append('section %s is missing' % n)
        continue
    fl = re.findall(r'^Functions:(.*)$', sec, re.M)
    if len(fl) != 1:
        errs.append('section %s: want one Functions: line, found %d' % (n, len(fl)))
        continue
    cur = None
    for tok in re.findall(r'`([^`]*)`', fl[0]):
        if '/' in tok:
            cur = tok
            if not (root / cur).is_file():
                errs.append('section %s: file %s does not exist' % (n, cur))
                cur = None
            continue
        if cur is None:
            errs.append('section %s: %s has no file before it' % (n, tok))
            continue
        src = (root / cur).read_text(errors='replace')
        if not re.search(r'^[A-Za-z_][\w \*]*[ \*]%s\(' % re.escape(tok), src, re.M):
            errs.append('section %s: %s is not defined in %s' % (n, tok, cur))
if errs:
    sys.exit('check_put_lifecycle: ' + '\n'.join(errs))
