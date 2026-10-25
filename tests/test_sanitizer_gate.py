#!/usr/bin/env python3
"""Self-test for `make tsan-check` / `make asan-check` (tests/sanitizer_check.sh, F1).

A racy fixture makes the tsan gate FAIL with its report shown, a clean
threaded one PASS; a heap overflow, a signed-overflow UB and (where
LeakSanitizer can run) a leak make the asan gate FAIL; a failing test FAILs
without any report; an empty selection FAILs; a compiler that cannot build a
sanitized program gives NOTRUN exit 3; VAR=value prefixes reach the entry
and a .py entry runs under python3 with the harness switch set. Fixtures
are prebuilt here and named through EFS_SAN_LIST, so no tree is built.
"""
from pathlib import Path
import os, platform, shlex, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
gate = root / 'tests/sanitizer_check.sh'
cc = shlex.split(os.environ.get('CC', 'cc'))

RACE = r'''
#include <pthread.h>
#include <stdio.h>
static int counter;
static void *inc(void *a) { (void)a; for (int i = 0; i < 100000; i++) counter++; return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, inc, 0); pthread_create(&b, 0, inc, 0);
    pthread_join(a, 0); pthread_join(b, 0); printf("%d\n", counter); return 0; }
'''
CLEAN = r'''
#include <pthread.h>
#include <stdio.h>
static int counter; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *inc(void *a) { (void)a; for (int i = 0; i < 1000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); } return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, inc, 0); pthread_create(&b, 0, inc, 0);
    pthread_join(a, 0); pthread_join(b, 0); printf("%d\n", counter); return 0; }
'''
OVERFLOW = r'''
#include <stdlib.h>
#include <string.h>
/* The size is opaque so UBSan's object-size check (gcc) does not fire
 * first and the overflow reaches AddressSanitizer. */
int main(int argc, char **argv) { (void)argv; volatile size_t sz = 16; char *p = malloc(sz);
    volatile char *v = p; v[sz + argc - 1] = 1; int r = v[sz + argc - 1]; free(p); return r != 1; }
'''
UB = r'''
#include <limits.h>
#include <stdio.h>
int main(int argc, char **argv) { (void)argv; volatile int x = INT_MAX; x += argc; printf("%d\n", x); return 0; }
'''
LEAK = r'''
#include <stdlib.h>
#include <string.h>
static void *volatile keep; /* gcc 15 -O1 elides a malloc whose pointer only reaches a plain dead store */
int main(void) { char *p = malloc(48); memset(p, 1, 48); keep = p; keep = 0; return 0; }
'''
ENVCHK = r'''
#include <stdlib.h>
#include <string.h>
int main(void) { const char *v = getenv("EFS_SAN_FIXTURE_ENV"); return v && !strcmp(v, "1") ? 0 : 4; }
'''
FAILING = 'int main(void) { return 5; }\n'
# A Python harness: it must see the mode's switch (tests/sanitize_env.py).
DRIVER = '''
import os, sys
want = {'tsan': 'EFS_TEST_TSAN', 'asan': 'EFS_TEST_SANITIZE'}[sys.argv[1]]
sys.exit(0 if os.environ.get(want) == '1' else 7)
'''


def build(d, name, src, flags):
    c = d / (name + '.c'); c.write_text(src)
    r = subprocess.run(cc + ['-g', '-O1', '-pthread', *flags, str(c), '-o', str(d / name)],
                       capture_output=True, text=True)
    return (d / name) if r.returncode == 0 else None


def run_gate(d, mode, entries, **env):
    lst = d / ('list-' + mode + '-' + entries[0][0]); lst.write_text(''.join(f'{n}|{c}\n' for n, c in entries))
    e = dict(os.environ, EFS_SAN_LIST=str(lst), EFS_SAN_TIMEOUT='120',
             EFS_SAN_LOGDIR=str(d / ('logs-' + lst.name)), CC=os.environ.get('CC', 'cc'))
    e.update(env)
    r = subprocess.run(['bash', str(gate), mode], capture_output=True, text=True, env=e)
    return r.returncode, r.stdout


def lsan_possible():
    if platform.system() != 'Linux':
        return False
    try:
        return int(Path('/proc/sys/kernel/yama/ptrace_scope').read_text()) < 2
    except OSError:
        return True


