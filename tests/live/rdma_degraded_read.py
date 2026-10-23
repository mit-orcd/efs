#!/usr/bin/env python3
"""Installed AMD: verified32MiB read with shared follower SIGSTOP.

Fresh strict-RDMA reader mount; ordinary files/direct, features OFF. Workload
uses existing operation budgets, outer180s. Always restores the follower first.
Exclusive access required. Owns only one new file/directory and a home mount.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import uuid

CHUNK=128<<10
TOTAL=32<<20
def block(i):return hashlib.sha256(i.to_bytes(8,'big')).digest()*(CHUNK//32)
if len(sys.argv)>1 and sys.argv[1]=='--reader':
 path=Path(sys.argv[2]);h=hashlib.sha256();n=0
 with path.open('rb',buffering=0) as f:
  for base in range(0,TOTAL,8<<20):
   start=time.monotonic();data=f.read(8<<20)
   if len(data)!=(8<<20):
    print(json.dumps(dict(offset=base,requested=8<<20,returned=len(data),read_seconds=time.monotonic()-start)),flush=True)
   assert len(data)==8<<20, f"short read at {base}: {len(data)} of {8<<20}"
   for off in range(0,len(data),CHUNK):assert data[off:off+CHUNK]==block((base+off)//CHUNK)
   h.update(data);n+=len(data);print(json.dumps(dict(bytes=n,read_seconds=time.monotonic()-start)),flush=True)
  assert not f.read(1)
 print(json.dumps(dict(result='PASS',bytes=n,sha256=h.hexdigest())),flush=True)
 raise SystemExit(0)
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('source',type=Path);p.add_argument('mount',type=Path);p.add_argument('output',type=Path)
p.add_argument('--base-port',type=int,default=17432);p.add_argument('--pid-root',type=Path,default=Path('/data1/efs/logs'))
a=p.parse_args();assert os.environ.get('EFS_TRANSPORT')=='rdma'
a.source=a.source.resolve();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=False)
env=dict(os.environ,EFS_TRANSPORT='rdma',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_EXPIRY='0')
name='rdma-degraded-'+uuid.uuid4().hex;fixture=a.mount/name;fixture.mkdir();path=fixture/'payload'
reader=Path(tempfile.mkdtemp(prefix='rdma-degraded-',dir='/home/efs'))
paused=False;child=None;pid=None;rc=None
log=(a.output/'reader.log').open('w');mountlog=(a.output/'mount.log').open('w')
def abort(signum,_frame):raise KeyboardInterrupt(f'signal {signum}')
signal.signal(signal.SIGTERM,abort);signal.signal(signal.SIGINT,abort)
try:
 with path.open('xb') as f:
  for i in range(TOTAL//CHUNK):f.write(block(i))
  f.flush();os.fsync(f.fileno())
 subprocess.run([str(a.source/'scripts/client.sh'),f'127.0.0.1:{a.base_port}',str(reader)],env=dict(env,EFS_FUSE_LOG=str(a.output/'fuse.log')),stdout=mountlog,stderr=subprocess.STDOUT,check=True,timeout=90)
 statuses={}
 for node in (2,3):statuses[node]=subprocess.check_output([str(a.source/'efs-mgmt'),'raft-status',f'127.0.0.1:{a.base_port+node-1}'],text=True,timeout=15)
 node=next(n for n,s in statuses.items() if s.count('role=FOLLOWER')==2)
 pid=int((a.pid_root/f'efsd-n{node}.pid').read_text())
 cmd=Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0',b' ').decode();assert f'--port {a.base_port+node-1} ' in cmd  # D39: the endpoint selects the member
 (a.output/'fixture.json').write_text(json.dumps(dict(node=node,pid=pid,fixture=str(fixture),reader=str(reader),source=(a.source/'SOURCE_COMMIT').read_text().strip(),bytes=TOTAL,outer_seconds=180,statuses=statuses),indent=2)+'\n')
 os.kill(pid,signal.SIGSTOP);paused=True
 print(f'Paused follower node={node} pid={pid}',flush=True)
 child=subprocess.Popen(['timeout','-k','5','180s','python3',str(Path(__file__).resolve()),'--reader',str(reader/name/'payload')],stdout=log,stderr=subprocess.STDOUT,start_new_session=True,env=env)
 rc=child.wait();(a.output/'read.exit').write_text(str(rc)+'\n');print('read exit',rc,flush=True)
finally:
 if paused:os.kill(pid,signal.SIGCONT);print('Restored follower',pid,flush=True)
 if child and child.poll() is None:
  os.killpg(child.pid,signal.SIGTERM)
  try:child.wait(timeout=5)
  except subprocess.TimeoutExpired:os.killpg(child.pid,signal.SIGKILL)
 log.close();mountlog.close()
 if subprocess.run(['mountpoint','-q',str(reader)]).returncode==0:subprocess.run([str(a.source/'scripts/client.sh'),'stop',str(reader)],env=env,check=True,timeout=90)
 reader.rmdir()
 # Cleanup only our exact owned namespace after restoration.
 if path.exists():path.unlink()
 fixture.rmdir()
print('PASS' if rc==0 else 'FAIL',flush=True)
raise SystemExit(rc or 0)
