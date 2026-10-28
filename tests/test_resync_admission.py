#!/usr/bin/env python3
"""Real Raft pre-admission fence, bounded admin retry, failed-evidence retention."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import resync_admission as m
root = Path(__file__).resolve().parents[1]
source = (root / 'src/raft/raft.c').read_text()
a = source.index('int efs_raft_request_resync(')
body = source[a:source.index('\n}', a) + 2]
code = r'''
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efs_raft { int recv_persisting,role,leader,resync_requested;
    unsigned group;
    uint64_t prevote_term,prevote_bits,vote_bits,commit_index,last_applied; };
static unsigned forgets,resets;
static void read_proof_forget(struct efs_raft*r){(void)r;forgets++;}
static void reset_election(struct efs_raft*r){(void)r;resets++;}
''' + body + r'''
int main(void){
 struct efs_raft r,before;memset(&r,0,sizeof(r));
 assert(EFS_ERR_BUSY==-13);r.role=EFS_RAFT_FOLLOWER;r.recv_persisting=1;
 memcpy(&before,&r,sizeof(r));assert(efs_raft_request_resync(&r)==EFS_ERR_BUSY);
 assert(!memcmp(&r,&before,sizeof(r)) && !forgets && !resets);
 r.recv_persisting=0;assert(efs_raft_request_resync(&r)==EFS_OK);
 assert(r.resync_requested && forgets==1 && resets==1);
 r=(struct efs_raft){0};r.role=EFS_RAFT_LEADER;
 assert(efs_raft_request_resync(&r)==EFS_ERR_NOT_PRIMARY && !r.resync_requested);
}
'''
with tempfile.TemporaryDirectory(prefix='efs-resync-fence-') as work:
    p = Path(work) / 'test.c';p.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'include'),
                    str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)


def run(replies, limit=1, malformed=False):
    clock = [0.0];calls = []
    def invoke(argv, **kw):
        assert kw['timeout'] <= limit - clock[0]
        calls.append(argv)
        rc = replies[min(len(calls)-1, len(replies)-1)]
        suffix = ' (requested; the next conflicting AppendEntries asks for a snapshot)' if rc == 0 else \
                 ' (this replica is not eligible for resync: leader or retired; refused)' if rc == -15 else ''
        return subprocess.CompletedProcess(argv, 0 if rc == 0 else 1,
                                           'invalid reply\n' if malformed else
                                           f'raft-resync 127.0.0.1:1 group=2 rc={rc}{suffix}\n', '')
    def sleep(n):
        clock[0] += n
    with patch.object(m.subprocess, 'run', invoke), patch.object(m.time, 'monotonic', lambda: clock[0]), \
            patch.object(m.time, 'sleep', sleep):
        try:
            out = m.request_resync('mgmt', '127.0.0.1:1', 2, {}, limit)
        except AssertionError as e:
            return str(e), calls, clock[0]
    return out, calls, clock[0]

out, calls, elapsed = run([-13, 0])
assert 'rc=0' in out and len(calls) == 2 and elapsed == .05
out, calls, elapsed = run([-15, 0])
assert 'rejected' in out and len(calls) == 1 and elapsed == 0
out, calls, elapsed = run([-13], limit=.12)
assert 'deadline' in out and elapsed == .12 and len(calls) == 3
out, calls, elapsed = run([0], malformed=True)
assert 'invalid reply' in out and len(calls) == 1

# Execute the actual D4 finally block: a failed run retains its original
# exception and fixture; a successful run prunes unless explicitly kept.
tree = ast.parse((root/'tests/test_d4_plateau.py').read_text())
final = [n for n in tree.body if isinstance(n, ast.Try) and n.finalbody][-1].finalbody
class FailedRun(Exception):
    pass
class Cluster:
    def stop_all(self):
        pass
for failure, keep in ((False, False), (True, False), (False, True)):
    with tempfile.TemporaryDirectory(prefix='efs-d4-retain-') as work:
        p = Path(work)/'fixture';p.mkdir();(p/'evidence').write_text('original')
        body = ast.parse('raise FailedRun()' if failure else 'pass').body
        mod = ast.fix_missing_locations(ast.Module(body=[ast.Try(body=body, handlers=[], orelse=[], finalbody=final)], type_ignores=[]))
        with patch.dict(os.environ, {'EFS_D4_KEEP': '1'} if keep else {}, clear=True):
            try:
                exec(compile(mod, '<D4 production cleanup>', 'exec'),
                     dict(mounts=[], c=Cluster(), os=os, sys=sys, shutil=shutil, work=p, FailedRun=FailedRun))
                assert not failure
            except FailedRun:
                assert failure
        assert p.exists() == (failure or keep)
print('resync admission: real pre-effect BUSY, bounded retry, other-error refusal; D4 failed fixture retention PASS')
