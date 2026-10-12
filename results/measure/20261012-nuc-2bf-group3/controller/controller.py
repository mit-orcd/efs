import os,json,hashlib
from pathlib import Path
import matrix
b=matrix.home;o=matrix.out;s=matrix.src
assert (o/'build-exit.txt').read_text()=='0'
assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
manifest=json.loads((b/'inputs.json').read_text());assert manifest['commit']=='2bf7e257e2a36d7f8779b770c8eca3d967ab5835'
for n,h in manifest['files'].items():assert hashlib.sha256((s/n).read_bytes()).hexdigest()==h,n
matrix.matrix()
for n,h in manifest['files'].items():assert hashlib.sha256((s/n).read_bytes()).hexdigest()==h,n
(o/'matrix-exit.txt').write_text('0')
