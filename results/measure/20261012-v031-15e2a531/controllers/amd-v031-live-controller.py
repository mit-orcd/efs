from pathlib import Path
import subprocess,json,time,os
base=Path('/data1/efs/amd-v031-15e2a531-20261012-live')
r=Path('/data1/efs/amd-v031-15e2a531-20261012')
while not (r/'finished').exists():time.sleep(5)
assert (r/'finished').exists() and all(v['exit']==0 for v in json.loads((r/'results.json').read_text()))
base.mkdir(exist_ok=False)
start=time.time()
with (base/'run.log').open('w') as log:
 rc=subprocess.run(['python3','/tmp/amd-v031-live-fixture.py'],stdout=log,stderr=subprocess.STDOUT).returncode
(base/'result.json').write_text(json.dumps({'source':'15e2a5310df1dd0fd4e307a9508c95d513d14672','exit':rc,'duration_seconds':time.time()-start}))
(base/'finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
subprocess.run(['python3','/tmp/amd-v031-overwrite.py'],check=True)
