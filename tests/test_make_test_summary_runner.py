#!/usr/bin/env python3
"""Self-test of the runner embedded in scripts/make_test_summary.sh (K25/R1).

Exit 0 must mean "every command ran and passed". A failed or timed-out
command exits 1 with STATUS FAIL; all passing but one NOTRUN/SKIP line exits
3 with STATUS INCOMPLETE. Each failure keeps its full output in
tmp/fail-<index>.log, named on its summary line. The first case is the
reproducer from docs/status/devel-integration-review-20261022.md (R1), which
exited 0 before the fix. Runs the extracted Python block in a temporary
directory: no build, no make, no cluster.
"""
from pathlib import Path
import ast, os, shutil, subprocess, sys, tempfile, time, types

root = Path(__file__).resolve().parents[1]
source = (root / 'scripts/make_test_summary.sh').read_text()
runner = source.split('python3 - <<"EOP"\n', 1)[1].rsplit('\nEOP', 1)[0]


def run(cmds, timeout=None, jobs=None, budget_rows=None):
    """Run the runner on cmds; return (status, summary lines, directory)."""
    d = tempfile.mkdtemp(prefix='efs-summary-runner-')
    Path(d, 'cmds.txt').write_text(cmds)
    if budget_rows:
        Path(d,'tests').mkdir()
        Path(d,'tests/budgets.tsv').write_text(budget_rows)
    env = dict(os.environ)
    if timeout is not None:
        env['EFS_TEST_CMD_TIMEOUT'] = str(timeout)
    # Never inherit the outer run's pool size: serial unless a case asks.
    env['EFS_TEST_JOBS'] = str(jobs if jobs is not None else 1)
    r = subprocess.run([sys.executable, '-c', runner], cwd=d, env=env, timeout=60)
    return r.returncode, Path(d, 'summary.txt').read_text().splitlines(), d


def head(line, total, fail, notrun, status):
    want = 'TOTAL %d FAIL %d NOTRUN %d STATUS %s (' % (total, fail, notrun, status)
    assert line.startswith(want), (line, want)


