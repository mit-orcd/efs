#!/usr/bin/env python3
"""Unprivileged private Intel container fixture actions; raw evidence stays outside Git."""
import argparse,hashlib,json,os,re,signal,socket,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['build','proof','start','mount','stop-clients','stop-servers','run','inventory']);p.add_argument('--base',type=Path,required=True);p.add_argument('--host-index',type=int,choices=[0,1],required=True);p.add_argument('--command');p.add_argument('--source-commit');p.add_argument('--gc-backlog-idle-ms',type=int,default=20,choices=range(20,1001),help='fair yield after capped GC work makes physical progress; idle/blocked passes keep 1s backoff');p.add_argument('--writers',type=int,default=8,choices=range(1,33),help='private container writer slots per daemon; metadata/helper threads are separate');p.add_argument('--clients',type=int,default=2,choices=range(1,8));a=p.parse_args()
b=a.base.resolve();assert b.parent==Path('/data1/efs/bench') and b.name.startswith('container-') and os.getuid()!=0
s=b/'src';out=b/'artifacts';out.mkdir(exist_ok=True)
def mounted(path):
 # stat/ismount can return false on disconnected or temporarily unservable FUSE.
 for line in Path('/proc/self/mountinfo').read_text().splitlines():
  name=re.sub(r'\\([0-7]{3})',lambda m:chr(int(m[1],8)),line.split()[4])
  if name==str(path):return True
 return False
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_')};env.update(EFS_MD_RAFT_N='4',EFS_TRANSPORT='tcp',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_PACKED_WRITES='0',EFS_GC_BACKLOG_IDLE_MS=str(a.gc_backlog_idle_ms))
def actual(pid):
 q=Path('/proc')/str(pid);return {'pid':pid,'argv':[x.decode() for x in (q/'cmdline').read_bytes().split(b'\0') if x], 'start':(q/'stat').read_text().rsplit(')',1)[1].split()[19], 'environment':[x.decode() for x in (q/'environ').read_bytes().split(b'\0') if x.startswith(b'EFS_')]}
def launch(cmd,name):
 with (out/(name+'.log')).open('ab') as f:pr=subprocess.Popen(cmd,env=env,stdin=subprocess.DEVNULL,stdout=f,stderr=f,start_new_session=True,cwd=s)
 time.sleep(.3);assert pr.poll() is None,(name,pr.returncode);row=actual(pr.pid);assert row['argv']==cmd
 assert all(k+'='+v in row['environment'] for k,v in env.items() if k.startswith('EFS_'));return row
def stop(row):
 try:
  now=actual(row['pid']);assert now['argv']==row['argv'] and now['start']==row['start'],'PID identity changed'
 except FileNotFoundError:return
 fd=os.pidfd_open(row['pid']);signal.pidfd_send_signal(fd,signal.SIGTERM)
 end=time.monotonic()+30
 while time.monotonic()<end:
  try:
   if (Path('/proc')/str(row['pid'])/'stat').read_text().rsplit(')',1)[1].split()[0]=='Z':break
  except FileNotFoundError:break
  time.sleep(.1)
 else:os.close(fd);raise RuntimeError('graceful daemon stop deadline')
 os.close(fd)
if a.action=='build':
 assert a.source_commit and re.fullmatch('[0-9a-f]{40}',a.source_commit),'exact source commit required'
 m=json.loads((b/'devel-container-inputs.json').read_text());assert all(hashlib.sha256((s/n).read_bytes()).hexdigest()==h for n,h in m.items()),'source mismatch'
 with (out/'build.log').open('w') as f:
  subprocess.run(['make','clean'],cwd=s,stdout=f,stderr=f,check=True)
  cmd=['make','-j4','EFS_GIT_ID='+a.source_commit,'EFS_GIT_DESCRIBE='+a.source_commit,'EFS_GIT_BRANCH=devel-container','efsd','efs-fuse','efs-mgmt','efs-query','tests/test_store_containers','tests/test_store_durability','tests/test_store_gc','tests/test_store_overwrite']
  subprocess.run(cmd,cwd=s,stdout=f,stderr=f,check=True)
 (out/'build-identity.json').write_text(json.dumps({'source':a.source_commit,'command':cmd},indent=2));print('build PASS')
elif a.action=='proof':
 m=json.loads((b/'devel-container-inputs.json').read_text());bad=[n for n,h in m.items() if hashlib.sha256((s/n).read_bytes()).hexdigest()!=h];assert not bad,bad
 print(json.dumps({'inputs':len(m),'all_match':True,'binaries':{n:hashlib.sha256((s/n).read_bytes()).hexdigest() for n in ('efsd','efs-fuse','efs-mgmt','efs-query')},'version':subprocess.check_output([str(s/'efsd'),'--version'],text=True)}))
