#!/usr/bin/env python3
"""Production session verdict and Raft advancement for first-owner REGISTER."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = Path(os.environ.get('EFS_SESSION_APPLY_SOURCE', root / 'src/server/raft_host.c')).read_text()
a = s.index('    if (cmd[1] ==', s.index('    if (required_failure)'))
b = s.index('\n}', a)
verdict = s[a:b]
a = s.index('    if (cmd[0] == EFS_MD_CMD_SESSION && clen >= 2 &&', s.index('static int host_apply('))
b = s.index('    /* Infrastructure failures are never replicated business verdicts.', a)
advance = s[a:b]
code = r'''
#include "efs/common.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdio.h>
static int audible(unsigned kind, int rc) {
    uint8_t cmd[2] = {EFS_MD_CMD_SESSION, kind};
    uint64_t index = 1; efs_ino_t ino = 42; uint32_t shard = 1;
''' + verdict + r'''
}
static int core(unsigned kind, int rc) {
    uint8_t cmd[2] = {EFS_MD_CMD_SESSION, kind};
    uint32_t clen = sizeof(cmd); int ret = rc;
''' + advance + r'''
    return ret;
}
int main(void) {
    const int business[] = {EFS_OK, EFS_ERR_NOT_FOUND, EFS_ERR_INVAL,
                            EFS_ERR_BUSY, EFS_ERR_STALE};
    const unsigned admissions[] = {EFS_MD_SESS_REGISTER, EFS_MD_SESS_ESTABLISH, EFS_MD_SESS_REGISTER_ESTABLISH};
    for (unsigned k = 0; k < sizeof(admissions)/sizeof(admissions[0]); k++)
    for (unsigned i = 0; i < sizeof(business)/sizeof(business[0]); i++) {
        assert(audible(admissions[k], business[i]) == business[i]);
        assert(core(admissions[k], business[i]) == EFS_OK);
    }
    const int storage[] = {EFS_ERR_IO, EFS_ERR_NOMEM};
    for (unsigned k = 0; k < sizeof(admissions)/sizeof(admissions[0]); k++)
    for (unsigned i = 0; i < sizeof(storage)/sizeof(storage[0]); i++) {
        assert(audible(admissions[k], storage[i]) == storage[i]);
        assert(core(admissions[k], storage[i]) == storage[i]);
    }
    /* Existing idempotent CREATE handling is deliberately unchanged. */
    assert(audible(EFS_MD_SESS_CREATE, EFS_ERR_NOT_FOUND) == EFS_OK);
    assert(audible(EFS_MD_SESS_FENCE_LOC, EFS_ERR_BUSY) == EFS_ERR_BUSY);
    assert(core(EFS_MD_SESS_FENCE_LOC, EFS_ERR_BUSY) == EFS_OK);
    puts("REGISTER apply: missing-owner/refusal verdicts remain audible, business results advance Raft, storage failures retry PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-register-verdict-') as td:
    path = Path(td)/'test.c'; path.write_text(code)
    binary = path.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root/'include'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
