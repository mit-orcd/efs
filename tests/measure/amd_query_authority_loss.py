#!/usr/bin/env python3
"""AMD query safety during orderly metadata-quorum loss, then restore.
No power-loss repair: SIGTERM only, no data writes during degraded service.
Own exclusive AMD cluster access; run as efs; keep output outside Git.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'live'))
from query_rpc import filtered_request, page_request, query, request, receive

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--stop-nodes', nargs=2, type=int, choices=(1,2,3,4), default=(1,2))
a=p.parse_args();a.output.mkdir(parents=True, exist_ok=False)
base=Path('/data1/efs');root=base/'mnt/package2-deterministic-lifecycle'
assert os.getuid()!=0 and os.environ.get('USER')=='efs' and root.is_dir()
os.environ['EFS_QUERY_TEST_HOST']='127.0.0.1'
env=dict(os.environ,EFS_CLUSTER='amd',EFS_COMMANDS='1',EFS_COMMANDS_SERVER='1',
         EFS_PACKED_WRITES='0',EFS_EXPIRY='0',EFS_REPAIR_EXPERIMENT='0',EFS_TRANSPORT='tcp')
def feature_state(port):
    with socket.create_connection(('127.0.0.1',port),7) as conn:
        body=b'package2'.ljust(256,b'\0')
        conn.sendall(struct.pack('!I',257)+bytes([39])+body)
        length=struct.unpack('!I',receive(conn,4))[0]
        data=receive(conn,length)
    assert len(data)==9 and data[0]==40, data
    mask=struct.unpack('@I',data[1:5])[0];status=data[5]
    assert status in (0,2) and (status==0 or mask==0),(mask,status)
    return mask,status

fields=dict(root=root.stat().st_ino,uid=os.getuid(),gid=os.getgid(),match=3,term=b'file')
reply,text=query(17433,page_request(**fields,limit=1))
assert reply.status==0,(reply.status,text)
page=json.loads(text)['page'];old_boot=reply.boot_id
owner=int(page['token'])>>56
assert owner in a.stop_nodes and len(set(a.stop_nodes))==2
result={'old_owner':owner,'old_boot':old_boot,'stopped_nodes':a.stop_nodes}
live_ports=[17431+n for n in range(1,5) if n not in a.stop_nodes]
try:
    with (root/'.efs/.query'/('v1.stats.'+os.urandom(16).hex())).open('rb') as retained:
        original=retained.read();assert json.loads(original)['stats']['tree_files']==288
        pids=[]
        for n in a.stop_nodes:
            pid=int((base/('logs/efsd-n%d.pid'%n)).read_text())
            args=Path('/proc/%d/cmdline'%pid).read_bytes().split(b'\0')
            assert str(base/('n%d'%n)).encode() in args and b'efsd' in args[0]
            os.kill(pid,signal.SIGTERM);pids.append(pid)
        deadline=time.monotonic()+30
        while any(Path('/proc/%d'%pid).exists() for pid in pids):
            assert time.monotonic()<deadline,'orderly daemon stop deadline'
            time.sleep(.1)
        deadline=time.monotonic()+30
        while any(feature_state(port)[0] for port in live_ports):
            assert time.monotonic()<deadline,'degraded service advertises commands'
            time.sleep(.1)
        verdicts=[]
        requests=[request(**fields,version=v,operation=op) for v in (1,2) for op in (0,1)]
        requests += [filtered_request(**fields),page_request(**fields)]
        for port in live_ports:
            for req in requests:
                r,t=query(port,req);assert r.status and not t,(port,r.status,t)
                verdicts.append(dict(port=port,version=req.version,status=r.status))
        retained.seek(0);assert retained.read()==original
        result.update(degraded=verdicts,retained_bytes=len(original))
        (a.output/'degraded.json').write_text(json.dumps(result,indent=2)+'\n')
finally:
    # Restore daemons before touching FUSE mounts. The generic start script's
    # mkdir -p on a degraded mounted root can fail or block before node launch.
    env.update(EFS_MD_RAFT_N='4',EFS_DATA_GC_ADMISSION='1',EFS_GC_BACKLOG_IDLE_MS='20')
    for n in a.stop_nodes:
        pidfile=base/('logs/efsd-n%d.pid'%n)
        old=int(pidfile.read_text())
        if Path('/proc/%d'%old).exists(): continue
        with (base/('logs/efsd-n%d.log'%n)).open('ab') as log:
            child=subprocess.Popen([str(base/'src/efsd'),'--node-id',str(n),
                '--addr','127.0.0.1','--port',str(17431+n),'--storage',str(base/('n%d'%n)),
                '--storage-engine','files','--quota','20G','--direct-io']+
                (['--join','127.0.0.1:17432'] if n>1 else []),
                env=env,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        pidfile.write_text(str(child.pid)+'\n')
deadline=time.monotonic()+30
while True:
    try:
        if feature_state(17433)==(3,0): break
    except (ConnectionRefusedError,ConnectionResetError,TimeoutError): pass
    assert time.monotonic()<deadline,'restored feature deadline'
    time.sleep(.1)
r,t=query(17432+owner-1,page_request(**fields,token=int(page['token']),cursor=int(page['cursor']),limit=1))
assert r.status and not t,'old boot token accepted after restart'
result['old_token_status']=r.status
with (a.output/'payload.log').open('wb') as f:
    subprocess.run(['python3',str(base/'amd-package2-src/tests/live/efs_query_small_lifecycle.py'),
                    str(base/'mnt'),str(base/'mnt2'),'check'],env=env,stdout=f,
                   stderr=subprocess.STDOUT,check=True,timeout=180)
result['verdict']='PASS'
(a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result),flush=True)
