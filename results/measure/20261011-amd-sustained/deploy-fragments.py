#!/usr/bin/env python3
"""Replace only the four authorized AMD daemons and two clients; retain stores."""
import json,os,shutil,subprocess,time
from pathlib import Path
src=Path('/data1/efs/devel-amd-gc-fragments-20261011')
installed=Path('/data1/efs/src')
out=Path('/data1/efs/amd-gc-fragments-20261011/deployment');out.mkdir(parents=True,exist_ok=True)
env=os.environ|{'EFS_TRANSPORT':'tcp','EFS_COMMANDS':'0','EFS_COMMANDS_SERVER':'0'}
env.pop('EFS_GC_BACKLOG_IDLE_MS',None)
def run(args,**kwargs):return subprocess.run([str(v) for v in args],check=True,env=env,**kwargs)
assert os.environ['EFS_AMD_BUILD_ID'] in run([src/'efsd','--version'],capture_output=True,text=True).stdout
for name in ['efsd','efs-fuse','efs-mgmt','libefs.a']:
 shutil.copy2(installed/name,out/(name+'.previous'))
for mount in ['/data1/efs/mnt','/data1/efs/mnt2']:
 run([installed/'scripts/client.sh','stop',mount])
for port in range(17432,17436):
 run([installed/'scripts/server.sh','stop',f'127.0.0.1:{port}'])
for name in ['efsd','efs-fuse','efs-mgmt','libefs.a']:
 shutil.copy2(src/name,installed/name)
records=[]
for i in range(4):
 root=Path(f'/data1/efs/n{i+1}');assert root.is_dir()
 cmd=[str(installed/'efsd'),'--node-id',str(i+1),'--addr','127.0.0.1','--port',str(17432+i),'--storage',str(root),'--quota','37.5G','--direct-io']
 if i:cmd+=['--join','127.0.0.1:17432']
 with (out/f'node-{i+1}.log').open('ab') as log:
  p=subprocess.Popen(cmd,stdout=log,stderr=log,env=env,start_new_session=True)
 time.sleep(1);assert p.poll() is None
 records.append({'pid':p.pid,'argv':cmd})
 (root/'log/efsd.pid').write_text(str(p.pid)+'\n')
 run(['python3',installed/'scripts/server_processes.py','record',root/'log/efsd.pid',root])
time.sleep(5)
for mount in ['/data1/efs/mnt','/data1/efs/mnt2']:
 run([installed/'scripts/client.sh','127.0.0.1:17432',mount])
 run(['timeout','15','stat',mount])
for i in range(4):
 records[i]['gc_status']=run([installed/'efs-mgmt','gc-status',f'127.0.0.1:{17432+i}'],capture_output=True,text=True).stdout
(out/'deployment.json').write_text(json.dumps({'build':run([installed/'efsd','--version'],capture_output=True,text=True).stdout,'transport':'tcp','commands':False,'gc_idle':'default','nodes':records},indent=2)+'\n')
print('AMD deployment: four direct/TCP nodes, retained stores, two commands-off clients PASS')