dirs = []
try:
    # R1 reproducer: a failure plus a SKIP must not exit 0.
    rc, s, d = run("exit 7\nprintf 'SKIP injected prerequisite\\n'\n"); dirs.append(d)
    assert rc == 1, rc
    head(s[0], 2, 1, 1, 'FAIL')
    f = s[1].split('\t')
    assert f[:3] == ['0', '7', 'exit 7'] and f[4] == 'tmp/fail-0.log', f
    assert s[2].startswith('NOTRUN\t1\t'), s

    # All pass, including a backslash-continued command.
    rc, s, d = run("true\necho one \\\n  two\nprintf 'ok\\n'\n"); dirs.append(d)
    assert rc == 0, rc
    head(s[0], 3, 0, 0, 'PASS')
    assert len(s) == 1, s
    assert not list(Path(d, 'tmp').iterdir()), 'a passing run left failure logs'

    # A failure keeps every output line, not only the last one.
    rc, s, d = run("true\nfor i in 1 2 3; do echo line$i; done; echo err >&2; exit 4\n"); dirs.append(d)
    assert rc == 1, rc
    head(s[0], 2, 1, 0, 'FAIL')
    f = s[1].split('\t')
    assert f[0] == '1' and f[1] == '4' and f[3] == 'err' and f[4] == 'tmp/fail-1.log', f
    log = Path(d, f[4]).read_text()
    assert all(x in log for x in ('line1', 'line2', 'line3', 'err', '# exit 4')), log

    # A timeout fails (status 124) and kills the command's children.
    rc, s, d = run("true\nsleep 30 & sleep 30; wait\n", timeout=1); dirs.append(d)
    assert rc == 1, rc
    head(s[0], 2, 1, 0, 'FAIL')
    f = s[1].split('\t')
    assert f[:2] == ['1', '124'], f
    assert 'TIMEOUT after 1s' in Path(d, f[4]).read_text()

    # Task D: declared long budgets may exceed the default only. An explicit
    # hard user timeout must still interrupt a command with a larger budget.
    rc,s,d=run('sleep 2\n',timeout=.1,budget_rows='sleep 2\t1200\t8\tendurance\n');dirs.append(d)
    assert rc==1 and s[1].split('\t')[1]=='124',s
    # Exercise the real timeout selection without a 900-second sleep.
    fn=next(n for n in ast.parse(runner).body if isinstance(n,ast.FunctionDef) and n.name=='run')
    selected=[]
    class Child:
        returncode=0
        def communicate(self,timeout):selected.append(timeout);return ('',None)
    env={}
    ns=dict(budget=lambda c:(1200,8),limit=900,os=types.SimpleNamespace(environ=env),
            subprocess=types.SimpleNamespace(Popen=lambda *a,**k:Child(),PIPE=-1,STDOUT=-2),text=lambda x:x)
    exec(compile(ast.Module(body=[fn],type_ignores=[]),'<real runner timeout>','exec'),ns)
    assert ns['run']('true')==(0,'') and selected[-1]==1200
    env['EFS_TEST_CMD_TIMEOUT']='0.1';ns['limit']=.1
    assert ns['run']('true')==(0,'') and selected[-1]==.1

    # NOTRUN with everything passing is INCOMPLETE, never PASS.
    rc, s, d = run("true\necho 'NOTRUN: no RDMA device'\n"); dirs.append(d)
    assert rc == 3, rc
    head(s[0], 2, 0, 1, 'INCOMPLETE')
    assert s[1] == "NOTRUN\t1\techo 'NOTRUN: no RDMA device'\tNOTRUN: no RDMA device", s

    # No commands at all is not a pass.
    rc, s, d = run("\n"); dirs.append(d)
    assert rc == 1, rc
    head(s[0], 0, 0, 0, 'FAIL')

    # Parallel pool: mixed outcomes are counted exactly as in a serial run,
    # every command runs exactly once with its own TMPDIR, the pool really
    # overlaps, and an EXCLUSIVE command (named shutdown) runs alone after it.
    cmds = ''.join('sleep 0.5; echo "%d $TMPDIR" >> ran.log\n' % i for i in range(6))
    cmds += "exit 5\necho 'NOTRUN: no device'\n"
    cmds += 'echo "shutdown $(wc -l < ran.log)" >> ran.log\n'
    started = time.time()
    rc, s, d = run(cmds, jobs=4); dirs.append(d)
    elapsed = time.time() - started
    assert rc == 1, rc
    head(s[0], 9, 1, 1, 'FAIL')
    assert s[1].split('\t')[:2] == ['6', '5'], s
    assert s[2].startswith('NOTRUN\t7\t'), s
    rows = Path(d, 'ran.log').read_text().splitlines()
    pooled = [r.split() for r in rows if not r.startswith('shutdown')]
    assert sorted(int(r[0]) for r in pooled) == list(range(6)), rows
    assert len({r[1] for r in pooled}) == 6 and all('/tmp/case-' in r[1] for r in pooled), rows
    assert rows[-1].split() == ['shutdown', '6'], rows  # alone, after every pooled case
    assert elapsed < 2.5, elapsed  # six 0.5 s cases on four workers, not 3 s serial
    assert len(Path(d, 'timings.tsv').read_text().splitlines()) == 10

    # Budgets (tests/budgets.tsv, W160): the pool starts the largest budget
    # first, a command takes its declared slots (never more than the pool, so
    # a 3-slot command and two 1-slot commands share 4 workers while a second
    # 3-slot one waits), and a command past its budget is listed OVERBUDGET
    # with the status unchanged.
    d = tempfile.mkdtemp(prefix='efs-summary-runner-'); dirs.append(d)
    Path(d, 'tests').mkdir()
    Path(d, 'tests/budgets.tsv').write_text(
        'command\tbudget_s\tslots\treason\n'
        'sleep 1; echo "big-a $(date +%s.%N)" >> ran.log\t100\t3\tthree daemons\n'
        'sleep 1; echo "big-b $(date +%s.%N)" >> ran.log\t90\t3\tthree daemons\n'
        'sleep 0.5; echo "late $(date +%s.%N)" >> ran.log\t0.1\t1\t\n')
    cmds = ('echo "first $(date +%s.%N)" >> ran.log\n'            # recipe order: first, no budget
            'sleep 0.5; echo "late $(date +%s.%N)" >> ran.log\n'   # budget 0.1 s: OVERBUDGET
            'sleep 1; echo "big-b $(date +%s.%N)" >> ran.log\n'
            'sleep 1; echo "big-a $(date +%s.%N)" >> ran.log\n')
    Path(d, 'cmds.txt').write_text(cmds)
    env = dict(os.environ, EFS_TEST_JOBS='4')
    started = time.time()
    r = subprocess.run([sys.executable, '-c', runner], cwd=d, env=env, timeout=60)
    elapsed = time.time() - started
    s = Path(d, 'summary.txt').read_text().splitlines()
    assert r.returncode == 0, (r.returncode, s)
    head(s[0], 4, 0, 0, 'PASS')
    assert ' OVERBUDGET 1' in s[0], s[0]
    assert len(s) == 2 and s[1].split('\t')[:2] == ['OVERBUDGET', '1'] and s[1].split('\t')[3] == '0.1', s
    ends = {}
    for row in Path(d, 'ran.log').read_text().splitlines():
        name, at = row.split(); ends[name] = float(at)
    # big-a (budget 100) ends about 1 s before big-b (budget 90), which could
    # not start beside it on four workers; the 1-slot commands ran beside big-a.
    assert ends['big-b'] - ends['big-a'] > 0.7, ends
    assert ends['late'] < ends['big-b'] and ends['first'] < ends['big-b'], ends
    assert 1.9 < elapsed < 3.5, elapsed  # two 1 s commands in turn, not four workers at once
    tsv = Path(d, 'timings.tsv').read_text().splitlines()
    assert tsv[0] == 'index\tseconds\trc\tcommand\tbudget' and len(tsv) == 5, tsv
    assert {l.split('\t')[4] for l in tsv[1:]} == {'100', '90', '0.1', ''}, tsv
finally:
    for d in dirs: shutil.rmtree(d, ignore_errors=True)
print('make_test_summary runner: pass 0, failure 1, timeout 1, incomplete 3, empty 1, parallel pool exactly-once/TMPDIR/exclusive, budgets largest-first/slots/OVERBUDGET, full failure logs PASS')
