#!/usr/bin/env python3
"""Explicit ORCD experiment phases. Requires EFS_ORCD_RK_ARTIFACTS outside Git.

prepare: fresh isolated source exports and ablation builds (does not stop EFS).
round LABEL VARIANT NOOP OVERLAP INTERVAL [--posix]: start on fresh stores and test.
stop LABEL: cleanly unmount clients, then stop this run's recorded daemons.
Existing listeners/mounts must first be stopped with their owning deployment.
"""
import sys,os,json,shlex,time,concurrent.futures,re,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent
os.environ['EFS_QVOTE_ARTIFACTS']=os.environ['EFS_ORCD_RK_ARTIFACTS']
sys.path.insert(0,str(HERE.parent/'orcd_qvote'))
import control as c
REV=os.environ.get('EFS_ORCD_RK_REV','4d6530dd83b086493a65b97cb344daf93291d166')
if not re.fullmatch(r'[0-9a-f]{40}',REV):raise ValueError('use a full commit SHA')
RUN=os.environ.get('EFS_ORCD_RK_RUN','raft-kv-4d6530dd8-20261016')
if not re.fullmatch(r'[A-Za-z0-9_-]+',RUN):raise ValueError('invalid run name')
B='/home/erbmi1/orcd/scratch/efs/orcd-test/'+RUN
S='/tmp/efs-'+RUN
P=c.P;STORE='/data1/01/efs-'+RUN
M='/tmp/efs-mount';STATE=P/'matrix-state.json'
c.BASE=B
def par(hosts,fn):
 with concurrent.futures.ThreadPoolExecutor(max_workers=(4 if len(hosts)>7 else len(hosts))) as ex:
  fs={ex.submit(fn,h):h for h in hosts};out={}
  for f in concurrent.futures.as_completed(fs):
   h=fs[f];out[h]=f.result();print(h,str(out[h])[-500:],flush=True)
 return out


def start(label,variant,noop,overlap,interval,fresh=True):
 if not re.fullmatch(r'[A-Za-z0-9_-]+',label) or variant not in ('default','serial','noseek','stripe1'):raise ValueError('invalid variant/label')
 if noop not in (0,1) or overlap not in (0,1) or not 10<=interval<=1000:raise ValueError('invalid Raft settings')
 root=STORE+'-'+label
 env=c.ENV+f'export EFS_RAFT_BATCH_NOOP={noop} EFS_RAFT_BATCH_OVERLAP={overlap} EFS_RAFT_APPLIED_INTERVAL_MS={interval}\n'
 def server(h):
  i=c.SERVERS.index(h)+1;d=B+'/'+label+'/'+h
  return c.run(h,env+f'''cd {S}
! pgrep -x efsd
{'test ! -e '+root+'; mkdir '+root if fresh else 'test -d '+root}
mkdir -p {d}
cp efsd-{variant} efsd
nohup ./efsd --node-id {i} --addr 172.16.223.{i+56} --port 17432 --storage {root} --storage-engine files --quota 17G --direct-io {'--join '+c.SEED if i>1 else ''} >{d}/efsd{'-restart' if not fresh else ''}.log 2>&1 </dev/null &
echo $! >{d}/efsd.pid
python3 scripts/server_processes.py record {d}/efsd.pid {root}
''')
 par(c.SERVERS,server)
 for n in range(40):
  try:
   o=c.run(c.SERVERS[0],env+f'cd {S}; timeout 8 ./efs-mgmt status {c.SEED}',15)
   if len(re.findall(r'  up ',o))>=4:break
  except Exception:pass
  time.sleep(1)
 else:raise RuntimeError('cluster not ready')
 if fresh:
  # Membership UP can precede a leader. MKFS is idempotent and preserves
  # the committed salt; retry only explicit startup NOT_PRIMARY/BUSY.
  for bootstrap in range(20):
   try:
    print(c.run(c.SERVERS[0],env+f'cd {S}; ./efs-mgmt raft-mkfs {c.SEED}; ./efs-mgmt raft-setattr {c.SEED} 1 6 0 "$(id -u)" "$(id -g)" 2; ./efs-mgmt commands-policy {c.SEED} get'),flush=True)
    break
   except RuntimeError as error:
    if not any(code in str(error) for code in ('rc=-15','rc=-13')):raise
    if bootstrap==19:raise
    print('Waiting for bootstrap leader:',str(error),flush=True);time.sleep(1)
 par(c.CLIENTS,lambda h:c.run(h,env+f'cd {S}; mkdir -p {B}/{label}/{h}; EFS_FUSE_LOG={B}/{label}/{h}/fuse'+('-restart' if not fresh else '')+f'.log bash scripts/client.sh {c.SEED} {M}',120))
 STATE.write_text(json.dumps(dict(label=label,variant=variant,noop=noop,overlap=overlap,interval=interval)))

