#!/usr/bin/env python3
"""W149: a build without EFS_FAULTS has no fault hooks. efsd and efs-fuse
name every EFS_FAULT_* variable in their environment as ignored at startup, so a fault run on a normal build
cannot pass silently without its fault. The call sits under #if !EFS_FAULTS
in each main; an EFS_FAULTS build prints nothing. The hook itself lives
only behind EFS_FAULTS (K24): src/kv/kv_compact.c reads
EFS_FAULT_COMPACT_DIE inside its #if EFS_FAULTS block and nowhere else. The
removed EFS_KV_COMPACT_DIE is refused, not ignored (A6,
tests/test_env_removed.py).

Usage: make libefs.a && python3 tests/test_faults_env_ignored.py"""
from pathlib import Path
import os, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[1]
if not (root / 'libefs.a').exists():
    sys.exit('build libefs.a first (make libefs.a)')
for f, prog in (('src/server/efsd.c', 'efsd'), ('src/client/efs_fuse.c', 'efs-fuse')):
    s = (root / f).read_text()
    m = s[s.index('int main(int argc, char **argv)'):]
    assert '#if !EFS_FAULTS\n    efs_faults_env_ignored("%s");\n#endif' % prog in m, f
k = (root / 'src/kv/kv_compact.c').read_text()
a, b = k.index('#if EFS_FAULTS'), k.index('#else\n#define compact_maybe_die()')
assert 'getenv("EFS_FAULT_COMPACT_DIE")' in k[a:b]
assert k.count('COMPACT_DIE"') == 1 and 'EFS_KV_COMPACT_DIE' not in k
code = r'''
#include "efs/common.h"
#include <stdio.h>
int main(void) { printf("n=%u\n", efs_faults_env_ignored("efsd")); return 0; }
'''
with tempfile.TemporaryDirectory(prefix='efs-faults-env-') as d:
    p = Path(d) / 't.c'
    p.write_text(code)
    exe = str(p.with_suffix(''))
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'),
                    str(p), str(root / 'libefs.a'), '-pthread', '-lm', '-o', exe], check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('EFS_FAULT', 'EFS_KV_'))}
    r = subprocess.run([exe], env=env, capture_output=True, text=True, check=True)
    assert r.stdout == 'n=0\n' and r.stderr == '', r
    env.update(EFS_FAULT_COMPACT_DIE='1', EFS_KV_COMPACT_DIE='2', EFS_FAULT_DRAIN_FILE='/tmp/x',
               EFS_FAULTS='1', EFS_KV_COMPACT_DIEX='3')
    r = subprocess.run([exe], env=env, capture_output=True, text=True, check=True)
    lines = sorted(r.stderr.splitlines())
    assert r.stdout == 'n=2\n', r
    want = sorted('efsd: %s ignored: this build has no fault hooks (build with EFS_FAULTS=1 for fault tests)' % v
                  for v in ('EFS_FAULT_COMPACT_DIE', 'EFS_FAULT_DRAIN_FILE'))
    assert lines == want, lines
print('W149 fault variables: normal build names each EFS_FAULT_* as ignored; hook only behind EFS_FAULTS PASS')
