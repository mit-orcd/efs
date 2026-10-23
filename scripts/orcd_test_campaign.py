#!/usr/bin/env python3
"""ORCD four-host campaign. Shared-filesystem mailbox workers need no SSH after launch.
All job subprocesses have a unique ownership environment; timeouts retain artifacts.
"""
import argparse, concurrent.futures, hashlib, json, os, pathlib, signal, socket, subprocess, sys, time, uuid
P=pathlib.Path
HOSTS=['fcstor%03d'%i for i in range(3,7)]
ADDRS=['172.16.223.%d'%i for i in range(57,61)]
a=argparse.ArgumentParser();a.add_argument('mode',choices=['worker','controller','scale']);a.add_argument('run');a.add_argument('--seconds',type=int,default=7200);a.add_argument('--root');a.add_argument('--workers',type=int,default=32);args=a.parse_args()
RUN=P(args.run).resolve(); SRC=RUN/'source'; HOST=socket.gethostname().split('.')[0]
assert HOST in HOSTS
ENV=dict(os.environ,EFS_TRANSPORT='rdma',EFS_RDMA_DEV='mlx5_2',EFS_COMMANDS='1',EFS_COMMANDS_SERVER='1',EFS_EXPIRY='0',EFS_QUERY_TEST_HOST='172.16.223.57',EFS_QUERY_TEST_PORT='17432',EFS_QUERY_TEST_NODES=json.dumps({str(i+1):[ADDRS[i],17432] for i in range(4)}))
ENV.pop('SSH_AUTH_SOCK',None)
def save(path,data):
 path=P(path); tmp=path.with_name(path.name+'.tmp');tmp.write_text(json.dumps(data,indent=2));tmp.replace(path)
def owned(token):
 found=[]
 for p in P('/proc').glob('[0-9]*'):
  try:
   if p.stat().st_uid==os.getuid() and ('EFS_CAMPAIGN_JOB='+token).encode() in (p/'environ').read_bytes().split(b'\0'):found.append(int(p.name))
  except (OSError,PermissionError):pass
 return found
def terminate(token):
 for sig in (signal.SIGTERM,signal.SIGKILL):
  for pid in owned(token):
   try:os.kill(pid,sig)
   except ProcessLookupError:pass
  until=time.monotonic()+(90 if sig==signal.SIGTERM else 10)
  while owned(token) and time.monotonic()<until:time.sleep(.2)
 return owned(token)
