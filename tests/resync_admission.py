"""Bounded operator resync admission, not retries of application operations.

efs_raft_request_resync returns BUSY before any effect while a receive
persistence cohort is active. Share the caller's existing recovery deadline.
"""
import re
import subprocess
import time

EFS_ERR_BUSY = -13  # include/efs/common.h, checked by the real-guard test

def request_resync(binary, endpoint, group, env, deadline):
    attempt = 0
    while True:
        left = deadline - time.monotonic()
        assert left > 0, 'raft-resync admission remained BUSY until recovery deadline'
        p = subprocess.run([str(binary), 'raft-resync', endpoint, str(group)],
                           env=env, text=True, capture_output=True, timeout=min(60, left))
        out = p.stdout + p.stderr
        reply = re.search(r'^raft-resync ' + re.escape(endpoint) + r' group=' +
                          re.escape(str(group)) + r' rc=(-?\d+)(?: \([^\r\n]*\))?\s*$', out, re.M)
        assert reply, f'raft-resync invalid reply/transport failure: exit={p.returncode}: {out}'
        rc = int(reply[1])
        if rc == 0 and p.returncode == 0:
            return out
        assert rc == EFS_ERR_BUSY and p.returncode == 1, f'raft-resync rejected: {out}'
        attempt += 1
        print(f'raft-resync admission BUSY (attempt {attempt}, no request accepted): {out.strip()}', flush=True)
        left = deadline - time.monotonic()
        assert left > 0, 'raft-resync admission remained BUSY until recovery deadline'
        time.sleep(min(.05, left))
