"""Canonical independent private suites; preserve each fixture and verdict."""
import importlib.util,sys,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
b=m.BULK+'/qualification-release-28b2089be6a5';host='fcstor006';kind=sys.argv[1];p=Path('operations/orcd-release-28b2089b')
env=f'export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig:${{PKG_CONFIG_PATH:-}}; export LD_LIBRARY_PATH={m.BULK}/deps/libfuse/lib:${{LD_LIBRARY_PATH:-}}; export EFS_RDMA_DEV=mlx5_2 EFS_RAFT_FLOW=0 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0 EFS_ADMISSION_PRESSURE=0; '
if kind=='fault-integration':
 cmd=f'cd /tmp/efs; make -j4 EFS_GIT_ID=28b2089be6a5-release-normal EFS_GIT_BRANCH=release-v0.3.0-pre-alpha EFS_GIT_DESCRIBE=v0.3.0-pre-alpha-candidate tests/faults/fault_sim && EFS_FAULT_BIN=/tmp/efs bash tests/faults/run_faults.sh /tmp/efs-fault-release-28b2089be6a5 --integrate --mode direct'
elif kind in ('live-gc','packed-gc','create-faults','commands','packed-posix'):
 roots=['/data1/01/efs-orcd-release-28b2089be6a5-private','/data1/02/efs-orcd-release-28b2089be6a5-private'];rootargs=' '.join('--root '+x for x in roots)
 cmd='mkdir -p '+' '.join(roots)+'; cd /tmp/efs; python3 -u tests/live/gc_reclamation.py --mode direct --transport rdma '+rootargs
 if kind=='live-gc':cmd+=' --port 20510 --posix --posix-jobs 16 --continue-posix-failure'
 elif kind=='packed-gc':cmd+=' --port 20520 --mounted-packing-only --packing-clients 7 --posix-jobs 16 --continue-posix-failure'
 elif kind=='commands':cmd+=' --port 20540 --efs-commands-only'
 elif kind=='packed-posix':cmd+=' --port 20550 --mounted-packing-only --packing-clients 7 --posix --posix-jobs 16 --fixture-quota 4G --continue-posix-failure'
 else:cmd+=f' --port 20530 --create-live-retry --create-negative-replay --create-adopt-negative --create-adopt-reply-loss --server-binary {b}/fault-binaries/efsd --client-binary {b}/fault-binaries/efs-fuse'
else:raise SystemExit('unknown suite')
body=f'mkdir -p {b}/private-suites; {env}set +e; '+cmd+f' > {b}/private-suites/{kind}.log 2>&1;rc=$?;echo "$rc" > {b}/private-suites/{kind}.exit;tail -12 {b}/private-suites/{kind}.log;echo suite-exit=$rc;exit 0'
try:r=dict(suite=kind,transcript=m.shell(host,body,timeout=7200))
except Exception as e:r=dict(suite=kind,controller_error=str(e))
(p/(kind+'-controller.json')).write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r),flush=True)