with tempfile.TemporaryDirectory(prefix='efs-san-gate-') as td:
    d = Path(td)
    rc, out = run_gate(d, 'nomode', [('x', '/bin/true')])
    assert rc == 2, (rc, out)

    # A compiler that cannot build a sanitized program -> NOTRUN exit 3, never 0.
    for mode in ('tsan', 'asan'):
        rc, out = run_gate(d, mode, [('x', '/bin/true')], CC='/bin/false')
        assert rc == 3 and out.startswith(f'NOTRUN: {mode} runtime missing'), (mode, rc, out)
    print('sanitizer gate: runtime missing -> NOTRUN exit 3 PASS')

    notrun = []
    for mode, flags in (('tsan', ['-fsanitize=thread']), ('asan', ['-fsanitize=address,undefined'])):
        clean = build(d, 'clean-' + mode, CLEAN, flags)
        if clean is None or subprocess.run([str(clean)], capture_output=True).returncode != 0:
            notrun.append(mode)
            continue
        failing = build(d, 'failing-' + mode, FAILING, flags)
        envchk = build(d, 'envchk-' + mode, ENVCHK, flags)
        drv = d / 'drv.py'; drv.write_text(DRIVER)

        rc, out = run_gate(d, mode, [('clean', str(clean)), ('env', f'EFS_SAN_FIXTURE_ENV=1 {envchk}'),
                                     ('driver', f'{drv} {mode}')])
        assert rc == 0 and 'PASS clean' in out and 'PASS env' in out and 'PASS driver' in out \
            and 'VERDICT: PASS (3 run, 0 failed' in out, (mode, rc, out)
        rc, out = run_gate(d, mode, [('failing', str(failing)), ('clean', str(clean))])
        assert rc == 1 and 'FAIL failing: test rc=5' in out and 'PASS clean' in out \
            and 'VERDICT: FAIL (2 run, 1 failed' in out, (mode, rc, out)
        rc, out = run_gate(d, mode, [('clean', str(clean))], EFS_SAN_ONLY='nothing')
        assert rc == 1 and 'VERDICT: FAIL (no entries selected)' in out, (mode, rc, out)
        print(f'sanitizer gate ({mode}): clean, env prefix and .py driver PASS; failing test and empty selection FAIL PASS')

        if mode == 'tsan':
            race = build(d, 'race', RACE, flags)
            rc, out = run_gate(d, mode, [('race', str(race)), ('clean', str(clean))])
            assert rc == 1 and 'FAIL race: ' in out and 'ThreadSanitizer reports' in out and 'PASS clean' in out, (rc, out)
            assert 'WARNING: ThreadSanitizer: data race' in out and 'inc' in out, 'report not shown: ' + out
            print('sanitizer gate (tsan): racy fixture -> FAIL with the report, clean sibling PASS')
        else:
            over = build(d, 'overflow', OVERFLOW, ['-fsanitize=address'])  # UBSan's object-size check must not get there first
            ub = build(d, 'ub', UB, flags)
            rc, out = run_gate(d, mode, [('overflow', str(over)), ('ub', str(ub)), ('clean', str(clean))])
            assert rc == 1 and 'FAIL overflow: 1 AddressSanitizer errors' in out, (rc, out)
            assert 'FAIL ub: 1 UBSan errors' in out and 'PASS clean' in out and 'VERDICT: FAIL (3 run, 2 failed' in out, out
            assert 'heap-buffer-overflow' in out, 'report not shown: ' + out
            print('sanitizer gate (asan): heap overflow and signed overflow -> FAIL with the report, clean sibling PASS')
            if lsan_possible():
                leak = build(d, 'leak', LEAK, flags)
                rc, out = run_gate(d, mode, [('leak', str(leak))])
                assert rc == 1 and 'FAIL leak: 1 LeakSanitizer reports' in out and 'malloc' in out, (rc, out)
                print('sanitizer gate (asan): leaking fixture -> FAIL (LeakSanitizer) PASS')
            else:
                print('sanitizer gate (asan): LeakSanitizer case not run here (no ptrace or not Linux); '
                      'the AddressSanitizer and UBSan cases above are the gate')
    if notrun:
        print('sanitizer gate: real fixtures NOTRUN for %s (this compiler cannot build or run a sanitized program)' % ', '.join(notrun))
print('sanitizer gate self-test PASS')
