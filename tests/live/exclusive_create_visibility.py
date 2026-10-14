#!/usr/bin/env python3
"""Repeat stock two-process O_EXCL/close visibility; retain every failed file.

No fsync/retries are added to the original POSIX test. Run on an active mount
with the required profile; successful cases remove only their own fixtures.
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
a=p.parse_args()
if not os.path.ismount(a.mount):p.error('active private mount required')
if not 1<=a.cases<=10000 or not 1<=a.workers<=64:p.error('invalid cases/workers')
root=Path(tempfile.mkdtemp(prefix='exclusive-visibility-',dir=a.mount));started=time.monotonic();durations=[]
def one(i):
    d=root/str(i);d.mkdir();case_started=time.monotonic()
    try:
        posix_suite.create_excl_two_proc(str(d))
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
print(json.dumps(dict(result='FAIL' if failures else 'PASS',cases=a.cases,passed=a.cases-len(failures),workers=a.workers,seconds=time.monotonic()-started,max_case_s=max(durations),fixture=str(root),failures=failures)),flush=True)
raise SystemExit(bool(failures))
