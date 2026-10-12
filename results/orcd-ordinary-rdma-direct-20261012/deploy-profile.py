import importlib.util,json,time,shlex,concurrent.futures
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
p=Path('operations/orcd-packing-fix');meta=json.loads((p/'profile-stage.json').read_text());rev=meta['revision'];stage=meta['stage'];rt=m.BULK+'/qualification-b239dc7de104/sanitizer-runtime/usr/lib64'
print('Final-source native prerequisite starting',flush=True)
print(m.shell('fcstor006',f'''cd {stage}
mkdir .profile-toolchain; ln -s ../.profile-cc .profile-toolchain/cc
export PATH={stage}/.profile-toolchain:$PATH
export LD_LIBRARY_PATH={rt}:{m.BULK}/deps/libfuse/lib
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig
EFS_TEST_SANITIZE=1 CC={stage}/.profile-cc python3 tests/test_host_inbox.py
make -j8 CC={stage}/.profile-cc EFS_GIT_ID={rev[:12]} EFS_GIT_BRANCH=fix-orcd-packing-stress EFS_GIT_DESCRIBE={rev[:12]} test > {m.BULK}/fcstor006/profile-native-{rev[:12]}.log 2>&1
 tail -8 {m.BULK}/fcstor006/profile-native-{rev[:12]}.log
''',1800),flush=True)
# Capture fsynced acknowledged data before the orderly old-code shutdown.
marker='.orcd-profile-'+rev[:12]
body=f'''import pathlib,os,hashlib,json
p=pathlib.Path('/tmp/efs-mount/{marker}');p.mkdir()
b=b'ORCD orderly native RDMA/direct IO profile\\n'*4096
for n,data in [('large',b),('small',b[:512])]:
 with (p/n).open('wb') as f:f.write(data);f.flush();os.fsync(f.fileno())
print(json.dumps({{n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in ['large','small']}}))
'''
expected=json.loads(m.shell('fcstor007','python3 -c '+shlex.quote(body),90));(p/'profile-cold-expected.json').write_text(json.dumps(expected,indent=2)+'\n')
print('Old live deployment: orderly client drain/unmount, then server stop',flush=True)
m.stop()
for h in m.HOSTS:
 m.idle_check(h)
 backup='/tmp/efs-before-profile-'+rev[:12]
 m.shell(h,f'test ! -e {backup}; test "$(cat {stage}/.orcd-test-revision)" = {rev}; mv /tmp/efs {backup}; mv {stage} /tmp/efs')
 print(h,'installed',rev[:12],flush=True)
for i,h in enumerate(m.STORAGE,1):
 join='' if i==1 else ' --join '+m.SEED
 command=f'/tmp/efs/efsd --node-id {i} --addr {m.ADDRS[h]} --port 17432 --storage '+','.join(m.STORES)+' --storage-engine files --direct-io'+join
 m.shell(h,f'''cd /tmp/efs
nohup env EFS_MD_RAFT_N=4 EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_RAFT_FLOW=0 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0 EFS_ADMISSION_PRESSURE=0 EFS_REPAIR_EXPERIMENT=0 EFS_REPAIR_INIT_STORE=0 {command} </dev/null >>{m.BULK}/{h}/profile-efsd-{rev[:12]}.log 2>&1 &
echo $! > {m.BASE}/{h}/efsd.pid
python3 scripts/server_processes.py record {m.BASE}/{h}/efsd.pid {m.STORES[0]}
sleep 1; kill -0 "$(cat {m.BASE}/{h}/efsd.pid)"
''',90)
 print(h,'started RDMA/direct/files',flush=True)
for h in m.CLIENTS:
 print(m.shell(h,f'cd /tmp/efs; EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 EFS_COMMANDS=0 EFS_PACKED_WRITES=0 EFS_FUSE_LOG={m.BULK}/{h}/profile-fuse-{rev[:12]}.log bash scripts/client.sh {m.SEED} {m.MOUNT}',90),flush=True)
verify=f'''import pathlib,hashlib,json
p=pathlib.Path('/tmp/efs-mount/{marker}')
actual={{n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in ['large','small']}}
assert actual=={expected!r},actual
assert not pathlib.Path('/tmp/efs-mount/.efs').exists()
print(json.dumps(actual))
'''
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
 rows=list(pool.map(lambda h:{'host':h,'cold':json.loads(m.shell(h,'python3 -c '+shlex.quote(verify),90))},m.CLIENTS))
(p/'profile-cold-results.json').write_text(json.dumps(rows,indent=2)+'\n')
print('DEPLOY PASS: all11 final source, RDMA/direct/files, commands/packing off; acknowledged bytes survived orderly redeploy on all7 clients',flush=True)
