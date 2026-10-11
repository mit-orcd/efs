#!/usr/bin/env python3
"""Replace only the four authorized AMD daemons and two clients; retain stores."""
import json,os,shutil,subprocess,time
from pathlib import Path
src=Path('/data1/efs/devel-amd-overload-20261011')
installed=Path('/data1/efs/src')
out=Path('/data1/efs/amd-overload-20261011/deployment');out.mkdir(parents=True,exist_ok=True)
env=os.environ|{'EFS_TRANSPORT':'tcp','EFS_COMMANDS':'0','EFS_COMMANDS_SERVER':'0','EFS_MD_RAFT_N':'4','EFS_DATA_GC_ADMISSION':'1','EFS_GC_BACKLOG_IDLE_MS':'20'}
# Install only the exact binaries that passed the native and live gates.
proof=Path('/data1/efs/amd-overload-20261011')
for marker in ('normal-native.exit','fault-native.exit','long-queue.exit','final-functional.exit','final-functional/restore.exit'):
    assert (proof/marker).read_text().strip()=='0',marker
import hashlib
for line in (proof/'live-binary-sha256.txt').read_text().splitlines():
    digest,name=line.split()
    assert hashlib.sha256((src/name).read_bytes()).hexdigest()==digest,name
def run(args,**kwargs):return subprocess.run([str(v) for v in args],check=True,env=env,**kwargs)
assert os.environ['EFS_AMD_BUILD_ID'] in run([src/'efsd','--version'],capture_output=True,text=True).stdout
for name in ['efsd','efs-fuse','efs-mgmt','libefs.a','efs-bench','efs-query','blake3-bench']:
 if (installed/name).exists() and not (out/(name+'.previous')).exists():shutil.copy2(installed/name,out/(name+'.previous'))
for mount in ['/data1/efs/mnt','/data1/efs/mnt2']:
 run([installed/'scripts/client.sh','stop',mount])
for port in range(17432,17436):
 run([installed/'scripts/server.sh','stop',f'127.0.0.1:{port}'])
# Retain a coherent source tree too; the install manifest contains tracked
# source/docs/evidence only, never store directories or build products.
run(['rsync','-a','--from0','--files-from',proof/'install-source-files.list',str(src)+'/',str(installed)+'/'])
run(['make','clean'],cwd=installed)
for name in ['efsd','efs-fuse','efs-mgmt','libefs.a','efs-bench','efs-query','blake3-bench']:
 shutil.copy2(src/name,installed/name)
records=[]
for i in range(4):
 root=Path(f'/data1/efs/n{i+1}');assert root.is_dir()
 cmd=[str(installed/'efsd'),'--node-id',str(i+1),'--addr','127.0.0.1','--port',str(17432+i),'--storage',str(root),'--quota','37.5G','--direct-io']
 if i:cmd+=['--join','127.0.0.1:17432']
 logs=Path('/data1/efs/logs');logs.mkdir(exist_ok=True)
 canonical=logs/f'efsd-n{i+1}.log'
 with canonical.open('ab') as log:
  p=subprocess.Popen(cmd,stdout=log,stderr=log,env=env,start_new_session=True)
 time.sleep(1);assert p.poll() is None
 records.append({'pid':p.pid,'argv':cmd,'canonical_log':str(canonical)})
 (logs/f'efsd-n{i+1}.pid').write_text(str(p.pid)+'\n')
 run(['python3',installed/'scripts/server_processes.py','record',logs/f'efsd-n{i+1}.pid',root])
 (root/'log/efsd.pid').write_text(str(p.pid)+'\n')
 run(['python3',installed/'scripts/server_processes.py','record',root/'log/efsd.pid',root])
time.sleep(5)
for mount in ['/data1/efs/mnt','/data1/efs/mnt2']:
 run([installed/'scripts/client.sh','127.0.0.1:17432',mount])
 run(['timeout','15','stat',mount])
for i in range(4):
 records[i]['gc_status']=run([installed/'efs-mgmt','gc-status',f'127.0.0.1:{17432+i}'],capture_output=True,text=True).stdout
(out/'deployment.json').write_text(json.dumps({'build':run([installed/'efsd','--version'],capture_output=True,text=True).stdout,'transport':'tcp','commands':False,'gc_idle_ms':20,'data_gc_admission':True,'nodes':records},indent=2)+'\n')
print('AMD admission deployment: four direct/TCP nodes, retained stores, two commands-off clients PASS')
