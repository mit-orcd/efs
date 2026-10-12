from pathlib import Path
import subprocess,json,time,os
base=Path('/data1/efs/amd-deep-final-4e239d02-live-20261012')
r=Path('/data1/efs/amd-deep-final-4e239d02-20261012')
while not (r/'finished').exists():time.sleep(5)
assert (r/'finished').exists() and all(v['exit']==0 for v in json.loads((r/'results.json').read_text()))
base.mkdir(exist_ok=False)
start=time.time()
with (base/'run.log').open('w') as log:
 rc=subprocess.run(['python3','/tmp/amd-deep-final-live-fixture.py'],stdout=log,stderr=subprocess.STDOUT).returncode
(base/'result.json').write_text(json.dumps({'source':'4e239d029029b8105a6e07f0e69a26fdc449de32','exit':rc,'duration_seconds':time.time()-start}))
(base/'finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
subprocess.run(['python3','/tmp/amd-deep-final-overwrite.py'],check=True)
