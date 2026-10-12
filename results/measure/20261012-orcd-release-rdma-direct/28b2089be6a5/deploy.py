"""All-node exact release deployment, preserving prior stores and source trees."""
import importlib.util,json,sys,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
rev='28b2089be6a5df835cc7bb46b9c2a9e2f10df190';base=m.BULK+'/qualification-release-'+rev[:12];stage='/tmp/efs-orcd-release-'+rev[:12];backup='/tmp/efs-before-release-'+rev[:12]
action=sys.argv[1]
old=m.shell
def shell(h,b,timeout=90):
 b=b.replace('nohup env EFS_MD_RAFT_N=4 ', 'nohup env EFS_QUERY_DISK_MAX_MIB=65536 EFS_QUERY_DISK_TOTAL_MIB=131072 EFS_RAFT_FLOW=0 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0 EFS_ADMISSION_PRESSURE=0 EFS_MD_RAFT_N=4 ')
 b=b.replace('EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 ', 'EFS_PACKED_WRITES=1 EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 ')
 return old(h,b,timeout)
m.shell=shell
if action=='install':
 old('fcstor006',f'test "$(cat {base}/normal-native.exit)" = 0; test "$(cat {base}/fault-native.exit)" = 0; test "$(cat {base}/sanitizers.exit)" = 0')
 for h in m.HOSTS:
  print(h,old(h,f'''cd {stage}
python3 - <<'CHECK'
import pathlib,json,hashlib
root=pathlib.Path('.');rows=json.loads(pathlib.Path({(base+'/inputs.json')!r}).read_text());bad=[n for n,h in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
CHECK
test "$(id -u)" = 246111
for b in efsd efs-fuse efs-mgmt; do test -x "$b"; done
for b in efsd efs-fuse; do grep -aFq {rev[:12]}-release-normal "$b"; done
sha256sum -c {base}/{('normal-binary-sha256.txt' if h=='fcstor006' else h+'-stage-binaries.sha256')}
./efs-mgmt --version
test ! -e {backup}
''',timeout=90),flush=True)
 # Drain every FUSE mount before stopping any original storage daemon.
 m.stop()
 for h in m.HOSTS:m.idle_check(h)
 for h in m.HOSTS:
  print(h,old(h,f'mv /tmp/efs {backup}; mv {stage} /tmp/efs; printf "%s\\n" {rev} > /tmp/efs/.orcd-test-revision; printf "%s\\n" {rev} > /tmp/efs/.orcd-test-mgmt-revision',timeout=90),flush=True)
 print('all11 installed; originalstores preserved',flush=True)
elif action=='start':
 m.start('rdma',True,True)
 print(old('fcstor003',f'''cd /tmp/efs
ready=0
for i in {{1..120}}; do ./efs-mgmt status {m.SEED} > {base}/deployment-ready.log 2>&1 || true
 up=$(grep -cE '^  node [0-9]+: .*  up ' {base}/deployment-ready.log || true)
 if test "$up" = 4; then ready=1;break;fi
 sleep 2
done
test "$ready" = 1;cat {base}/deployment-ready.log
''',timeout=300),flush=True)
 m.mount('rdma',True)
elif action=='cold':
 m.stop();m.start('rdma',True,True);m.mount('rdma',True)
elif action=='remount-peer':
 h='fcstor009';old(h,'cd /tmp/efs; bash scripts/client.sh stop /tmp/efs-mount',timeout=180)
 shell(h,f'cd /tmp/efs; EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 EFS_COMMANDS=1 EFS_FUSE_LOG={m.BULK}/{h}/efs-fuse.log bash scripts/client.sh {m.SEED} /tmp/efs-mount',timeout=180)
else:raise SystemExit('unknown action')
