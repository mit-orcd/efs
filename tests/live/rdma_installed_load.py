#!/usr/bin/env python3
"""Seven distinct installed AMD clients; stock core budgets and outer180s.

Exclusive cluster access required. Owns only new home mounts and POSIX fixtures;
never changes daemon settings, quotas or stores. Logs remain outside Git.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('source',type=Path)
p.add_argument('output',type=Path)
p.add_argument('--filter')
a=p.parse_args()
assert os.environ.get('EFS_TRANSPORT')=='rdma'
a.source=a.source.resolve();a.output=a.output.resolve()
a.output.mkdir(parents=True,exist_ok=False)
env=dict(os.environ,EFS_TRANSPORT='rdma',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',
         EFS_PACKED_WRITES='0',EFS_EXPIRY='0',EFS_REPAIR_EXPERIMENT='0')
root=Path(tempfile.mkdtemp(prefix='rdma-seven-',dir='/home/efs'))
(a.output/'mount-root.txt').write_text(str(root)+'\n')
sockdir=Path(tempfile.mkdtemp(prefix='rdma-phases-',dir='/tmp'))
s=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);s.bind(str(sockdir/'socket'));s.settimeout(.1)
stop=threading.Event();streams=[];children=[];mounts=[];count=0
phase=(a.output/'phases.log').open('w')
def receive():
 global count
 while not stop.is_set():
  try:line=s.recv(1024).decode()
  except socket.timeout:continue
  count+=1
  if count<=4096:phase.write(line.strip()+f' received_ns={time.monotonic_ns()}\n');phase.flush()
thread=threading.Thread(target=receive,daemon=True);thread.start()
def abort(signum,_frame):raise KeyboardInterrupt(f'signal {signum}')
signal.signal(signal.SIGTERM,abort);signal.signal(signal.SIGINT,abort)
cleanup_errors=[]
try:
 for i in range(7):
  mount=root/f'c{i}';mount.mkdir();mounts.append(mount)
  log=(a.output/f'c{i}-mount.log').open('w');streams.append(log)
  e=dict(env,EFS_FUSE_LOG=str(a.output/f'c{i}-fuse.log'))
  subprocess.run([str(a.source/'scripts/client.sh'),'127.0.0.1:17432',str(mount)],env=e,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=90)
 start=time.monotonic()
 for i,mount in enumerate(mounts):
  log=(a.output/f'c{i}.log').open('w');streams.append(log)
  cmd=['timeout','-k','5','180s','python3',str(a.source/'tests/posix/posix_suite.py'),str(mount),'--suite','core','--jobs','16','--tag',f'rdma-seven-{root.name}-{i}','--results',str(a.output/f'c{i}.tsv')]
  if a.filter:cmd+=['--filter',a.filter]
  e=dict(env,EFS_POSIX_PHASE_SOCKET=str(sockdir/'socket'),EFS_POSIX_PHASE_CLIENT=f'c{i}')
  children.append(subprocess.Popen(cmd,env=e,stdout=log,stderr=subprocess.STDOUT,start_new_session=True))
 codes=[]
 for i,c in enumerate(children):
  rc=c.wait();codes.append(rc);(a.output/f'c{i}.exit').write_text(str(rc)+'\n')
 (a.output/'complete.json').write_text(json.dumps(dict(result='PASS' if not any(codes) else 'FAIL',exit_codes=codes,seconds=time.monotonic()-start,clients=7,jobs_per_client=16,outer_seconds=180,filter=a.filter,source=(a.source/'SOURCE_COMMIT').read_text().strip()),indent=2)+'\n')
 print('PASS' if not any(codes) else 'FAIL',codes,flush=True)
finally:
 for c in children:
  if c.poll() is None:os.killpg(c.pid,signal.SIGTERM)
 end=time.monotonic()+5
 for c in children:
  if c.poll() is None:
   try:c.wait(timeout=max(0,end-time.monotonic()))
   except subprocess.TimeoutExpired:os.killpg(c.pid,signal.SIGKILL)
 for mount in mounts:
  if subprocess.run(['mountpoint','-q',str(mount)]).returncode==0:
   try:subprocess.run([str(a.source/'scripts/client.sh'),'stop',str(mount)],env=env,check=True,timeout=90,stdout=subprocess.DEVNULL)
   except (subprocess.CalledProcessError,subprocess.TimeoutExpired) as error:cleanup_errors.append(str(error))
 stop.set();thread.join();s.close();phase.close()
 for stream in streams:stream.close()
 (a.output/'cleanup.json').write_text(json.dumps(dict(errors=cleanup_errors,phase_datagrams=count,retained=min(count,4096),missing_observations_unknown=True),indent=2)+'\n')
 (sockdir/'socket').unlink();sockdir.rmdir()
 if not cleanup_errors:
  for mount in mounts:mount.rmdir()
  for marker in root.glob('.efs-*.identity'):marker.unlink()
  # Client stop may leave its own identity files; keep nonempty home root.
  if not any(root.iterdir()):root.rmdir()
assert not cleanup_errors,cleanup_errors
raise SystemExit(bool(any(codes)))
