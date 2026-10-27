#!/usr/bin/env python3
"""Task K: .efs query requests through a seed after its group-2 retirement.

Use an already built private tree, 19xxx endpoints and a fresh evidence path.
Runs the existing three virtual-command POSIX cases, with original deadlines;
all three must pass (a skip/NOTRUN is not success for this measurement).
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tests'))
import k33_daemons as kd
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--host',default='127.0.0.1')
p.add_argument('--port-min',type=int,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--product-source',required=True)
a=p.parse_args()
assert 19000 <= a.port_min <= 19990
version=subprocess.check_output([str(root/'efsd'),'--version'],text=True)
assert a.product_source in version,version
a.out.mkdir(parents=True,exist_ok=False)
left=[]
for proc in Path('/proc').glob('[0-9]*'):
    try:
        if proc.stat().st_uid==os.getuid() and (proc/'exe').resolve() in (root/'efsd',root/'tests/faults/efsd',root/'efs-fuse'):
            left.append(str(proc))
    except (OSError,ValueError):pass
left.extend(line for line in Path('/proc/self/mountinfo').read_text().splitlines() if str(root/'tmp')+'/' in line)
assert not left,'NOTRUN: preceding private fixtures remain: '+repr(left)
def ports(n):
    found=[]
    for port in range(a.port_min,20000):
        with socket.socket() as s:
            try:s.bind((a.host,port))
            except OSError:continue
        found.append(port)
        if len(found)==n:return found
    raise AssertionError('no private ports free')
kd.free_ports=ports
work=Path(tempfile.mkdtemp(prefix='efs-k44-query-',dir=os.environ.get('TMPDIR')))
env=dict(os.environ)
c=kd.Cluster(work,'query',[1,2,3,4],env,hosts={i:a.host for i in range(1,5)})
mount=None
try:
    c.start(1);c.start(2,join=1);c.start(3,join=1)
    for i in (1,2,3):c.wait_log(i,r'raft-host: formed: 3 members')
    c.start(4,join=1)
    c.wait_voters([2,3,4],lambda i,v:v[2]==(1,14))
    c.wait_log(1,r'group 2 replica retired')
    rc,out=kd.mgmt(env,'commands-policy',c.ep(1),'get')
    (a.out/'policy-get.txt').write_text(out)
    assert rc==0 and 'rc=0' in out,out
    version_no=re.search(r'version=(\d+)',out).group(1)
    mask=re.search(r'mask=(\d+)',out).group(1)
    assert mask=='3','private cluster must have both commands-policy bits enabled: '+out
    rc,out=kd.mgmt(env,'commands-policy',c.ep(1),'set',mask,version_no)
    (a.out/'policy-set.txt').write_text(out)
    assert rc==0 and 'rc=0' in out,out
    assert kd.fuse_ok(),'NOTRUN: FUSE prerequisites absent'
    mount=kd.Mount(c,1,'mnt')
    args=[sys.executable,str(root/'tests/posix/posix_suite.py'),str(mount.path),
          '--suite','extras','--filter','virt_','--jobs','1','--results',str(a.out/'cases.tsv')]
    (a.out/'command.json').write_text(json.dumps(args)+'\n')
    with (a.out/'posix-query.log').open('w') as log:
        result=subprocess.run(args,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=120)
    (a.out/'exit').write_text(str(result.returncode)+'\n')
    output=(a.out/'posix-query.log').read_text()
    assert result.returncode==0 and re.search(r'POSIX suite: 3/3 pass, 0 fail, 0 skip, 0 notrun',output),output
finally:
    if mount:mount.close()
    c.stop_all()
    (a.out/'identity.txt').write_text(version)
    (a.out/'fixture.txt').write_text(str(work)+'\n')
    for log in work.rglob('*.log'):
        if not log.is_symlink():shutil.copyfile(log,a.out/('-'.join(log.relative_to(work).parts)))
    print('kept fixture',work,flush=True)
print('K44 retired-seed .efs queries: 3/3 PASS',flush=True)
