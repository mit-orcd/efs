#!/usr/bin/env python3
"""Exact 20k APPEND/fsync gate on the deployed preserved NUC cluster."""
import argparse,hashlib,json,os,subprocess,sys,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011')
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--count',type=int,default=20000);p.add_argument('--name',default='w52-resume');p.add_argument('--after',choices=('meta-placement','batch-install'),default='meta-placement')
a=p.parse_args();assert a.count in (400,2000,20000) and a.name.replace('-','').isalnum()
out=base/a.name;out.mkdir(exist_ok=False)
installed=Path('/data1/efs/src');end=time.monotonic()+14400
ready=base/a.after/('restore-exit.txt' if a.after=='meta-placement' else 'run-exit.txt')
while not ready.exists():
    assert time.monotonic()<end,'metadata placement restoration timeout'
    time.sleep(1)
assert ready.read_text().strip()=='0'
if a.after=='meta-placement':assert (base/'meta-placement/tests-exit.txt').read_text().strip()=='0'
identity_path=base/('deployment-20261011/installed-images.json' if a.after=='meta-placement' else 'batch-install/images.json')
expected=json.loads(identity_path.read_text())
for name,identity in expected.items():
    assert hashlib.sha256((installed/name).read_bytes()).hexdigest()==identity['sha256']
assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
primary=Path('/data1/efs/mnt')/(a.name+'-20261011');primary.mkdir(exist_ok=False)
peer=Path('/data1/efs/mnt2')/primary.name
sys.path.insert(0,str(installed/'tests/live'))
import append_burst
try:
    path,report=append_burst.run(primary,peer,a.count,out)
    # Warm two-mount byte verification is inside run(); no cold/restart or
    # power-loss acceptance is inferred from that success.
    (out/'run-exit.txt').write_text('0\n')
    print(a.count,'exact 4 KiB records, fsync and two-mount bytes PASS',flush=True)
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());(out/'run-exit.txt').write_text('1\n');raise
