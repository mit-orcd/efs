"""Independent first/last exact-payload oracle on both cold service mounts."""
import hashlib,json
from pathlib import Path
base=Path(__file__).resolve().parent;checked=[]
for tag in ('late','early','late2'):
 placement=json.loads((base/tag/'placement.json').read_text())
 final=json.loads((base/tag/'load-final.json').read_text())
 for mount in ('mnt','mnt2'):
  count=0
  for w in range(8):
   group='0' if w<4 else '2';n=final['counts'][w];assert n>=8
   for i in list(range(4))+list(range(n-4,n)):
    d=Path(placement['groups'][group][(w%4)*32+i//1000]['path'])
    d=Path(str(d).replace('/data1/efs/mnt/',f'/data1/efs/{mount}/'))
    size=1+((w*65537+i)*1103515245+12345)%512
    expected=(hashlib.sha256(f'{w}:{i}'.encode()).digest()*16)[:size]
    assert (d/f'f{w}-{i}').read_bytes()==expected,str(d/f'f{w}-{i}')
    count+=1
  checked.append(dict(tag=tag,mount=mount,exact_files=count,coverage='first and last four completed files per worker'))
(base/'cold-verification.json').write_text(json.dumps(dict(checked=checked),indent=2))
print('384 independent first/last exact payload checks through two cold mounts PASS')
