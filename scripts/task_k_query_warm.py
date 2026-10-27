#!/usr/bin/env python3
"""Task K: stable .efs stats/ls/find after advertised catalog readiness.

Separate from the cold-directory POSIX extras measurement: those original
cases and budgets stay unchanged, and their SKIPs remain INCOMPLETE. Uses the
existing live-test warmup on every serving catalog, then checks each command
once and requires exact quiescent payloads/metadata. No product setting changes.
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
import time

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tests'))
import k33_daemons as kd
sys.path.insert(0,str(root/'tests/live'))
from query_timing import refresh_seconds,settle_seconds,wait_query_nodes_indexed
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
    fixture=mount.path/'catalog-fixture';fixture.mkdir()
    expected={'report.txt':kd.payload('K44-query-report',4096),
              'second.bin':kd.payload('K44-query-second',1024)}
    for name,body in expected.items():
        with (fixture/name).open('wb') as stream:
            stream.write(body);stream.flush();os.fsync(stream.fileno())
    # Keep the product default (60 s), discover its advertised interval,
    # and use the existing per-node readiness gate. This is not a larger
    # query syscall/case deadline or a retry of a failing command.
    interval=refresh_seconds(mount.path)
    os.environ['EFS_QUERY_TEST_NODES']=json.dumps({str(i):[a.host,c.ports[i]] for i in (1,2,3,4)})
    begin=time.monotonic()
    serving=wait_query_nodes_indexed(fixture.stat().st_ino,settle_seconds(interval))
    (a.out/'readiness.json').write_text(json.dumps(dict(interval_s=interval,
        deadline_s=settle_seconds(interval),elapsed_s=time.monotonic()-begin,serving=serving),indent=2)+'\n')
    for command in ('stats','ls','find'):
        args=[sys.executable,str(fixture/'.efs'/command)]
        if command=='find':args+=['report']
        args+=['--json']
        result=subprocess.run(args,env=env,capture_output=True,text=True,timeout=30)
        (a.out/(command+'.stdout')).write_text(result.stdout)
        (a.out/(command+'.stderr')).write_text(result.stderr)
        (a.out/(command+'-exit')).write_text(str(result.returncode)+'\n')
        assert result.returncode==0,(command,result.returncode,result.stderr)
        value=json.loads(result.stdout);assert value['complete'] is True,value
        if command=='stats':
            assert value['stats']['tree_files']==2 and value['stats']['tree_bytes']==5120,value
        else:
            names={bytes.fromhex(e['path_hex']).split(b'/')[-1]:e['size'] for e in value['entries']}
            want={name.encode():len(body) for name,body in expected.items() if command=='ls' or name=='report.txt'}
            assert names==want,(command,names,want)
    for name,body in expected.items():assert (fixture/name).read_bytes()==body,name
    (a.out/'result.json').write_text(json.dumps(dict(verdict='PASS',commands=3,
        exact_payload_files=2,exact_payload_bytes=5120))+'\n')

finally:
    if mount:mount.close()
    c.stop_all()
    (a.out/'identity.txt').write_text(version)
    (a.out/'fixture.txt').write_text(str(work)+'\n')
    for log in work.rglob('*.log'):
        if not log.is_symlink():shutil.copyfile(log,a.out/('-'.join(log.relative_to(work).parts)))
    print('kept fixture',work,flush=True)
print('K44 retired-seed stable .efs queries: stats/ls/find + exact payloads PASS',flush=True)
