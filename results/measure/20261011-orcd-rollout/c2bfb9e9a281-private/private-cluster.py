"""Exact-source isolated distributed ORCD fixture; never uses existing stores."""
import importlib.util,sys,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
base=m.BULK+'/qualification-c2bfb9e9a281';port=19862;seed=m.ADDRS[m.STORAGE[0]]+':'+str(port);mount='/tmp/efs-c2bf-mount'
roots=[f'/data1/{i:02}/efs-orcd-c2bf-private' for i in range(1,7)]
def source(h):return '/tmp/efs-orcd-unit-c2bfb9e9' if h=='fcstor006' else '/tmp/efs-orcd-stage-c2bfb9e9a281'
def call(h,body,t=120):
 out=m.shell(h,body,timeout=t);print(h,out,flush=True);return out
action=sys.argv[1]
if action in ('start','restart'):
 call('fcstor006',f'test \"$(cat {base}/fault-native.exit)\" = 0; test \"$(cat {base}/normal-native.exit)\" = 0; test \"$(cat {base}/sanitizers.exit)\" = 0')
 call('fcstor006',"python3 -c \"import json; j=json.load(open('/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-c2bfb9e9a281/post-native-input-verification.json')); assert j['inputs']==1557 and not j['mismatches']\"")
 # Global preflight before creating or starting the first private resource.
 for h in m.STORAGE:
  call(h,'; '.join(['test "$(id -u)" = 246111',f'test -x {source(h)}/efsd',f'! ss -ltn | grep -q ":{port} "']+[f'test '+('! -e' if action=='start' else '-d')+f' {r}; mountpoint -q {Path(r).parent}' for r in roots]))
 for h in m.HOSTS:
  call(h,f'{source(h)}/efs-mgmt --version; for b in efsd efs-fuse; do grep -aFq c2bfb9e9-release-normal {source(h)}/$b; done')
 for i,h in enumerate(m.STORAGE,1):
  cmd=f'{source(h)}/efsd --node-id {i} --addr {m.ADDRS[h]} --port {port} --storage '+','.join(roots)+' --direct-io'+(' --join '+seed if i>1 else '')
  call(h,f'''{('mkdir '+ ' '.join(roots)) if action=='start' else ':'}
cd {source(h)}
nohup env EFS_MD_RAFT_N=4 EFS_TRANSPORT=tcp EFS_COMMANDS_SERVER=1 EFS_RAFT_FLOW=0 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0 EFS_ADMISSION_PRESSURE=0 {cmd} </dev/null >>{base}/{h}-private-server.log 2>&1 &
echo $! > {base}/{h}-private-server.pid
sleep 1
kill -0 "$(cat {base}/{h}-private-server.pid)"
''')
elif action=='init':
 h=m.STORAGE[0];call(h,f'''cd {source(h)}
ready=0
for i in {{1..30}}; do output=$(./efs-mgmt status {seed} 2>&1 || true); n=$(printf '%s\\n' "$output" | grep -cE '^  node [0-9]+: .*  up ' || true); if test "$n" = 4; then ready=1; break; fi; sleep 1; done
test "$ready" = 1
printf '%s\\n' "$output"
./efs-mgmt raft-mkfs {seed}
./efs-mgmt raft-setattr {seed} 1 6 0 246111 246111
''')
elif action=='mount':
 for h in m.CLIENTS:
  call(h,f'''if test -e {mount}; then test -d {mount}; ! mountpoint -q {mount}; test -z \"$(ls -A {mount})\"; fi
mkdir -p {mount}
cd {source(h)}
EFS_TRANSPORT=tcp EFS_COMMANDS=1 EFS_PACKED_WRITES=1 EFS_RAFT_FLOW=0 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0 EFS_ADMISSION_PRESSURE=0 EFS_FUSE_LOG={base}/{h}-private-fuse.log bash scripts/client.sh {seed} {mount}
findmnt -rn -M {mount}
''')
elif action=='stop-fourth':
 h=m.STORAGE[3]
 call(h,f"p=$(cat {base}/{h}-private-server.pid); test \"$(readlink /proc/$p/exe)\" = {source(h)}/efsd; tr '\\0' ' ' < /proc/$p/cmdline | grep -F -- '--port {port}'; kill -TERM $p; for i in {{1..60}}; do kill -0 $p 2>/dev/null || break; sleep 1; done; ! kill -0 $p 2>/dev/null",180)
elif action=='restore-fourth':
 h=m.STORAGE[3];cmd=f'{source(h)}/efsd --node-id 4 --addr {m.ADDRS[h]} --port {port} --storage '+','.join(roots)+' --direct-io --join '+seed
 call(h,f"! ss -ltn | grep -q ':{port} '; nohup env EFS_MD_RAFT_N=4 EFS_TRANSPORT=tcp EFS_COMMANDS_SERVER=1 EFS_RAFT_FLOW=0 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0 EFS_ADMISSION_PRESSURE=0 {cmd} </dev/null >>{base}/{h}-private-server.log 2>&1 & echo $! > {base}/{h}-private-server.pid; sleep 1; kill -0 \"$(cat {base}/{h}-private-server.pid)\"")
elif action=='stop':
 for h in m.CLIENTS:call(h,f'cd {source(h)}; if mountpoint -q {mount}; then bash scripts/client.sh stop {mount}; fi',180)
 for h in m.STORAGE:
  call(h,f'''p=$(cat {base}/{h}-private-server.pid)
python3 - "$p" <<'CHECK'
import pathlib,sys,os
p=pathlib.Path('/proc')/sys.argv[1];assert p.stat().st_uid==os.getuid();args=(p/'cmdline').read_bytes().split(bytes([0]));assert b'--port' in args and b'{port}' in args and {roots[0].encode()!r} in b','.join(args)
CHECK
kill -TERM "$p"
for i in {{1..60}}; do kill -0 "$p" 2>/dev/null || break; sleep 1; done
! kill -0 "$p" 2>/dev/null
''',180)
else:raise SystemExit('unknown action')
