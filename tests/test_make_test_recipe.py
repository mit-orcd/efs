#!/usr/bin/env python3
"""`make test-all` (= `make test` + `make test-long`) is the code gate: it runs
every code test and nothing that depends on files a release tree omits.
`make test` is the fast unit subset; no command runs in both.

W135: `docs/check-architecture.py` links into `results/`, which release trees
and archived copies omit, so it lives in `make docs-check` (and `make check`
runs both). Any docs lag inside the `test` recipe would stop the code tests.

W139: every unit binary in TEST_BINS and every tests/test_*.py and
tests/faults/test_*.py is run by the recipe, or is listed below with the
reason it is not. W160: every test-long command has a time budget and pool
slots in tests/budgets.tsv, bounded here per command and as a gate. A test that genuinely needs hardware or a tool the host
lacks prints a `NOTRUN:` line and exits 0; scripts/make_test_summary.sh
lists those commands under NOTRUN and exits 3 (INCOMPLETE), so they never
pass silently.
"""
# Python tests the recipe does not run, each with its reason.
NOT_WIRED = {
    'tests/test_soak.py': 'Task D 15-minute accelerated endurance run; its own gate (make test-soak), outside the W160 15-minute test-all target',
    'tests/test_d4_plateau.py': 'F1/D4 measurement driver (30-60 min runs); its gate-length drain and '
                                'efsd RSS are open findings (docs/status/f1-followups-20261025.md)',
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

# W160: the gate's time budget. Every test-long command declares in
# tests/budgets.tsv the seconds it may take at EFS_TEST_JOBS=8 and the pool
# slots it occupies; a budget above COMMAND_BOUND_S needs a recorded reason,
# and the gate the budgets add up to (the pool's list schedule at 8 slots,
# largest budget first, as scripts/make_test_summary.sh runs it, plus the
# EXCLUSIVE commands' budgets in series and the fast suite) stays under
# GATE_BOUND_S: the 15 min target with the budgets' own margin. A new or slower test therefore raises its budget here, in
# the same change, or fails this test; the gate cannot drift unnoticed.
COMMAND_BOUND_S = 300
GATE_TARGET_S = 960         # the W160 gate, measured at EFS_TEST_JOBS=8: 15 min until wave 9 (Oct 25) added the
                            # D43 expiry fixture, decoder refusal, K38, W159 and W77 tests (pool 1,245 s); 16 min since
BUDGET_MARGIN = 1.5         # budgets are 1.25 x the measured 8-job time, rounded up to 5 s with a 5 s floor,
                            # so the gate they add up to may be up to 1.5 x the target
GATE_BOUND_S = GATE_TARGET_S * BUDGET_MARGIN
JOBS = 8
FAST_DEFAULT_S = 1          # a `make test` command without a budget row (each runs well under a second)
budgets = {}
for line in (root / 'tests/budgets.tsv').read_text().splitlines():
    f = line.split('\t')
    if not line.strip() or line.startswith('#') or f[0] == 'command':
        continue
    assert len(f) == 4, 'tests/budgets.tsv: want command, budget_s, slots, reason: %r' % line
    cmd, budget, slots, reason = f[0].strip(), float(f[1]), int(f[2]), f[3].strip()
    assert cmd not in budgets, 'tests/budgets.tsv: %s twice' % cmd
    assert budget > 0 and 1 <= slots <= JOBS, line
    assert budget <= COMMAND_BOUND_S or reason, \
        'tests/budgets.tsv: %s is budgeted %g s, above %d s, with no reason' % (cmd, budget, COMMAND_BOUND_S)
    budgets[cmd] = (budget, slots, reason)
long_cmds = [c.strip() for c in slow if runs(c)]
missing = [c for c in long_cmds if c not in budgets]
assert not missing, 'test-long commands without a row in tests/budgets.tsv: %s' % missing
gone = sorted(c for c in budgets if c not in long_cmds)
assert not gone, 'tests/budgets.tsv rows for commands `make test-long` no longer runs: %s' % gone

exclusive = re.search(r'EXCLUSIVE=re\.compile\((.*?)\)\n', (root / 'scripts/make_test_summary.sh').read_text(), re.S).group(1)
exclusive = re.compile(''.join(re.findall(r'"([^"]*)"', exclusive)))
pool, alone = [], 0.0
for c in [c.strip() for c in cmds if runs(c)]:
    budget, slots = budgets.get(c, (FAST_DEFAULT_S, 1))[:2]
    slots = min(slots, JOBS)
    if exclusive.search(c):
        alone += budget
    else:
        pool.append((budget, slots))
# The runner's pool: most slots first, then the largest budget; the first
# pending command whose slots fit starts, else the schedule waits for a finish.
import heapq
pending = sorted(pool, key=lambda b: (-b[1], -b[0]))
running, free, now = [], JOBS, 0.0     # running: (end, slots)
while pending:
    k = next((k for k, b in enumerate(pending) if b[1] <= free), None)
    if k is None:
        now, got = heapq.heappop(running)
        free += got
        continue
    budget, slots = pending.pop(k)
    heapq.heappush(running, (now + budget, slots))
    free -= slots
makespan = max(end for end, _ in running) if running else 0.0
total = makespan + alone
assert total <= GATE_BOUND_S, ('the budgets in tests/budgets.tsv add up to %.0f s at %d jobs (pool %.0f s + exclusive %.0f s), '
                               'above the %.0f s gate bound (%d s target x %.2f budget margin): make the test faster or split it, and record why if a budget must grow'
                               % (total, JOBS, makespan, alone, GATE_BOUND_S, GATE_TARGET_S, BUDGET_MARGIN))
print('make test-all recipe: %d commands (%d fast, %d long), docs gate kept out, %d unit binaries and %d Python tests all run '
      '(%d listed as not wired); budgets: %d rows, budgeted gate %.0f s of %.0f s at %d jobs PASS'
      % (len(cmds), len(fast), len(slow), len(bins), len(scripts) - len(NOT_WIRED), len(NOT_WIRED), len(budgets), total, GATE_BOUND_S, JOBS))
