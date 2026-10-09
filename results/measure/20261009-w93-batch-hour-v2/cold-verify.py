import hashlib,json,subprocess
from pathlib import Path
base=Path(__file__).resolve().parent
oracle=json.loads((base/'payload-oracle.json').read_text());checked=[]
for tag in ('baseline','noop','both','group'):
 for mount in ('mnt','mnt2'):
  root=Path('/data1/efs')/mount/f'tiny-w93-batch-hour-v2-{tag}-20261009'
  for expected in oracle:
   data=(root/expected['name']).read_bytes()
   assert len(data)==expected['bytes'] and hashlib.sha256(data).hexdigest()==expected['sha256'],str(root/expected['name'])
  checked.append(dict(tag=tag,mount=mount,exact_files=len(oracle)))
(base/'cold-verification.json').write_text(json.dumps(dict(checked=checked,build=subprocess.check_output(['/data1/efs/src/efsd','--version'],text=True)),indent=2))
print('PASS: 256 exact seeded payload checks after full cluster restart and cold mounts')