def stop(label):
 par(c.CLIENTS,lambda h:c.run(h,f'cd {S}; bash scripts/client.sh stop {M}',150))
 par(c.SERVERS,lambda h:c.run(h,f'python3 {S}/scripts/server_processes.py stop-file {B}/{label}/{h}/efsd.pid {STORE}-{label}',120))

def test(label,posix=False):
 par(c.HOSTS,lambda h:c.run(h,f'nohup python3 {B}/sample.py $(pgrep -x '+('efsd' if h in c.SERVERS else 'efs-fuse')+f') {B}/{label}/{h}/resource.jsonl >{B}/{label}/{h}/sample.log 2>&1 </dev/null &'))
 epoch=time.time()+25
 def one(h):
  d=B+'/'+label+'/'+h
  body=f'cd {S}; set +e\ntimeout --kill-after=15 180 python3 {B}/workload.py {M} {d}/workload.json --seconds 90 --start {epoch} >{d}/workload.log 2>&1\necho $? >{d}/workload.exit\n'
  if posix:body+=f'timeout --kill-after=15 600 python3 tests/posix/posix_suite.py {M} --suite core --jobs 16 --tag rk-{label}-{h} --results {d}/posix.tsv >{d}/posix.log 2>&1\necho $? >{d}/posix.exit\ntail -3 {d}/posix.log\n'
  body+=f'cat {d}/workload.exit'
  return c.run(h,body,850)
 par(c.CLIENTS,one)
 code='''import json,pathlib
b=pathlib.Path(%r);rows=[];exits={}
for p in b.glob('fcstor*/workload.json'):
 d=json.loads(p.read_text());rows+=d['rows'];exits[p.parent.name]=(p.parent/'workload.exit').read_text().strip()
s={}
for op in sorted({r['op'] for r in rows}):
 r=[x for x in rows if x['op']==op];v=sorted(x['ms'] for x in r)
 s[op]=dict(count=len(v),errors=sum(x['error'] is not None for x in r),p50=v[int(.5*(len(v)-1))],p95=v[int(.95*(len(v)-1))],p99=v[int(.99*(len(v)-1))],max=max(v))
print(json.dumps(dict(label=%r,summary=s,exits=exits)))
'''%(B+'/'+label,label)
 result=json.loads(c.run('fcstor003','python3 -c '+shlex.quote(code)))
 (P/(label+'-summary.json')).write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
 if len(result['exits'])!=7 or any(v!='0' for v in result['exits'].values()):raise RuntimeError('workload failed; retain cluster for investigation')
 return result

