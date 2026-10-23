#!/usr/bin/env python3
"""AMD jobs16 diagnostic: profile managed daemons/FUSE, trace names worker.
Run as efs: python3 tests/measure/amd_names_loaded.py /data1/efs/NEW_DIRECTORY
Start managed services with EFS_READ_PROF=1 EFS_RPC_PROF=1 EFS_BATCH_OBS=1;
restore quiet service afterwards. Requires perf/strace attachment permission.
The case retains its default deadline. This script does not restart services.
"""
import os,subprocess as sp,time,json,signal
from pathlib import Path
import sys
root=Path('/data1/efs/src'); out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=False)
pids=[]
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  exe=os.readlink(p/'exe');args=(p/'cmdline').read_bytes().decode().split('\0')
  if exe not in (str(root/'efsd'),str(root/'efs-fuse')):continue
  env=dict(x.split('=',1) for x in (p/'environ').read_bytes().decode().split('\0') if '=' in x)
  assert env.get('EFS_TRANSPORT')=='rdma'
  assert all(env.get(k)=='0' for k in ('EFS_COMMANDS','EFS_COMMANDS_SERVER','EFS_EXPIRY'))
  pids.append((int(p.name),'n'+args[args.index('--node-id')+1] if '--node-id' in args else 'fuse-'+Path(args[-2]).name))
 except (FileNotFoundError,PermissionError):pass
assert len(pids)==6,pids
(out/'pids.json').write_text(json.dumps(pids))
base=['python3','-u',str(root/'tests/posix/posix_suite.py'),'/data1/efs/mnt','--suite','core']
results=[]
def run(name,extra,prefix=[]):
 with open(out/(name+'.log'),'w') as log:
  t=time.monotonic();p=sp.run(prefix+base+extra+['--tag',name,'--results',str(out/(name+'.tsv'))],stdout=log,stderr=sp.STDOUT,cwd=root)
 results.append({'stage':name,'exit':p.returncode,'seconds':time.monotonic()-t})
 (out/'progress.json').write_text(json.dumps(results,indent=2))
 assert 'worker still alive' not in (out/(name+'.log')).read_text()
 print(results[-1],flush=True)


import socket,threading
sock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);sock.bind(str(out/'phase.sock'));sock.settimeout(.2)
os.environ['EFS_POSIX_PHASE_SOCKET']=str(out/'phase.sock')
stop=threading.Event();tracers=[]
def observer():
 with open(out/'phases.log','w') as log:
  while not stop.is_set():
   try:msg=sock.recv(4096).decode()
   except socket.timeout:continue
   log.write(msg);log.flush()
   if 'test=names_crazy_dirs event=create-read-list' in msg:
    pid=int(dict(x.split('=',1) for x in msg.split())['pid'])
    h=open(out/'names-strace.stderr','w');handles.append(h)
    tracers.append(sp.Popen(['strace','-ttt','-T','-yy','-e','trace=%file,read,write,close,fsync,fdatasync,getdents64,fstat,lseek','-p',str(pid),'-o',str(out/'names.strace')],stdout=h,stderr=h))
handles=[];t=threading.Thread(target=observer);t.start()
with open(out/'perf.log','w') as log:
 prof=sp.Popen(['perf','record','-e','cycles:u','-F','99','--call-graph','dwarf,8192','-p',','.join(str(p) for p,n in pids),'-o',str(out/'loaded.perf.data')],stdout=log,stderr=log)
 try:run('loaded',['--jobs','16'])
 finally:
  prof.send_signal(signal.SIGINT);prof.wait(timeout=30)
  stop.set();t.join();sock.close()
  for p in tracers:
   if p.poll() is None:p.send_signal(signal.SIGINT)
  for p in tracers:p.wait(timeout=30)
  for h in handles:h.close()
(out/'finished').write_text('done\n')
