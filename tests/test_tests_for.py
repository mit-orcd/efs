#!/usr/bin/env python3
"""Catalogue routes (scripts/tests_for.py): multi-tag rows listed once, a real
component without rows falls back to its parent with a note, a misspelled one
is refused (Task EB); a declared function prefix
without rows lists nothing, a misspelled one is refused (Task ED)."""
import importlib.util, re, subprocess, sys, tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('tests_for', root / 'scripts/tests_for.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

with tempfile.TemporaryDirectory(prefix='efs-tests-for-') as temp:
    t = Path(temp)
    tsv = t / 'budgets.tsv'
    tsv.write_text('# comment\ncommand\tbudget_s\tslots\treason\ttarget\tsubsystem\tpurpose\n'
                   './tests/a\t5\t1\tr\ttest\tclient/efs_fuse.open\tp\n'
                   'python3 tests/b.py\t5\t1\tr\ttest\tclient/write,client/efs_fuse.release\tp\n'
                   'python3 tests/c.py\t5\t1\tr\ttest-long\tclient/efs_fuse.release,client/efs_fuse.open\tp\n'
                   'python3 tests/d.py\t5\t1\tr\ttest\tmeta/meta_apply\tp\n')
    rows = list(m.catalogue(tsv))
    assert rows[1] == ('python3 tests/b.py', 'test', ['client/write', 'client/efs_fuse.release']), rows[1]
    # a command with two matching tags appears once, in catalogue order
    assert m.select(rows, 'client/efs_fuse') == [('./tests/a', 'test'), ('python3 tests/b.py', 'test'), ('python3 tests/c.py', 'test-long')]
    assert m.select(rows, 'client/efs_fuse.release') == [('python3 tests/b.py', 'test'), ('python3 tests/c.py', 'test-long')]
    assert m.select(rows, 'client/write') == [('python3 tests/b.py', 'test')]
    assert m.select(rows, 'client') == []  # a prefix is not a parent without the dot
    src = t / 'src'
    (src / 'meta/meta_apply').mkdir(parents=True)
    (src / 'meta/meta_apply/publish.c').write_text('')
    (src / 'client/efs_fuse').mkdir(parents=True)
    (src / 'client/efs_fuse/plock_init.h').write_text('')
    # a real component without a row of its own: the parent's rows, named
    assert m.resolve(rows, 'meta/meta_apply.publish', src) == ([('python3 tests/d.py', 'test')], 'meta/meta_apply')
    assert m.resolve(rows, 'client/efs_fuse.plock_init', src) == (m.select(rows, 'client/efs_fuse'), 'client/efs_fuse')
    # a misspelled component is refused, not answered with the parent's rows
    for bad in ('meta/meta_apply.publsh', 'client/efs_fuse.opne', 'meta/meta_apply.publish.deep'):
        try:
            m.resolve(rows, bad, src)
        except LookupError as e:
            assert 'unknown component %s' % bad in str(e), (bad, e)
        else:
            raise AssertionError(bad)
    try:
        m.resolve(rows, 'nowhere', src)
    except LookupError as e:
        assert 'no catalogue row for subsystem nowhere' in str(e)
    else:
        raise AssertionError('bare unknown subsystem must be refused')

# the real catalogue through the command line
run = lambda sub: subprocess.run([sys.executable, 'scripts/tests_for.py', sub], cwd=root, capture_output=True, text=True)
r = run('client/efs_fuse.release')
assert r.returncode == 0 and r.stdout.splitlines().count('python3 tests/test_stop_dirty_drain.py') == 1, r
r = run('meta/meta_apply.publish')
assert r.returncode == 0 and 'no row of its own for meta/meta_apply.publish' in r.stderr and './tests/test_meta_apply' in r.stdout, r
r = run('meta/meta_apply.publsh')
assert r.returncode == 2 and 'unknown component meta/meta_apply.publsh' in r.stderr and not r.stdout, r
# a declared function-prefix tag (Task ED): no rows lists nothing (exit 1), a misspelling is refused (exit 2)
prefixes = m.prefix_tags()
assert 'server/raft_host.publish' in prefixes and 'server/raft_host.read_index' in prefixes, prefixes
host = (root / 'src/server/raft_host.c').read_text()
for line in (root / 'tests/budgets.tsv').read_text().splitlines():
    if line.startswith('# prefixes server/raft_host:'):
        for c in ','.join(n.split('=')[1] for n in line.split(':', 1)[1].split()).split(','):
            assert re.search(r'^\w.*\b%s\w*\(' % c, host, re.M), 'declared prefix %s defines no function in raft_host.c' % c
r = run('server/raft_host.publish')
assert r.returncode == 1 and not r.stdout and 'function-prefix component server/raft_host.publish' in r.stderr, r
r = run('server/raft_host.publsh')
assert r.returncode == 2 and 'unknown component server/raft_host.publsh' in r.stderr and not r.stdout, r
# guide counts (Task ED): every stated form, right and drifted, on the temp catalogue
gspec = importlib.util.spec_from_file_location('check_guide_counts', root / 'scripts/check_guide_counts.py')
g = importlib.util.module_from_spec(gspec)
gspec.loader.exec_module(g)
with tempfile.TemporaryDirectory(prefix='efs-guide-counts-') as temp:
    guide = Path(temp) / 'README.md'
    rows = [('./tests/a', 'test', ['client/efs_fuse.open']), ('./tests/b', 'test', ['client/efs_fuse', 'client/write']),
            ('./tests/c', 'test', ['client/efs_fuse.open'])]
    guide.write_text('`tests_for.py client/efs_fuse.<file>`\n| s | `open.c` | h | x | `.open` (2) |\n| s | `main.c` | h | x | bare tag |\n'
                     '- `python3 scripts/tests_for.py client/efs_fuse` lists all 3 rows\nOne catalogue rows keep the bare `client/efs_fuse` tag\n')
    assert g.check(guide, rows) == [], g.check(guide, rows)
    guide.write_text('`tests_for.py client/efs_fuse.<file>`\n| s | `open.c` | h | x | `.open` (7) |\n| s | `main.c` | h | x | `.main` |\n'
                     '| s | `write.c` | h | x | none routed |\n- `python3 scripts/tests_for.py client/efs_fuse` lists all 50 rows\n'
                     'Two catalogue rows keep the bare `client/efs_fuse` tag\n- `python3 scripts/tests_for.py client/efs_fsue`\n')
    rows.append(('./tests/d', 'test', ['client/efs_fuse.write']))
    drift = g.check(guide, rows)
    assert [d.split(':')[1] for d in drift] == ['2', '3', '4', '5', '6', '7'], drift
print('tests_for: multi-tag rows once, real component falls back with a note, misspelled component refused, declared prefix without rows lists nothing PASS')