def worker():
 box=RUN/'mail'/HOST;box.mkdir(parents=True,exist_ok=True)
 active={}; seen=set()
 while not (RUN/'workers.stop').exists() or active:
  save(box/'heartbeat.json',dict(time=time.time(),pid=os.getpid(),active=list(active)))
  for req in sorted(box.glob('*.request.json')):
   token=req.name.split('.')[0]
   if token in seen or (box/(token+'.result.json')).exists():continue
   seen.add(token);job=json.loads(req.read_text());log=open(box/(token+'.log'),'wb')
   env=dict(ENV,EFS_CAMPAIGN_JOB=token,EFS_QUERY_TEST_FIXTURE="query-"+token)
   proc=subprocess.Popen(job['cmd'],cwd=SRC,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
   active[token]=(proc,job,log,time.monotonic())
   save(box/(token+'.started.json'),dict(pid=proc.pid,time=time.time(),cmd=job['cmd']))
  for token,(proc,job,log,start) in list(active.items()):
   timeout=time.monotonic()-start>job['timeout'];cancel=(box/(token+'.cancel')).exists()
   if proc.poll() is None and not timeout and not cancel:continue
   original=proc.poll();left=terminate(token) if timeout or cancel or (original is not None and not job.get('allow_children')) else []
   try:proc.wait(timeout=3)
   except subprocess.TimeoutExpired:left.append(proc.pid)
   log.close();save(box/(token+'.result.json'),dict(rc=124 if timeout else 130 if cancel else original,seconds=time.monotonic()-start,leftover=left,cmd=job['cmd']))
   del active[token]
  time.sleep(.5)
def scale():
 root=P(args.root);root.mkdir(parents=True,exist_ok=False);end=time.monotonic()+args.seconds
 counters=[0]*args.workers;errors=[];samples={}
 def work(w):
  base=root/('w%02d'%w);base.mkdir();i=0
  try:
   while i<10000000//args.workers and time.monotonic()<end:
    d=base/('d%05d'%(i//1000))
    if i%1000==0:d.mkdir()
    data=hashlib.sha256(('%s:%d:%d'%(HOST,w,i)).encode()).digest()*8
    p=d/('f%04d'%(i%1000))
    with open(p,'xb') as f:f.write(data)
    if i%10000==0:
     with open(p,'rb') as f:assert f.read()==data
     samples[str(p)]=hashlib.md5(data).hexdigest()
    i+=1;counters[w]=i
  except BaseException as e:errors.append(dict(worker=w,index=i,error=repr(e)))
 with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
  futures=[pool.submit(work,w) for w in range(args.workers)]
  while not all(f.done() for f in futures):
   save(RUN/(HOST+'-scale.json'),dict(files=sum(counters),per_worker=counters,errors=errors,time=time.time()));time.sleep(5)
 save(RUN/(HOST+'-scale-samples.json'),samples)
 save(RUN/(HOST+'-scale.json'),dict(files=sum(counters),per_worker=counters,errors=errors,time=time.time(),complete=True))
 return bool(errors)
class Controller:
 def __init__(self):self.results=[];self.jobs=[];self.daemons=[];self.start=time.time()
 def status(self,state,**kw):save(RUN/'status.json',dict(state=state,time=time.time(),started=self.start,elapsed=time.time()-self.start,results=self.results,**kw))
 def submit(self,host,cmd,timeout,allow_children=False):
  token=uuid.uuid4().hex;box=RUN/'mail'/host;box.mkdir(parents=True,exist_ok=True);save(box/(token+'.request.json'),dict(cmd=cmd,timeout=timeout,allow_children=allow_children));j=(host,token);self.jobs.append(j);return j
 def wait(self,j):
  host,token=j;box=RUN/'mail'/host
  while not (box/(token+'.result.json')).exists():
   self.status('RUNNING',current=j)
   hb=box/'heartbeat.json'
   if not hb.exists() or time.time()-json.loads(hb.read_text())['time']>90:raise RuntimeError('worker lost '+host)
   time.sleep(2)
  r=json.loads((box/(token+'.result.json')).read_text())
  if r['leftover']:raise RuntimeError('unsafe leftovers '+str(r))
  return r
 def case(self,name,cmd,timeout=900,host=HOSTS[0]):
  j=self.submit(host,cmd,timeout);r=self.wait(j);r.update(name=name,host=host,job=j[1]);self.results.append(r);self.status('RUNNING');return r['rc']==0
 def shell(self,host,script,timeout=120):
  j=self.submit(host,['bash','-euc',script],timeout,True);r=self.wait(j)
  if r['rc']:raise RuntimeError('control command failed '+str(j))
  return (RUN/'mail'/host/(j[1]+'.log')).read_text()
 def start_cluster(self):
  # D39: one configured member list for every host; the endpoint selects the member.
  (RUN/'cluster.conf').write_text(''.join('%d %s:17432\n'%(i+1,ADDRS[i]) for i in range(len(HOSTS))))
  for i,h in enumerate(HOSTS):
   cmd=[str(SRC/'efsd'),'--cluster',str(RUN/'cluster.conf'),'--addr',ADDRS[i],'--port','17432','--storage','/data1/01/efs-orcd-test-campaign','--quota','2T','--direct-io']
   j=self.submit(h,cmd,15000);self.daemons.append(j)
   time.sleep(3)
   if (RUN/'mail'/h/(j[1]+'.result.json')).exists():raise RuntimeError('daemon failed '+h)
 def mount(self):
  self.shell(HOSTS[0],sys.executable+" scripts/orcd_test_ready.py "+" ".join(a+":17432" for a in ADDRS),210)
  for h in HOSTS:
   self.shell(h,'mkdir -p /tmp/efs-mount; bash scripts/client.sh '+ADDRS[0]+':17432 /tmp/efs-mount',90)
  self.shell(HOSTS[0],'mkdir -p /tmp/efs-mount-peer; bash scripts/client.sh '+ADDRS[0]+':17432 /tmp/efs-mount-peer',90)
 def stop_cluster(self):
  for h in HOSTS:
   for m in (['/tmp/efs-mount','/tmp/efs-mount-peer'] if h==HOSTS[0] else ['/tmp/efs-mount']):
    self.shell(h,'bash scripts/client.sh stop '+m,120)
  for h,t in self.daemons:(RUN/'mail'/h/(t+'.cancel')).touch()
  for j in self.daemons:self.wait(j)
  self.daemons=[]
 def run(self):
  self.status('PREFLIGHT')
  for h in HOSTS:self.shell(h,"test $(id -u) -ne 0; test -d /sys/class/infiniband/mlx5_2; test -w /data1/01; ! ss -ltn | grep -q ':17432 '; test ! -e /data1/01/efs-orcd-test-campaign; mkdir /data1/01/efs-orcd-test-campaign; sha256sum efsd efs-fuse efs-mgmt; ./efsd --version")
  self.start_cluster()
  self.shell(HOSTS[0],"for i in $(seq 1 60); do o=$(./efs-mgmt --cluster "+str(RUN/'cluster.conf')+" raft-mkfs 172.16.223.57:17432); echo \"$o\"; if echo \"$o\" | grep -q 'rc=0'; then exit 0; fi; sleep 2; done; exit 1",150)
  self.mount()
  py=sys.executable;mnt='/tmp/efs-mount';peer='/tmp/efs-mount-peer'
  self.case('posix-core',[py,'tests/posix/posix_suite.py',mnt,'--suite','core','--jobs','4','--keep','--results',str(RUN/'posix.tsv')],1500)
  self.case('two-client',[py,'tests/posix/posix_2client.py','--local',mnt,peer,'--results',str(RUN/'two-client.tsv')],900)
  for name in ['efs_commands','efs_command_options','efs_commands_load']:
   self.case(name,[py,'tests/live/'+name+'.py',mnt,peer],600)
  prepared=self.case('persist-prepare',[py,'tests/posix/posix_persist.py',mnt,'--phase','prepare','--tag','campaign','--keep'],600)
  self.stop_cluster();self.start_cluster();self.mount()
  if prepared:self.case('persist-cold-verify',[py,'tests/posix/posix_persist.py',mnt,'--phase','verify','--tag','campaign','--keep'],600)
  # Load window is bounded by the four-hour total; evidence is never deleted.
  duration=max(60,min(9000,int(self.start+13500-time.time())))
  loads=[self.submit(h,[py,str(RUN/'campaign.py'),'scale',str(RUN),'--root',mnt+'/scale-'+h,'--seconds',str(duration)],duration+120) for h in HOSTS]
  while not all((RUN/'mail'/h/(t+'.result.json')).exists() for h,t in loads):
   if time.time()>self.start+14000:break
   self.case('query-under-load',[py,'tests/live/efs_commands_load.py',mnt,peer],240)
   # Independent samples, not a retry of failures: stop repeating a failed query.
   if self.results[-1]['rc']:break
   time.sleep(30)
  for j in loads:
   r=self.wait(j);r.update(name='scale-10M',host=j[0],job=j[1]);self.results.append(r)
  for h in HOSTS:self.shell(h,'./efs-mgmt pack-compact '+ADDRS[HOSTS.index(h)]+':17432 64; ./efs-mgmt sync-status '+ADDRS[HOSTS.index(h)]+':17432',180)
  self.stop_cluster();self.start_cluster();self.mount()
  for h in HOSTS:
   script="import json,hashlib; d=json.load(open(%r)); bad=[p for p,s in d.items() if hashlib.md5(open(p,'rb').read()).hexdigest()!=s]; print('samples',len(d),'bad',bad); assert not bad"%str(RUN/(h+'-scale-samples.json'))
   self.case('scale-restart-sample',[py,'-c',script],600,h)
  # Stop cleanly, retain all stores. RDMA counters are emitted on disconnect.
  self.stop_cluster()
  for h in HOSTS:
   self.case('rdma-evidence',['bash','-ec',"grep -h 'RDMA counters' "+str(RUN/'mail'/h/'*.log')+" | tee "+str(RUN/(h+'-rdma.txt'))+" | grep -Eq 'data_(tx|rx)=[1-9][0-9]*/[1-9]'"],30,h)
  self.case('packing-evidence',['bash','-ec',"grep -hE 'packed_total=[1-9][0-9]*' "+str(RUN/'mail'/'fcstor*'/'*.log')+" > "+str(RUN/'packing-evidence.txt')],30)
  failed=any(r['rc'] for r in self.results)
  counts={h:json.loads((RUN/(h+'-scale.json')).read_text())['files'] for h in HOSTS}
  total=sum(counts.values())
  self.status('FAIL' if failed else 'PASS' if total>=40000000 else 'INCOMPLETE_SCALE_TARGET',files_by_host=counts,total_files=total,target_files=40000000,notrun=['W123 loss-report continuity: absent from source','Full tens-of-millions payload verification: sample-only'])
  (RUN/'exit-status').write_text(str(1 if failed else 0 if total>=40000000 else 2)+'\n');(RUN/'workers.stop').touch()
if args.mode=='worker':worker()
elif args.mode=='scale':sys.exit(scale())
else:
 c=Controller()
 try:c.run()
 except BaseException as e:
  c.status('BLOCKED',error=repr(e));(RUN/'exit-status').write_text('125\n');raise
