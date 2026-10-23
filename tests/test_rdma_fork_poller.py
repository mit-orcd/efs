#!/usr/bin/env python3
"""K31: the mount session must start after the daemon fork, and an RDMA
device must refuse use from a process that does not own its poller.

Found on orcd-mini (Oct 23): K5 moved the mount session RPC before
efs-fuse's daemon fork. Over RDMA that first RPC starts the device's
completion poller thread; fork() keeps only the calling thread, so the
serving daemon had a device marked "poller started" and no poller. Replies
were delivered to the receive CQ (no HCA error counter moved) and never
pushed to a connection: every LOOKUP/GETATTR timed out, mkdir returned EIO.
TCP has no poller, so only RDMA mounts failed. RDMA hardware is absent on
most test hosts, so this is a source-order check plus the fail-closed guard.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
fuse = (root / 'src/client/efs_fuse.c').read_text()
rdma = (root / 'src/common/rdma.c').read_text()

def body(src, sig):
    """Function text from its signature to its closing brace at column 0."""
    i = src.index(sig + '\n{')  # the definition, not a forward declaration
    return src[i:src.index('\n}\n', i) + 3]

# 1. Exactly one session start, inside the serving function, after the fork.
calls = [m.start() for m in re.finditer(r'efs_client_session_start\(\)', fuse)]
assert len(calls) == 1, 'expected one efs_client_session_start() call, found %d' % len(calls)
main_mt = body(fuse, 'static int efs_fuse_main_mt(int argc, char *argv[])')
assert 'efs_client_session_start()' in main_mt, 'session start must be in efs_fuse_main_mt (the serving process)'
fork_at = main_mt.index('pid_t child = fork();')
sess_at = main_mt.index('efs_client_session_start()')
assert sess_at > fork_at, 'session start must follow the daemon fork (K31)'
assert main_mt.index('setsid();') < sess_at, 'session start must follow setsid() in the child'
# ... and before the FUSE session is created, so a failure exits before FUSE_INIT.
assert sess_at < main_mt.index('fuse_session_new'), 'session start must precede fuse_session_new'
# The failure path leaves through the cleanup label, never returns from the middle.
tail = main_mt[sess_at:sess_at + 400]
assert 'goto out;' in tail and 'return 1;' not in tail, 'session failure must take the cleanup path'

# 2. The RDMA device records its poller's owner and refuses another process.
assert re.search(r'pid_t\s+poller_pid;', rdma), 'struct efs_rdma_dev needs poller_pid'
start = body(rdma, 'static int dev_shared_cq_start(struct efs_rdma_dev *dev)')
guard = start.index('dev->poller_pid != getpid()')
assert guard < start.index('ibv_create_comp_channel'), 'the fork guard must run before any reuse'
assert 'return -1;' in start[guard:guard + 900], 'a foreign process must be refused, fail closed'
assert start.index('dev->poller_pid = getpid();') < start.index('dev->poller_started = 1;'), \
    'poller_pid must be set before poller_started is published'
print('RDMA fork/poller ownership: session after daemon fork, foreign-process refusal PASS')
