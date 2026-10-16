import control as c
import json,shlex,sys,time,concurrent.futures,re
from pathlib import Path
P=c.P;MOUNT='/tmp/efs-mount'
def stop_existing():
 rows=json.loads((P/'inventory.json').read_text())
 for h in c.CLIENTS:
  for p in rows[h]:
   if p['comm']=='efs-fuse':
    src=str(Path(p['exe']).parent);mount=p['args'][-1]
    print(h,c.run(h,f'cd {src}; bash scripts/client.sh stop {shlex.quote(mount)}',150),flush=True)
 for h in c.SERVERS:
  for p in rows[h]:
   if p['comm']=='efsd':print(h,c.run(h,f'python3 {Path(p["exe"]).parent}/scripts/server_processes.py stop-port {p["pid"]} 17432',120),flush=True)

def start(label,variant):
 for i,h in enumerate(c.SERVERS,1):
  root=c.STORE_PREFIX+'-'+label
  body=c.ENV+f'''cd {c.SRC}
! pgrep -x efsd
! ss -ltn | grep -q ':17432 '
test ! -e {root}; mkdir {root}; mkdir -p {c.BASE}/{label}/{h}
cp efsd-{variant} efsd
nohup ./efsd --node-id {i} --addr 172.16.223.{i+56} --port 17432 --storage {root} --quota 17G --direct-io {'--join '+c.SEED if i>1 else ''} >{c.BASE}/{label}/{h}/efsd.log 2>&1 </dev/null &
echo $! > {c.BASE}/{label}/{h}/efsd.pid
python3 scripts/server_processes.py record {c.BASE}/{label}/{h}/efsd.pid {root}
'''
  c.run(h,body)
 for attempt in range(40):
  try:
   output=c.run(c.SERVERS[0],c.ENV+f'cd {c.SRC}; timeout 8 ./efs-mgmt status {c.SEED}',15)
   if len(re.findall(r'  up ',output))>=4:break
  except Exception:pass
  time.sleep(1)
 else:raise RuntimeError('not ready')
 print(c.run(c.SERVERS[0],c.ENV+f'''cd {c.SRC}
./efs-mgmt raft-mkfs {c.SEED}
./efs-mgmt raft-setattr {c.SEED} 1 6 0 "$(id -u)" "$(id -g)" 2
./efs-mgmt commands-policy {c.SEED} get
'''),flush=True)
 def mount(h):return c.run(h,c.ENV+f'''cd {c.SRC}; mkdir -p {c.BASE}/{label}/{h}
EFS_FUSE_LOG={c.BASE}/{label}/{h}/fuse.log bash scripts/client.sh {c.SEED} {MOUNT}
test -f {MOUNT}/.efs/stats
''',120)
 c.parallel(c.CLIENTS,mount)

def stop(label):
 c.parallel(c.CLIENTS,lambda h:c.run(h,f'cd {c.SRC}; bash scripts/client.sh stop {MOUNT}',150))
 c.parallel(c.SERVERS,lambda h:c.run(h,f'python3 {c.SRC}/scripts/server_processes.py stop-file {c.BASE}/{label}/{h}/efsd.pid {c.STORE_PREFIX}-{label}',120))

def tests(label):
 c.parallel(c.HOSTS,lambda h:c.run(h,f'nohup python3 {c.BASE}/sample.py $(pgrep -x '+('efsd' if h in c.SERVERS else 'efs-fuse')+f') {c.BASE}/{label}/{h}/resource.jsonl >{c.BASE}/{label}/{h}/sample.log 2>&1 </dev/null &'))
 # Independent jobs start against the same epoch; outer timeouts remain visible failures.
 start=time.time()+25
 def test(h):
  d=f'{c.BASE}/{label}/{h}'
  return c.run(h,c.ENV+f'''cd {c.SRC}; set +e
 timeout --signal=TERM --kill-after=15 180 python3 {c.BASE}/workload.py {MOUNT} {d}/workload.json --seconds 90 --start {start} >{d}/workload.log 2>&1
 echo $? >{d}/workload.exit
 timeout --signal=TERM --kill-after=15 600 python3 tests/posix/posix_suite.py {MOUNT} --suite core --jobs 16 --tag qvote-{label}-{h} --results {d}/posix.tsv >{d}/posix.log 2>&1
 echo $? >{d}/posix.exit
 tail -4 {d}/workload.log; tail -4 {d}/posix.log
''',850)
 with concurrent.futures.ThreadPoolExecutor(max_workers=7) as pool:
  for h,result in zip(c.CLIENTS,pool.map(test,c.CLIENTS)):print(h,result,flush=True)

if __name__=='__main__':
 action=sys.argv[1]
 if action=='stop-existing':stop_existing()
 elif action=='start':start(sys.argv[2],sys.argv[3])
 elif action=='tests':tests(sys.argv[2])
 elif action=='stop':stop(sys.argv[2])
 elif action=='upload':c.upload(c.HERE/'workload.py',c.BASE+'/workload.py');c.upload(c.HERE/'sample.py',c.BASE+'/sample.py')
