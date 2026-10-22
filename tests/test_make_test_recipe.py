#!/usr/bin/env python3
"""The `make test` recipe is the code gate: it runs every code test and nothing
that depends on files a release tree omits.

W135: `docs/check-architecture.py` links into `results/`, which release trees
and archived copies omit, so it lives in `make docs-check` (and `make check`
runs both). Any docs lag inside the `test` recipe would stop the code tests.
"""
from pathlib import Path
import os, re, subprocess
root = Path(__file__).resolve().parents[1]

env = dict(os.environ)
for k in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL'): env.pop(k, None)
out = subprocess.run(['make', '-n', 'test'], cwd=root, env=env, capture_output=True, text=True, check=True).stdout
cmds = [l for l in out.splitlines() if l.strip() and not l.startswith(('make', '#'))]
assert len(cmds) > 100, 'make -n test printed %d commands' % len(cmds)

docs = [c for c in cmds if 'check-architecture' in c or 'docs-check' in c]
assert not docs, 'the docs gate is inside the test recipe (W135): %s' % docs
print('make test recipe: %d commands, docs gate kept out of the code gate PASS' % len(cmds))
