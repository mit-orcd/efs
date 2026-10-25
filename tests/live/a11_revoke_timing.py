#!/usr/bin/env python3
"""A11 finding 1 evidence: how long a clean unmount's session revoke takes.

Usage: python3 tests/live/a11_revoke_timing.py [FILES ...]   (default 1000 4000)

Run from the root of a built tree (make, make tests/faults/efsd). For each
FILES count it starts three private efsd daemons on free loopback ports
(tests/k33_daemons.py), mounts efs-fuse, creates FILES 64-byte files in
directories of 200, unmounts with `fusermount3 -u` and waits for efs-fuse
to exit. It prints one line per run:
  files=N touched=T revoke_s=S exit=RC
where revoke_s is the time between the client's "put tickets outstanding
at unmount" and "session revoked at unmount" log lines (both builds log
them with timestamps), so the same script measures the tree before and
after the fix. Work directories are deleted at the end of each run.
"""
import os, re, shutil, subprocess, sys, tempfile, time
from datetime import datetime
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from k33_daemons import Cluster, Mount, payload  # noqa: E402


def stamp(line):
    return datetime.strptime(line[:23], '%Y-%m-%dT%H:%M:%S.%f').timestamp()


def run(files):
    work = Path(tempfile.mkdtemp(prefix='efs-a11-revoke-', dir=os.environ.get('TMPDIR')))
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_')}
    c = Cluster(work, 'p', [1, 2, 3], env)
    try:
        c.start(1)
        c.start(2, join=1)
        c.start(3, join=1)
        for i in (1, 2, 3):
            c.wait_log(i, r'raft-host: formed: 3 members')
        m = Mount(c, 1, 'mnt')
        for d in range(files // 200 + 1):
            (m.path / f'd{d}').mkdir()
        for k in range(files):
            (m.path / f'd{k // 200}' / f'f{k}').write_bytes(payload(f'stop-{k}', 64))
        subprocess.run(['fusermount3', '-u', str(m.path)], check=True)
        rc = m.p.wait(timeout=600)
        log = m.logp.read_text(errors='replace').splitlines()
        settled = [l for l in log if 'put tickets outstanding at unmount' in l]
        revoked = [l for l in log if 'session revoked at unmount' in l]
        assert settled and revoked, '\n'.join(log[-30:])
        touched = re.search(r'touched=(\d+)', revoked[-1]).group(1)
        took = stamp(revoked[-1]) - stamp(settled[-1])
        print(f'files={files} touched={touched} revoke_s={took:.2f} exit={rc} :: {revoked[-1][24:]}',
              flush=True)
    finally:
        c.stop_all()
        shutil.rmtree(work, ignore_errors=True)


for n in [int(a) for a in sys.argv[1:]] or [1000, 4000]:
    run(n)
