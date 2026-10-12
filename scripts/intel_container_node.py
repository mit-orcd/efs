#!/usr/bin/env python3
"""Unprivileged private Intel container fixture actions; raw evidence stays outside Git."""
import argparse,hashlib,json,os,re,signal,socket,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['build','proof','start','mount','stop-clients','stop-servers','run','inventory']);p.add_argument('--base',type=Path,required=True);p.add_argument('--host-index',type=int,choices=[0,1],required=True);p.add_argument('--command');p.add_argument('--source-commit');p.add_argument('--clients',type=int,default=2,choices=range(1,8));a=p.parse_args()
b=a.base.resolve();assert b.parent==Path('/data1/efs/bench') and b.name.startswith('container-') and os.getuid()!=0
s=b/'src';out=b/'artifacts';out.mkdir(exist_ok=True)
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_')};env.update(EFS_MD_RAFT_N='4',EFS_TRANSPORT='tcp',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_PACKED_WRITES='0')
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
  cmd=[str(s/'efsd'),'--node-id',str(n),'--addr',ip,'--port',str(27432+j),'--storage',str(r),'--quota','4G','--writers','2','--storage-engine','containers','--direct-io']
  if n!=1:cmd+=['--join','10.0.0.207:27432']
  rows.append(launch(cmd,'n%d'%n))
 (out/'servers.json').write_text(json.dumps(rows,indent=2));print(json.dumps(rows))
elif a.action=='mount':
 rows=json.loads((out/'clients.json').read_text()) if (out/'clients.json').exists() else [];seed=['10.0.0.209:27432','10.0.0.207:27432'][a.host_index]
 # Existing stopped clients may be remounted; live clients retain their identities.
 for i in range(a.clients):
  if i<len(rows) and os.path.ismount(b/('mnt%d'%i)):continue
  m=b/('mnt%d'%i);m.mkdir(exist_ok=True);assert not os.path.ismount(m)
  row=launch([str(s/'efs-fuse'),seed,'fs',str(m),'-f'],'client%d'%i);end=time.monotonic()+40
  while not os.path.ismount(m):
   assert time.monotonic()<end,'mount timeout';time.sleep(.1)
  if i<len(rows):rows[i]=row
  else:rows.append(row)
 (out/'clients.json').write_text(json.dumps(rows,indent=2));print(json.dumps(rows))
elif a.action=='stop-clients':
 for row in json.loads((out/'clients.json').read_text()):
  m=Path(row['argv'][3]);assert m.parent==b
  if os.path.ismount(m):subprocess.run([str(s/'scripts/client.sh'),'stop',str(m)],env=env,cwd=s,check=True,timeout=75)
 print('clients graceful PASS')
elif a.action=='stop-servers':
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
