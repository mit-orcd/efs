#!/usr/bin/env python3
"""AMD-only names_crazy_dirs diagnostic; run as efs on an idle cluster.
Usage: python3 tests/measure/amd_names_profile.py /data1/efs/NEW_EVIDENCE_DIR
Requires perf/strace attachment permission. Profiles only this case, keeping
its default deadline. Attaches only installed /data1/efs/src processes;
independent clients are excluded. No service restart or feature change.
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

run('quiet',['--jobs','1','--filter','names_crazy_dirs'])
with open(out/'perf-record.log','w') as log:
 prof=sp.Popen(['perf','record','-e','cycles:u','-F','199','--call-graph','dwarf,8192','-p',','.join(str(p) for p,n in pids),'-o',str(out/'names.perf.data')],stdout=log,stderr=log)
 time.sleep(.5)
 try:
  for i in range(3):run('perf-'+str(i),['--jobs','1','--filter','names_crazy_dirs'])
 finally:prof.send_signal(signal.SIGINT);prof.wait(timeout=30)
tracers=[];handles=[]
for pid,name in pids:
 h=open(out/('strace-'+name+'.stderr'),'w');handles.append(h)
 p=sp.Popen(['strace','-f','-c','-w','-p',str(pid),'-o',str(out/('syscalls-'+name+'.txt'))],stdout=h,stderr=h);tracers.append(p)
time.sleep(.5)
try:
 run('strace',['--jobs','1','--filter','names_crazy_dirs'],['strace','-ff','-ttt','-T','-yy','-e','trace=%file,read,write,close,fsync,fdatasync','-o',str(out/'python')])
finally:
 for p in tracers:p.send_signal(signal.SIGINT)
 for p in tracers:p.wait(timeout=30)
 for h in handles:h.close()
(out/'finished').write_text('done\n')
