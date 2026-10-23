#!/usr/bin/env python3
"""`make test-all` (= `make test` + `make test-long`) is the code gate: it runs
every code test and nothing that depends on files a release tree omits.
`make test` is the fast unit subset; no command runs in both.

W135: `docs/check-architecture.py` links into `results/`, which release trees
and archived copies omit, so it lives in `make docs-check` (and `make check`
runs both). Any docs lag inside the `test` recipe would stop the code tests.

W139: every unit binary in TEST_BINS and every tests/test_*.py and
tests/faults/test_*.py is run by the recipe, or is listed below with the
reason it is not. A test that genuinely needs hardware or a tool the host
lacks prints a `NOTRUN:` line and exits 0; scripts/make_test_summary.sh
lists those commands under NOTRUN and exits 3 (INCOMPLETE), so they never
pass silently.
"""
# Python tests the recipe does not run, each with its reason.
NOT_WIRED = {
    'tests/test_live_ticket_retry.py': 'W137: fake reply length; decided with the PUT-ticket stack (alpha K13)',
}
from pathlib import Path
import os, re, subprocess
root = Path(__file__).resolve().parents[1]

env = dict(os.environ)
for k in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL'): env.pop(k, None)
def dry(target):
    out = subprocess.run(['make', '-n', target], cwd=root, env=env, capture_output=True, text=True, check=True).stdout
    return [l for l in out.splitlines() if l.strip() and not l.startswith(('make', '#'))]
cmds = dry('test-all')
fast, slow = dry('test'), dry('test-long')
runs = lambda c: c.startswith(('python3 ', './tests/')) or c.split(' ', 1)[0].count('=')
both = sorted(set(c for c in fast if runs(c)) & set(c for c in slow if runs(c)))
assert not both, 'commands in both `make test` and `make test-long`: %s' % both
assert len(cmds) > 100, 'make -n test-all printed %d commands' % len(cmds)

docs = [c for c in cmds if 'check-architecture' in c or 'docs-check' in c]
assert not docs, 'the docs gate is inside the test recipe (W135): %s' % docs

tokens = set()
for c in cmds: tokens.update(c.replace(';', ' ').replace('&&', ' ').split())
makefile = (root / 'Makefile').read_text()
bins = set()
for m in re.finditer(r'^TEST_BINS\s*\+?=\s*(.*)$', makefile, re.M): bins.update(m.group(1).split())
assert len(bins) > 40, bins
unrun = sorted(b for b in bins if './' + b not in tokens)
assert not unrun, 'unit binaries built by `all` but never run by `make test-all` (W139): %s' % unrun

scripts = sorted(str(p.relative_to(root)) for p in list((root / 'tests').glob('test_*.py')) + list((root / 'tests/faults').glob('test_*.py')))
unrun = [s for s in scripts if s not in tokens and s not in NOT_WIRED]
assert not unrun, 'Python tests never run by `make test-all` and not listed with a reason (W139): %s' % unrun
stale = sorted(s for s in NOT_WIRED if s in tokens or not (root / s).exists())
assert not stale, 'NOT_WIRED entries that are wired or gone: %s' % stale
print('make test-all recipe: %d commands (%d fast, %d long), docs gate kept out, %d unit binaries and %d Python tests all run (%d listed as not wired) PASS'
      % (len(cmds), len(fast), len(slow), len(bins), len(scripts) - len(NOT_WIRED), len(NOT_WIRED)))
