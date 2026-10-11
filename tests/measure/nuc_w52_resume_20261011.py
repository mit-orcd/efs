#!/usr/bin/env python3
"""Exact 20k APPEND/fsync gate on the deployed preserved NUC cluster."""
import argparse,hashlib,json,os,subprocess,sys,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011')
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--count',type=int,default=20000);p.add_argument('--name',default='w52-resume');p.add_argument('--after',choices=('meta-placement','batch-install','clock-install'),default='meta-placement');p.add_argument('--before-2000',action='store_true',help='Run a separate identical 2,000-record preserved-store check before the long gate.')
a=p.parse_args();assert a.count in (400,2000,20000) and a.name.replace('-','').isalnum()
assert not a.before_2000 or (a.count==20000 and a.after.endswith('-install'))
out=base/a.name;out.mkdir(exist_ok=False)
installed=Path('/data1/efs/src');end=time.monotonic()+14400
ready=base/a.after/('restore-exit.txt' if a.after=='meta-placement' else 'run-exit.txt')
while not ready.exists():
    assert time.monotonic()<end,'metadata placement restoration timeout'
    phases=(a.after,) if a.after=='meta-placement' else tuple(a.after.split('-')[0]+'-'+v for v in ('build','live','install'))
    failed=[v for v in phases if (base/v/'failure.txt').exists()]
    if failed:
        (out/'blocked-prerequisite.json').write_text(json.dumps(dict(status='not run',no_workload_created=True,failed=failed),indent=2))
        (out/'run-exit.txt').write_text('125\n');sys.exit(125)
    time.sleep(1)
assert ready.read_text().strip()=='0'
if a.after=='meta-placement':assert (base/'meta-placement/tests-exit.txt').read_text().strip()=='0'
identity_path=base/('deployment-20261011/installed-images.json' if a.after=='meta-placement' else a.after+'/images.json')
expected=json.loads(identity_path.read_text())
for name,identity in expected.items():
    assert hashlib.sha256((installed/name).read_bytes()).hexdigest()==identity['sha256']
assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
primary=Path('/data1/efs/mnt')/(a.name+'-20261011');primary.mkdir(exist_ok=False)
peer=Path('/data1/efs/mnt2')/primary.name
sys.path.insert(0,str(installed/'tests/live'))
import append_burst
try:
    if a.before_2000:
        short=out/'before-2000';short.mkdir()
        short_primary=Path('/data1/efs/mnt')/(a.name+'-before-2000-20261011');short_primary.mkdir(exist_ok=False)
        append_burst.run(short_primary,Path('/data1/efs/mnt2')/short_primary.name,2000,short)
        (short/'run-exit.txt').write_text('0\n')
        print('candidate 2000-record preserved-store check PASS; starting separate long gate',flush=True)
    path,report=append_burst.run(primary,peer,a.count,out)
    # Warm two-mount byte verification is inside run(); no cold/restart or
    # power-loss acceptance is inferred from that success.
    (out/'run-exit.txt').write_text('0\n')
    print(a.count,'exact 4 KiB records, fsync and two-mount bytes PASS',flush=True)
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());(out/'run-exit.txt').write_text('1\n');raise