def cold_restart():
 state=json.loads(STATE.read_text());label=state['label']
 def marker(h):
  code="import pathlib,os,hashlib; p=pathlib.Path(%r); data=hashlib.sha256(%r.encode()).digest()*65536; f=p.open('wb'); f.write(data); f.flush(); os.fsync(f.fileno()); f.close(); assert p.read_bytes()==data; print(p,len(data))"%(M+'/rk-cold-'+label+'-'+h,h)
  return c.run(h,'python3 -c '+shlex.quote(code),120)
 par(c.CLIENTS,marker)
 stop(label);start(**state,fresh=False)
 def verify(h):
  code="""import pathlib,hashlib,json,subprocess
mount=pathlib.Path(%r);hosts=%r
for host in hosts:
 p=mount/('rk-cold-'+%r+'-'+host)
 assert p.read_bytes()==hashlib.sha256(host.encode()).digest()*65536,str(p)
fixture=mount/('qvote-'+json.loads(pathlib.Path(%r).read_text())['host'].split('.')[0])/'fixed'
for i in range(32):assert (fixture/('f%%03d'%%i)).read_bytes()==bytes([i])*512
r=subprocess.run(['python3',str(fixture/'.efs/ls'),'--json'],capture_output=True,timeout=30)
assert r.returncode==0,(r.returncode,r.stderr)
assert len(json.loads(r.stdout)['entries'])==32
print('PASS: 7 x 2 MiB cross-client markers, 32 fixed payloads, cold query')
"""%(M,c.CLIENTS,label,B+'/'+label+'/'+h+'/workload.json')
  return c.run(h,'python3 -c '+shlex.quote(code),150)
 results=par(c.CLIENTS,verify);(P/'cold-restart.json').write_text(json.dumps(results,indent=2))

def build(h):
 body=f'''test ! -e {S}; mkdir -p {S} {B}/{h}; tar -xzf {B}/source.tgz -C {S}
cd {S}; export PKG_CONFIG_PATH={c.LIB}/pkgconfig
make -j8 EFS_GIT_ID={REV[:12]} EFS_GIT_BRANCH=devel EFS_GIT_DESCRIBE={REV[:12]} FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,{c.LIB}" efsd efs-fuse efs-mgmt tests/test_kv_lsm_concurrent tests/test_meta_apply >{B}/{h}/build-default.log 2>&1
cp efsd efsd-default
sha256sum efsd efs-fuse efs-mgmt >{B}/{h}/binary-default.sha256
'''
 if h in c.SERVERS:
  for v in ['serial','noseek','stripe1']:
   body+=f'''tar -xzf {B}/source.tgz src/kv/kv_lsm_internal.h src/kv/kv_seg.c
python3 {B}/variant.py {S} {v}
rm -f src/kv/*.o libefs.a
make -j8 EFS_GIT_ID={REV[:12]} EFS_GIT_BRANCH=kv-{v} EFS_GIT_DESCRIBE={REV[:12]}-{v} efsd tests/test_kv_lsm_concurrent >{B}/{h}/build-{v}.log 2>&1
cp efsd efsd-{v}; cp tests/test_kv_lsm_concurrent tests/test_kv_lsm_concurrent-{v}
sha256sum efsd-{v} >{B}/{h}/binary-{v}.sha256
timeout 60 tests/test_kv_lsm_concurrent-{v} 10 >{B}/{h}/unit-{v}.log 2>&1
'''
  body+=f'''tar -xzf {B}/source.tgz src/kv/kv_lsm_internal.h src/kv/kv_seg.c
cp efsd-default efsd
'''
 body+=f'cat {B}/{h}/binary-default.sha256'
 return c.run(h,body,1800)

def prepare():
 with (P/'source.tgz').open('wb') as f:subprocess.run(['git','-C',c.REPO,'archive','--format=tar.gz',REV],stdout=f,check=True)
 for local,remote in [(P/'source.tgz','source.tgz'),(HERE/'variant.py','variant.py'),(Path(c.REPO)/'tests/live/orcd_qvote/workload.py','workload.py'),(HERE.parent/'orcd_qvote/sample.py','sample.py')]:c.upload(local,B+'/'+remote)
 par(c.HOSTS,build)

if __name__=='__main__':
 if sys.argv[1]=='cold':cold_restart()
 elif sys.argv[1]=='prepare':prepare()
 elif sys.argv[1]=='round':
  label,v,no,ov,ms=sys.argv[2:7]
  start(label,v,int(no),int(ov),int(ms));test(label,'--posix' in sys.argv)
 elif sys.argv[1]=='stop':stop(sys.argv[2])
 elif sys.argv[1]=='test':test(sys.argv[2],'--posix' in sys.argv)
