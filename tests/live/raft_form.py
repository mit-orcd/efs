"""Form a private test cluster with `efs-mgmt raft-mkfs`, as devops start.sh does.

Since W153 formation commits the joined membership through group 0, which
needs a leader. Right after the members join there may be none yet, and
raft-mkfs answers NOT_PRIMARY (rc=-15). That one answer is retried within a
bounded deadline; every other failure is final.
"""
import re
import subprocess
import time

NOT_PRIMARY = re.compile(r'raft-mkfs rc=-15\b')


def form(mgmt, addr, env=None, deadline_s=60, quiet=False):
    """Run raft-mkfs against addr until it succeeds; return its output."""
    deadline = time.monotonic() + deadline_s
    while True:
        result = subprocess.run([str(mgmt), 'raft-mkfs', str(addr)], env=env,
                                capture_output=True, text=True, timeout=60)
        out = result.stdout + result.stderr
        if not quiet:
            print(out.strip(), flush=True)
        if result.returncode == 0 and 'raft-mkfs rc=0 ' in out:
            return out
        if NOT_PRIMARY.search(out) and time.monotonic() < deadline:
            time.sleep(0.5)
            continue
        raise RuntimeError(f'raft-mkfs {addr} failed (rc {result.returncode}): {out.strip()}')
