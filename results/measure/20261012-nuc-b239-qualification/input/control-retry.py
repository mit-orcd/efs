import json,subprocess,os,hashlib,traceback
from pathlib import Path
base=Path('/data1/efs/nuc-b239-qualification');out=base/'sanitizers';src=base/'san-source'
assert (out/'run-exit.txt').read_text().strip()=='1'
assert 'old source must fail overwrite byte assertion' in (out/'failure.txt').read_text()
assert 'too few arguments' in (out/'fold-old-control.log').read_text()
for p in out.glob('*-exit.txt'):
 if p.name not in ('run-exit.txt','fold-old-control-exit.txt'):assert p.read_text().strip()=='0',p
control=base/'w112-b239-revert-control.c';manifest=json.loads((base/'w112-b239-revert-control.json').read_text());assert hashlib.sha256(control.read_bytes()).hexdigest()==manifest['sha256']
with (out/'fold-b239-revert-control.log').open('w') as f:rc=subprocess.run(['python3','tests/test_fold_observation.py'],cwd=src,env=dict(os.environ,FOLD_TEST_WRITE_SOURCE=str(control)),stdout=f,stderr=subprocess.STDOUT,timeout=180).returncode
(out/'fold-b239-revert-control-exit.txt').write_text(str(rc));assert rc!=0
assert "sent[i]=='N'" in (out/'fold-b239-revert-control.log').read_text()
retained=out/'initial-control-input-failure';retained.mkdir()
for n in ('run-exit.txt','failure.txt'):(out/n).rename(retained/n)
(out/'control-retry.json').write_text(json.dumps(manifest,indent=2));(out/'run-exit.txt').write_text('0')
print('Same-source reverted W112 guard control fails exact overwrite bytes; fixed normal/sanitizer passed; aggregate accepted')
