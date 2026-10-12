from pathlib import Path
import subprocess,json,time,os
base=Path('/data1/efs/amd-release-final-1e3a4638-ordinary-20261012')
r=Path('/data1/efs/amd-release-final-1e3a4638-20261012')
assert (r/'finished').exists() and all(v['exit']==0 for v in json.loads((r/'results.json').read_text()))
base.mkdir(exist_ok=False)
start=time.time()
with (base/'run.log').open('w') as log:
 rc=subprocess.run(['python3','/tmp/amd-release-final-ordinary-fixture.py'],stdout=log,stderr=subprocess.STDOUT).returncode
(base/'result.json').write_text(json.dumps({'source':'1e3a46384e897a47751871b15e82ead86c9d0abd','exit':rc,'duration_seconds':time.time()-start}))
(base/'finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
subprocess.run(['python3','/tmp/amd-release-final-ordinary-overwrite.py'],check=True)
