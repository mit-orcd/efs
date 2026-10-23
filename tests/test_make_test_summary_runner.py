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
import os, shutil, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'scripts/make_test_summary.sh').read_text()
runner = source.split('python3 - <<"EOP"\n', 1)[1].rsplit('\nEOP', 1)[0]


def run(cmds, timeout=None):
    """Run the runner on cmds; return (status, summary lines, directory)."""
    d = tempfile.mkdtemp(prefix='efs-summary-runner-')
    Path(d, 'cmds.txt').write_text(cmds)
    env = dict(os.environ)
    if timeout is not None:
        env['EFS_TEST_CMD_TIMEOUT'] = str(timeout)
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

    # NOTRUN with everything passing is INCOMPLETE, never PASS.
    rc, s, d = run("true\necho 'NOTRUN: no RDMA device'\n"); dirs.append(d)
    assert rc == 3, rc
    head(s[0], 2, 0, 1, 'INCOMPLETE')
    assert s[1] == "NOTRUN\t1\techo 'NOTRUN: no RDMA device'\tNOTRUN: no RDMA device", s

    # No commands at all is not a pass.
    rc, s, d = run("\n"); dirs.append(d)
    assert rc == 1, rc
    head(s[0], 0, 0, 0, 'FAIL')
finally:
    for d in dirs: shutil.rmtree(d, ignore_errors=True)
print('make_test_summary runner: pass 0, failure 1, timeout 1, NOTRUN 3, empty 1, full failure logs PASS')
