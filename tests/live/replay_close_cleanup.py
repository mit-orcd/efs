#!/usr/bin/env python3
"""NUC captured close/append deadlock regression using disposable store copies."""
import argparse,hashlib,json,os,re,shutil,socket,subprocess,tempfile,time
from raft_tail import commands
from pathlib import Path
p=argparse.ArgumentParser(description="Clone and replay a retained four-node close/append deadlock. Never modifies the original fixture or installed stores.")
p.add_argument('--fixture',required=True,type=Path)
p.add_argument('--blocked-index',required=True,type=int)
p.add_argument('--resolution-index',required=True,type=int)
p.add_argument('--inode',required=True,type=int)
p.add_argument('--inspector',required=True,type=Path)
a=p.parse_args()
source=Path(__file__).resolve().parents[2]
old=a.fixture.resolve()
manifest=json.loads((old/'manifest.json').read_text())
assert len(manifest['roots'])==4 and manifest['port']==20190
for port in range(20190,20194):
 with socket.socket() as sock:sock.bind(('127.0.0.1',port))
work=Path(tempfile.mkdtemp(prefix='close-replay-',dir=old.parent))
print(work,flush=True)
# D39: the copied stores are bound to the fixture's configured member list.
conf=old/'cluster.conf';assert conf.is_file(),'fixture has no cluster.conf'
env=dict(os.environ,EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp',EFS_RAFT_OBS='1',EFS_CLUSTER_CONF=str(conf))
procs=[];logs=[]
try:
 for i,root in enumerate(manifest['roots']):
  target=work/f'n{i+1}';shutil.copytree(root,target)
  log=(work/f'n{i+1}.log').open('wb');logs.append(log)
  args=[str(source/'efsd'),'--cluster',str(conf),'--addr','127.0.0.1','--port',str(20190+i),'--storage',str(target),'--quota','1G','--direct-io']
  procs.append(subprocess.Popen(args,env=env,stdout=log,stderr=log));time.sleep(1)
 deadline=time.monotonic()+90
 while time.monotonic()<deadline:
  texts=[];ok=True
  for i in range(4):
   r=subprocess.run([str(source/'efs-mgmt'),'raft-status',f'127.0.0.1:{20190+i}'],env=env,capture_output=True,text=True,timeout=10)
   texts.append(r.stdout+r.stderr)
   if r.returncode:ok=False
   if i in (1,2,3):
    m=re.search(r'group 2 hosted=1 .*?commit=(\d+) applied=(\d+)',r.stdout)
    if not m or int(m[2])<=a.resolution_index or int(m[1])!=int(m[2]):ok=False
  (work/'raft-status.txt').write_text('\n'.join(texts))
  if ok:
   (work/'result.json').write_text(json.dumps({'pass':False,'log_progress':True,'original_applied':a.blocked_index-1,'original_blocked_close':a.blocked_index,'resolution_records_through':a.resolution_index,'source_fixture':str(old),'status':texts},indent=2))
   print('CAPTURED DEADLOCK LOG PROGRESS PASS; waiting for transaction recovery',flush=True)
   time.sleep(20)
   break
  time.sleep(1)
 else:raise AssertionError('cloned fixture did not progress')
finally:
 for p in procs:
  if p.poll() is None:p.terminate()
 stop_timeouts=[]
 for p in procs:
  try:p.wait(timeout=45)
  except subprocess.TimeoutExpired:
   p.kill();p.wait(timeout=15)
   stop_timeouts.append(p.pid)
 for log in logs:log.close()
 if stop_timeouts:raise AssertionError(("owned clone graceful stop timeouts",stop_timeouts))

# Inspect only stopped clones. Prove the new retry committed, then that the
# durable obligation was retired. OPEN reservations may remain only when
# a surviving durable lease still protects the file.
entries=commands(work/'n2/mdraft/log/raft.log',{0:2**63,2:2**63})
retries=[{'group':g,'index':i,'term':t,'command':c.hex()}
         for (g,i),(t,c) in entries.items()
         if len(c)==17 and c[0]==32 and int.from_bytes(c[1:9],'big')==a.inode]
assert retries,'log advanced without proving durable cleanup retry'
inspections=[]
for node in (2,3,4):
 result=subprocess.run([str(a.inspector.resolve()),str(work/f'n{node}'/'mdraft/kv'),str(a.inode)],capture_output=True,text=True,check=True)
 inspections.append({'node':node,'stdout':result.stdout})
result=json.loads((work/'result.json').read_text())
result.update(pass_=True,deferred_retry_entries=retries,offline_inspections=inspections)
result["pass"]=result.pop("pass_")
result["binary_sha256"]={name:hashlib.sha256((source/name).read_bytes()).hexdigest() for name in ("efsd","efs-mgmt")}
(work/'result.json').write_text(json.dumps(result,indent=2))
print('CAPTURED DEADLOCK REPLAY AND CLEANUP PASS',flush=True)