elif a.action=='start':
 rows=[];ip=['10.0.0.207','10.0.0.209'][a.host_index]
 for j in range(2):
  n=1+a.host_index*2+j;r=(b if j==0 else Path('/data2/efs/bench')/b.name)/('n%d'%n);r.mkdir(parents=True,exist_ok=True)
  assert not (r/'data').exists() or (out/'servers.json').exists(),'refuse unowned nonfresh store'
  cmd=[str(s/'efsd'),'--node-id',str(n),'--addr',ip,'--port',str(27432+j),'--storage',str(r),'--quota','4G','--writers',str(a.writers),'--storage-engine','containers','--direct-io']
  if n!=1:cmd+=['--join','10.0.0.207:27432']
  rows.append(launch(cmd,'n%d'%n))
 (out/'servers.json').write_text(json.dumps(rows,indent=2))
 # Popen success does not mean storage recovery has opened the listener yet.
 deadline=time.monotonic()+60
 for port in (27432,27433):
  while True:
   try:
    with socket.create_connection((ip,port),1):pass
    break
   except OSError:
    assert time.monotonic()<deadline,'daemon listener startup timeout'
    assert all(Path('/proc',str(r['pid'])).exists() for r in rows),'daemon exited during recovery'
    time.sleep(.2)
 print(json.dumps(rows))
elif a.action=='mount':
 rows=json.loads((out/'clients.json').read_text()) if (out/'clients.json').exists() else [];seed=['10.0.0.209:27432','10.0.0.207:27432'][a.host_index]
 # Existing stopped clients may be remounted; live clients retain their identities.
 for i in range(a.clients):
  if i<len(rows) and mounted(b/('mnt%d'%i)):
   row=actual(rows[i]['pid']);assert row['argv']==rows[i]['argv'] and row['start']==rows[i]['start'],'stale client identity'
   assert hashlib.sha256((Path('/proc')/str(row['pid'])/'exe').read_bytes()).digest()==hashlib.sha256((s/'efs-fuse').read_bytes()).digest(),'drain old client before rebuilding'
   continue
  m=b/('mnt%d'%i);m.mkdir(exist_ok=True);assert not mounted(m)
  row=launch([str(s/'efs-fuse'),seed,'fs',str(m),'-f'],'client%d'%i);end=time.monotonic()+40
  if i<len(rows):rows[i]=row
  else:rows.append(row)
  (out/'clients.json').write_text(json.dumps(rows,indent=2))
  while not mounted(m):
   assert time.monotonic()<end,'mount timeout';time.sleep(.1)
  while subprocess.run(['timeout','3','stat',str(m)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
   assert time.monotonic()<end,'mounted client serving timeout';time.sleep(.2)
 (out/'clients.json').write_text(json.dumps(rows,indent=2));print(json.dumps(rows))
elif a.action=='stop-clients':
 for row in json.loads((out/'clients.json').read_text()):
  m=Path(row['argv'][3]);assert m.parent==b
  if mounted(m):subprocess.run([str(s/'scripts/client.sh'),'stop',str(m)],env=env,cwd=s,check=True,timeout=75)
 print('clients graceful PASS')
elif a.action=='stop-servers':
 clients=json.loads((out/'clients.json').read_text()) if (out/'clients.json').exists() else []
 assert not any(mounted(Path(r['argv'][3])) for r in clients),'stop clients on BOTH hosts before stopping daemons'
 for row in json.loads((out/'servers.json').read_text()):stop(row)
 print('servers graceful PASS')
elif a.action=='run':
 c=json.loads(a.command);name=c['name'];assert '/' not in name
 with (out/(name+'.log')).open('w') as f:
  start=time.monotonic()
  try:rc=subprocess.run(c['argv'],cwd=s,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=c['timeout']).returncode
  except subprocess.TimeoutExpired:rc=124
 row={'name':name,'argv':c['argv'],'seconds':round(time.monotonic()-start,3),'returncode':rc,'status':'PASS' if rc==0 else 'FAIL'};(out/(name+'.json')).write_text(json.dumps(row,indent=2));print(json.dumps(row));raise SystemExit(rc)
elif a.action=='inventory':
 rows=[]
 for j in range(2):
  n=1+a.host_index*2+j;r=(b if j==0 else Path('/data2/efs/bench')/b.name)/('n%d'%n)
  rows.append({'node':n,'inventory':json.loads(subprocess.check_output(['python3',str(s/'tests/live/storage_engine_inventory.py'),str(r)],env=env,text=True))})
 print(json.dumps(rows))
