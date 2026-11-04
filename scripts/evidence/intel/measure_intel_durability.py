#!/usr/bin/env python3
"""Finite files-only scratch experiment using the existing efs-bench binary.
Run locally on an Intel host, never against an EFS storage directory or mount.
The caller creates a dedicated empty phase directory; every case uses a new child.
No daemon settings/processes are modified. Benchmark performs hash verification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('binary',type=Path)
ap.add_argument('scratch',type=Path)
ap.add_argument('phase',choices=['loaded-before','quiet','stat-only','loaded-after'])
a=ap.parse_args()
if not a.scratch.is_dir() or list(a.scratch.iterdir()):
    ap.error('scratch must exist and be empty')
if a.scratch.is_symlink():ap.error('scratch cannot be a symlink')
binary=a.binary.resolve(strict=True)
print(json.dumps(dict(kind='identity',phase=a.phase,binary=str(binary),
    sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),host=os.uname().nodename,
    scratch=str(a.scratch.resolve()),time=time.time())),flush=True)
orders={'loaded-before':[[1,4,8,16],[16,8,4,1]],
        'quiet':[[16,8,4,1],[1,4,8,16]],
        'loaded-after':[[4,16],[16,4]], 'stat-only':[[4,16],[16,4]]}
cases=[(f'each-q{qd}-r{rep}',qd,'each','allocating',1)
       for rep,order in enumerate(orders[a.phase]) for qd in order]
if a.phase in ('loaded-before','quiet'): cases += [('batch16-q4',4,'batch','allocating',16),
          ('overwrite-q4',4,'each','overwrite',1),
          ('acceptance-only-q4',4,'none','allocating',1)]
for name,qd,persist,policy,batch in cases:
    root=a.scratch/name;root.mkdir()
    cmd=[str(binary),'--bench','principles','--storage',str(root),
        '--rw','write','--policy',policy,'--persist',persist,
        '--directory-sync','worker','--objects','4096','--payload-size','65536',
        '--qd',str(qd),'--batch-size',str(batch),'--direct-io']
    before=Path('/proc/diskstats').read_text();start=time.time()
    p=subprocess.run(cmd,text=True,capture_output=True,timeout=180)
    end=time.time();after=Path('/proc/diskstats').read_text()
    match=next((l for l in p.stdout.splitlines() if l.startswith('BENCH_OK ')),None)
    fields=dict(re.findall(r'(\w+)=([^\s]+)',match or ''))
    empty=not list(root.iterdir())
    ok=(p.returncode==0 and fields.get('errors')=='0' and
        fields.get('verified_objects')=='4096' and empty)
    print(json.dumps(dict(kind='case',phase=a.phase,case=name,command=cmd,
        start=start,end=end,returncode=p.returncode,pass_checks=ok,empty=empty,
        fields=fields,stdout=p.stdout,stderr=p.stderr,
        disks_before=before,disks_after=after)),flush=True)
    if not ok:raise SystemExit('Benchmark failed; preserving remaining scratch for inspection')
    root.rmdir()
print(json.dumps(dict(kind='complete',phase=a.phase,time=time.time())),flush=True)
