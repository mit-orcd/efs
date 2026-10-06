#!/usr/bin/env python3
"""A losing simple UNLINK must propagate its apply verdict, not just its index."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = (root / 'src/server/raft_host.c').read_text()
def function(name):
    match = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', text, re.M)
    if not match: raise RuntimeError(name)
    return text[match.start():text.index('\n}', match.end()) + 2]
unlink = text[text.index('void server_raft_host_unlink('):]
unlink = unlink[:unlink.index('\n}')]
wait = re.search(r'rc = (host_wait_\w+)\(h, dg, idx, term, &hint\);', unlink).group(1)
source = r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#define HOST_APPLY_RC_MASK 7
struct host_group { int hosted; uint64_t arc_idx[8], arc_term[8]; int arc_rc[8]; };
struct efs_raft_host { pthread_mutex_t mu; struct host_group group; uint64_t obs_arc_miss, obs_arc_term_miss; };
static struct host_group *group_slot(struct efs_raft_host *h, uint8_t group) { (void)group; return &h->group; }
static int host_wait_applied(struct efs_raft_host *h, uint8_t group, uint64_t idx, int *hint) {
    (void)h; (void)group; (void)idx; (void)hint; return EFS_OK;
}
''' + function('host_apply_rc_locked') + '\n' + function(wait) + r'''
int main(void) {
    struct efs_raft_host h = {.mu = PTHREAD_MUTEX_INITIALIZER};
    h.group.hosted = 1; h.group.arc_idx[3] = 11; h.group.arc_term[3] = 7;
    int verdicts[] = {EFS_OK, EFS_ERR_NOT_FOUND, EFS_ERR_BUSY, EFS_ERR_STALE};
    for (unsigned i = 0; i < sizeof(verdicts)/sizeof(verdicts[0]); ++i) {
        h.group.arc_rc[3] = verdicts[i];
        assert(WAIT(&h, 0, 11, 7, NULL) == verdicts[i]);
    }
    assert(WAIT(&h, 0, 11, 8, NULL) == EFS_ERR_NOT_PRIMARY);
    assert(WAIT(&h, 0, 19, 7, NULL) == EFS_ERR_BUSY);
    h.group.hosted = 0; assert(WAIT(&h, 0, 19, 7, NULL) == EFS_OK);
    puts("simple unlink: rejected apply, term mismatch, overwritten verdict and forwarding PASS");
}
'''.replace('WAIT(', wait + '(')
with tempfile.TemporaryDirectory(prefix='efs-unlink-verdict-') as directory:
    path = Path(directory) / 'test.c'; path.write_text(source)
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(root / 'include')]
    subprocess.run(command + [str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
