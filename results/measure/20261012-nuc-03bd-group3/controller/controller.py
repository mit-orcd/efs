import os,json,hashlib
from pathlib import Path
import matrix
b=matrix.home;o=matrix.out;s=matrix.src
assert (o/'build-exit.txt').read_text()=='0'
assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
manifest=json.loads((b/'inputs.json').read_text());assert manifest['commit']=='03bd89fb7de56c2c6af44ab3bd26c54caf4b8274'
for n,h in manifest['files'].items():assert hashlib.sha256((s/n).read_bytes()).hexdigest()==h,n
matrix.matrix()
for n,h in manifest['files'].items():assert hashlib.sha256((s/n).read_bytes()).hexdigest()==h,n
(o/'matrix-exit.txt').write_text('0')
