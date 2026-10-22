#!/usr/bin/env python3
"""Repeat stock two-process O_EXCL/close visibility; retain every failed file.

No fsync/retries are added to the original POSIX test. Run on an active mount
with the required profile; successful cases remove only their own fixtures.

The stock case starts its writers with fork(). Under concurrent workers that
fork races other threads' FUSE direct-I/O reads, and on kernels without
upstream 738adade96b2 the reader can see stale bytes although efs-fuse
replied correctly (W121, docs/status/orcd-w121-fork-read-20261016.md).
--spawn starts the same two writers with posix_spawn, which does not split
the parent's pages, so it checks EFS O_EXCL/EOF visibility apart from W121.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'posix'))
import posix_suite
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('mount',type=Path)
p.add_argument('--cases',type=int,default=160)
p.add_argument('--workers',type=int,default=16)
p.add_argument('--spawn',action='store_true',help='posix_spawn writers (W121-isolated)')
a=p.parse_args()
if not os.path.ismount(a.mount):p.error('active private mount required')
if not 1<=a.cases<=10000 or not 1<=a.workers<=64:p.error('invalid cases/workers')
def create_excl_spawn(d):
    """create_excl_two_proc with posix_spawn writers; same checks."""
    path=os.path.join(d,'f')
    snippet=("import os,sys,errno\np=sys.argv[1]\ntry:\n"
             "    fd=os.open(p, os.O_CREAT|os.O_EXCL|os.O_WRONLY, 0o644)\n"
             "    os.write(fd, b'x'); os.close(fd); sys.exit(0)\n"
             "except OSError as e:\n    sys.exit(17 if e.errno==errno.EEXIST else 2)\n")
    pids=[os.posix_spawn(sys.executable,[sys.executable,'-c',snippet,path],os.environ) for _ in range(2)]
    rcs=[os.waitpid(pid,0)[1]>>8 for pid in pids]
    if sorted(rcs)!=[0,17]:raise posix_suite.Fail("O_EXCL two-proc rcs=%s (want one 0 and one 17)"%rcs)
    posix_suite.eq(posix_suite.rd(path),b"x","winner content")
root=Path(tempfile.mkdtemp(prefix='exclusive-visibility-',dir=a.mount));started=time.monotonic();durations=[]
def one(i):
    d=root/str(i);d.mkdir();case_started=time.monotonic()
    try:
        (create_excl_spawn if a.spawn else posix_suite.create_excl_two_proc)(str(d))
        if time.monotonic()-case_started>30:raise posix_suite.Fail("stock case budget exceeded: 30s")
        shutil.rmtree(d)
        return None
    except Exception as exc:
        row=dict(case=i,directory=str(d),error=repr(exc))
        try:
            st=(d/'f').stat();row.update(ino=st.st_ino,size=st.st_size,payload=(d/'f').read_bytes().hex())
        except OSError as err:row['inspection_error']=repr(err)
        return row
    finally:durations.append(time.monotonic()-case_started)
with ThreadPoolExecutor(a.workers) as pool:failures=[r for r in pool.map(one,range(a.cases)) if r]
if not failures:root.rmdir()
print(json.dumps(dict(result='FAIL' if failures else 'PASS',writers='posix_spawn' if a.spawn else 'fork',cases=a.cases,passed=a.cases-len(failures),workers=a.workers,seconds=time.monotonic()-started,max_case_s=max(durations),fixture=str(root),failures=failures)),flush=True)
raise SystemExit(bool(failures))
