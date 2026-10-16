import control as c,experiment as e
import json,time,shlex
# Final OFF result must be complete before retiring any process.
for h in c.CLIENTS:
 c.run(h,f'test -f {c.BASE}/off2/{h}/posix.exit; test -f {c.BASE}/off2/{h}/workload.exit')
# Exact deterministic durable marker, unique to each client.
code='''from pathlib import Path
import os,hashlib
p=Path('/tmp/efs-mount')/NAME
data=(NAME.encode()+b'\\n')*4096
with p.open('wb') as f:f.write(data);f.flush();os.fsync(f.fileno())
assert p.read_bytes()==data
print(hashlib.sha256(data).hexdigest())
'''
markers=c.parallel(c.CLIENTS,lambda h:c.run(h,'python3 -c '+shlex.quote(code.replace('NAME',repr('qvote-cold-'+h)))))
(c.P/'cold-markers.json').write_text(json.dumps(markers))
e.stop('off2')
backup='/tmp/efs-before-'+c.RUN
for h in c.HOSTS:
 print(h,c.run(h,f'''test ! -e {backup}; test -d {c.SRC}
if test -e /tmp/efs; then mv /tmp/efs {backup}; fi
mv {c.SRC} /tmp/efs
printf '%s\\n' {c.REV} >/tmp/efs/.orcd-test-revision
sha256sum /tmp/efs/efsd /tmp/efs/efs-fuse
'''),flush=True)
c.SRC='/tmp/efs'
for i,h in enumerate(c.SERVERS,1):
 root=c.STORE_PREFIX+'-off2';d=f'{c.BASE}/final/{h}';ctl=f'/home/erbmi1/efs-runs/orcd-test/{h}'
 print(h,c.run(h,c.ENV+f'''cd /tmp/efs; mkdir -p {d} {ctl}
nohup ./efsd --node-id {i} --addr 172.16.223.{i+56} --port 17432 --storage {root} --storage-engine files --quota 17G --direct-io {'--join '+c.SEED if i>1 else ''} >{d}/efsd.log 2>&1 </dev/null &
echo $! >{ctl}/efsd.pid
python3 scripts/server_processes.py record {ctl}/efsd.pid {root}
'''),flush=True)
for n in range(60):
 try:
  r=c.run('fcstor003',c.ENV+'timeout 8 /tmp/efs/efs-mgmt raft-status '+c.SEED,15)
  if 'role=LEADER' in r:break
 except Exception:pass
 time.sleep(1)
else:raise RuntimeError('Final startup not ready')
for h in c.CLIENTS:
 print(h,c.run(h,c.ENV+f'''cd /tmp/efs; mkdir -p {c.BASE}/final/{h}
EFS_FUSE_LOG={c.BASE}/final/{h}/fuse.log bash scripts/client.sh {c.SEED} /tmp/efs-mount
''',120),flush=True)
verify='''from pathlib import Path
import hashlib
for h in range(7,14):
 name='qvote-cold-fcstor%03d'%h
 data=(name.encode()+b'\\n')*4096
 actual=(Path('/tmp/efs-mount')/name).read_bytes()
 assert actual==data,(name,len(actual))
print('all seven fsynced payload markers PASS')
'''
result=c.parallel(c.CLIENTS,lambda h:c.run(h,c.ENV+'python3 -c '+shlex.quote(verify)+f'\npython3 /tmp/efs-mount/.efs/stats --json >{c.BASE}/final/{h}/stats.json\nnohup python3 {c.BASE}/sample.py $(pgrep -x efs-fuse) {c.BASE}/final/{h}/resource.jsonl >{c.BASE}/final/{h}/sample.log 2>&1 </dev/null &',120))
(c.P/'final-verification.json').write_text(json.dumps(result))
c.parallel(c.SERVERS,lambda h:c.run(h,f'nohup python3 {c.BASE}/sample.py $(pgrep -x efsd) {c.BASE}/final/{h}/resource.jsonl >{c.BASE}/final/{h}/sample.log 2>&1 </dev/null &'))
print('FINAL OFF DEPLOYMENT VERIFIED',flush=True)
