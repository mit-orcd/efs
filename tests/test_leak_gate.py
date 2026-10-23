#!/usr/bin/env python3
"""Self-test for `make leak-check` (tests/leak_check.sh, K26).

A deliberately leaking fixture makes the gate FAIL, a clean one PASS, a
leak inside a Python-driven test (the EFS_LEAK_WRAP path) FAILs, a failing
test FAILs even without a leak, and valgrind absent gives NOTRUN exit 3.
A fake valgrind that writes no report must not PASS (vacuous-run guard).
The real-memcheck cases need valgrind; without it they are reported as
NOTRUN here, and the gate's own NOTRUN/no-report cases still run.
"""
from pathlib import Path
import os, shutil, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
gate = root / 'tests/leak_check.sh'
cc = os.environ.get('CC', 'cc')

LEAK = r'''
#include <stdlib.h>
#include <string.h>
static void *keep;
int main(void){
    char *p = malloc(48); memset(p, 1, 48);
    keep = p; keep = 0;            /* last pointer dropped: definitely lost */
    return 0;
}
'''
CLEAN = r'''
#include <stdlib.h>
#include <string.h>
int main(void){ char *p = malloc(48); memset(p, 1, 48); free(p); return 0; }
'''
# A forked child that _exit()s holding memory (the unit tests' crash
# simulation) must keep its exit status and must not fail the gate.
FORKING = r'''
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void){
    pid_t pid = fork();
    if (pid == 0) { char *p = malloc(64); memset(p, 2, 64); p = 0; _exit(0); }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return 9;
    return 0;
}
'''
ENVCHK = r'''
#include <stdlib.h>
#include <string.h>
int main(void){ const char *v = getenv("EFS_LEAK_FIXTURE_ENV"); return v && !strcmp(v, "1") ? 0 : 4; }
'''
FAILING = r'''
int main(void){ return 5; }
'''
# A Python driver shaped like the four former ASan tests: it builds its own
# binary and must run it through EFS_LEAK_WRAP.
DRIVER = r'''
import os, shlex, subprocess, sys
wrap = shlex.split(os.environ.get('EFS_LEAK_WRAP', ''))
subprocess.run([*wrap, sys.argv[0].replace('.py', '')], check=True)
'''


def build(d, name, src):
    c = d / (name + '.c'); c.write_text(src)
    subprocess.run([cc, '-g', '-O0', str(c), '-o', str(d / name)], check=True)
    return d / name


def run_gate(d, entries, **env):
    lst = d / 'list'; lst.write_text(''.join(f'{n}|{c}\n' for n, c in entries))
    e = dict(os.environ, EFS_LEAK_LIST=str(lst), EFS_LEAK_TIMEOUT='120',
             EFS_LEAK_LOGDIR=str(d / ('logs-' + entries[0][0])))
    e.update(env)
    r = subprocess.run(['bash', str(gate)], capture_output=True, text=True, env=e)
    return r.returncode, r.stdout


with tempfile.TemporaryDirectory(prefix='efs-leak-gate-') as td:
    d = Path(td)

    # valgrind absent -> NOTRUN, exit 3, never 0.
    rc, out = run_gate(d, [('x', '/bin/true')], EFS_VALGRIND=str(d / 'no-such-valgrind'))
    assert rc == 3 and out.strip() == 'NOTRUN: valgrind missing', (rc, out)
    print('leak gate: valgrind absent -> NOTRUN exit 3 PASS')

    # A "valgrind" that runs the program but writes no report cannot PASS.
    fake = d / 'fake-valgrind'
    fake.write_text('#!/bin/sh\nwhile [ "${1#--}" != "$1" ]; do shift; done\nexec "$@"\n')
    fake.chmod(0o755)
    rc, out = run_gate(d, [('noreport', '/bin/true')], EFS_VALGRIND=str(fake))
    assert rc == 1 and 'FAIL noreport: no memcheck report' in out and 'VERDICT: FAIL' in out, (rc, out)
    print('leak gate: run without a memcheck report -> FAIL PASS')

    # Empty selection cannot PASS either.
    rc, out = run_gate(d, [('x', '/bin/true')], EFS_VALGRIND=str(fake), EFS_LEAK_ONLY='nothing')
    assert rc == 1 and 'VERDICT: FAIL (no binaries selected)' in out, (rc, out)

    if shutil.which('valgrind') is None:
        print('leak gate: real-memcheck fixtures NOTRUN (valgrind missing on this host)')
        sys.exit(0)

    leak = build(d, 'leak', LEAK)
    clean = build(d, 'clean', CLEAN)
    failing = build(d, 'failing', FAILING)
    forking = build(d, 'forking', FORKING)
    envchk = build(d, 'envchk', ENVCHK)
    drv = d / 'pyleak.py'; drv.write_text(DRIVER); shutil.copy(leak, d / 'pyleak')

    rc, out = run_gate(d, [('clean', str(clean))])
    assert rc == 0 and 'PASS clean' in out and 'VERDICT: PASS (1 run, 0 failed' in out, (rc, out)
    print('leak gate: clean fixture -> PASS exit 0 PASS')

    rc, out = run_gate(d, [('leak', str(leak)), ('clean', str(clean))])
    assert rc == 1, (rc, out)
    assert 'FAIL leak: definite leak 48 bytes' in out, out
    assert 'PASS clean' in out and 'VERDICT: FAIL (2 run, 1 failed' in out, out
    assert 'malloc' in out and 'main' in out, 'leak stack not shown: ' + out
    print('leak gate: leaking fixture -> FAIL with stack, clean sibling still PASS')

    rc, out = run_gate(d, [('pyleak', 'py:' + str(drv))])
    assert rc == 1 and 'FAIL pyleak: definite leak 48 bytes' in out, (rc, out)
    print('leak gate: leak inside a Python-driven test (EFS_LEAK_WRAP) -> FAIL PASS')

    rc, out = run_gate(d, [('forking', str(forking))])
    assert rc == 0 and 'PASS forking' in out, (rc, out)
    print('leak gate: crash-simulation child keeps its exit status, parent gated PASS')

    rc, out = run_gate(d, [('envchk', 'EFS_LEAK_FIXTURE_ENV=1 ' + str(envchk))])
    assert rc == 0 and 'PASS envchk' in out, (rc, out)
    print('leak gate: VAR=value prefix reaches the binary under memcheck PASS')

    rc, out = run_gate(d, [('failing', str(failing))])
    assert rc == 1 and 'FAIL failing: test rc=5' in out, (rc, out)
    print('leak gate: failing test without a leak -> FAIL PASS')
