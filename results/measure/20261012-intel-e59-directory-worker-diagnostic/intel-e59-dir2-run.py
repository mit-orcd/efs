import os,sys,json,time,subprocess,hashlib,signal,socket
from pathlib import Path
base=Path('/data1/efs/intel-e59-dir2-diag-20261012');out=base/'evidence';host=sys.argv[1];cases=json.loads(Path(sys.argv[2]).read_text());tmp=Path('/tmp/efs-intel-e59-dir2-diag-20261012-'+host)
assert (out/'builds.finished').exists()
tmp.mkdir(exist_ok=False);(tmp/'mnt').mkdir();(tmp/'peer').mkdir();(tmp/'temp').mkdir();(out/'cluster-scripts').mkdir()
subprocess.run(['tar','-xzf',str(base/'amd-cluster-test-scripts.tar.gz'),'-C',str(out/'cluster-scripts')],check=True)
baseenv={k:v for k,v in os.environ.items() if not k.startswith('EFS_')}
baseenv.update(EFS_RAFT_FLOW='0',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_GROUP_DRAIN='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_BATCH_OVERLAP='0',EFS_TRANSPORT='tcp',EFS_DATA_GC_ADMISSION='1',EFS_GC_BACKLOG_IDLE_MS='20',EFS_CLUSTER_SCRIPTS=str(out/'cluster-scripts'),TMPDIR=str(tmp/'temp'))
(out/'runner.pid').write_text(str(os.getpid()))
def binaries(root):return {n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ('efsd','efs-fuse','efs-mgmt','libefs.a','efs-bench','efs-query')}
def owned_processes():
 found=[]
 for p in Path('/proc').glob('[0-9]*'):
  try:
   if p.stat().st_uid!=os.getuid():continue
   argv=[x.decode(errors='replace') for x in (p/'cmdline').read_bytes().split(b'\0') if x]
   if not argv:continue
   own=any(x.startswith(str(base)+'/') or x.startswith(str(tmp)+'/') for x in argv)
   kind=(p/'comm').read_text().strip()
   if own and (kind in ('efsd','efs-fuse') or kind.startswith('memcheck')):
    found.append({'pid':int(p.name),'argv':argv,'start':(p/'stat').read_text().rsplit(')',1)[1].split()[19]})
  except (FileNotFoundError,ProcessLookupError,PermissionError):pass
 return found

def cleanup():
 mounts=[l.split()[1] for l in Path('/proc/mounts').read_text().splitlines() if 'fuse.efs-fuse' in l and (l.split()[1].startswith(str(base)+'/') or l.split()[1].startswith(str(tmp)+'/'))]
 before=owned_processes()
 for mount in mounts:subprocess.run(['fusermount3','-uz',mount],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=15)
 for sig,delay in [(signal.SIGTERM,3),(signal.SIGKILL,1)]:
  for row in owned_processes():
   try:
    fd=os.pidfd_open(row['pid']);p=Path('/proc')/str(row['pid'])
    if (p/'stat').read_text().rsplit(')',1)[1].split()[19]==row['start']:signal.pidfd_send_signal(fd,sig)
    os.close(fd)
   except (FileNotFoundError,ProcessLookupError):pass
  if owned_processes():time.sleep(delay)
 remaining=owned_processes();after=[l for l in Path('/proc/mounts').read_text().splitlines() if 'fuse.efs-fuse' in l and (l.split()[1].startswith(str(base)+'/') or l.split()[1].startswith(str(tmp)+'/'))]
 return {'before_processes':before,'before_mounts':mounts,'remaining_processes':remaining,'remaining_mounts':after,'idle':not remaining and not after}

def gate(case):
 name=case['name'];d=out/'gates'/name;d.mkdir(parents=True,exist_ok=False);root=Path(case['cwd']);env=baseenv.copy();env.update(case['environment']);variant=case['variant']
 env.update(EFS_FAULTS='1' if variant=='fault' else '0',EFS_GIT_ID='e59e815fab2916340baad585dc08712ed182b7ea',EFS_GIT_BRANCH='qualification-v030-tcp-direct',EFS_GIT_DESCRIBE='v0.3.0-pre-alpha-candidate')
 if 'gc_reclamation.py' in ' '.join(case['command']):
  work=Path(case['command'][case['command'].index('--root')+1]);work.mkdir(parents=True,exist_ok=False)
 row=dict(case,source='e59e815fab2916340baad585dc08712ed182b7ea',started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),environment={k:v for k,v in env.items() if k.startswith('EFS_') or k=='TMPDIR'},before_binaries=binaries(root),before_owned_processes=owned_processes())
 assert not row['before_owned_processes'],row['before_owned_processes']
 if name.startswith('packing'):
  row['packing_exclusivity_processes']=subprocess.check_output(['ps','-u',str(os.getuid()),'-o','pid,ppid,pcpu,comm,args'],text=True)
 (d/'command.json').write_text(json.dumps(row,indent=2));(out/'active.json').write_text(json.dumps({'name':name,'started':row['started_utc']}));print(row['started_utc'],name,'START',flush=True);t=time.monotonic()
 with (d/'run.log').open('w') as log:
  try:
   p=subprocess.Popen(case['command'],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True);row['pid']=p.pid
   (out/'active.json').write_text(json.dumps({'name':name,'pid':p.pid,'started':row['started_utc']}))
   try:rc=p.wait(timeout=case['timeout'])
   except subprocess.TimeoutExpired:
    os.killpg(p.pid,signal.SIGTERM)
    try:p.wait(timeout=30)
    except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
    rc=124
  except Exception as error:log.write(repr(error));rc=125
 row.update(returncode=rc,seconds=round(time.monotonic()-t,3),status='PASS' if rc==0 else 'NOTRUN' if rc==77 else 'FAIL',after_binaries=binaries(root),cleanup=cleanup())
 if name.startswith('fault-integration'):
  results=tmp/'fault-integration/results.jsonl'
  if results.exists():
   raw=[json.loads(l) for l in results.read_text().splitlines()];row['fault_subcases']=raw
   if rc==2 and any(r.get('status')=='GAP' for r in raw) and all(r.get('status') in ('PASS','GAP') for r in raw):row['status']='GAP'
 if not row['cleanup']['idle']:row['cleanup_blocker']=True
 (d/'result.json').write_text(json.dumps(row,indent=2));(d/'run.exit').write_text(str(rc)+'\n')
 with (out/'all-gates.jsonl').open('a') as f:f.write(json.dumps(row)+'\n')
 print(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),name,row['status'],rc,flush=True)
 return row['cleanup']['idle']
try:
 for case in cases:
  if (out/'stop-request.json').exists():break
  if not gate(case):break
finally:
 final=cleanup();final['source_post']={}
 manifest=json.loads((base/'inputs.json').read_text())
 for variant in ('normal',):
  root=base/variant;bad=[n for n,h in manifest.items() if not (root/n).is_file() or hashlib.sha256((root/n).read_bytes()).hexdigest()!=h]
  final['source_post'][variant]={'files':len(manifest),'mismatches':bad,'all_match':not bad}
 final['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime());(out/'finished.json').write_text(json.dumps(final,indent=2));(out/'active.json').unlink(missing_ok=True)
 print('CONTROLLER_FINISHED',json.dumps(final),flush=True)
