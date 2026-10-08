import itertools,json,subprocess,tempfile
from pathlib import Path
rows=[]
for layout,direct,size in itertools.product(['files','container'],[False,True],[1,512,65536]):
 with tempfile.TemporaryDirectory(prefix='efs-cold-a-') as a,tempfile.TemporaryDirectory(prefix='efs-cold-b-') as b:
  flags=['--bench','principles','--storage',a,'--storage',b,'--layout',layout,'--payload-size',str(size),'--logical-size','1','--objects','16','--qd','4','--direct-io' if direct else '--no-direct-io']
  if layout=='container' and not direct:flags+=['--container-alignment','1']
  outputs=[]
  for extra in [['--rw','write','--policy','allocating','--persist','batch','--retain'],['--recover-existing']]:
   r=subprocess.run(['/tmp/efs-w95-cold-sanitize',*flags,*extra],capture_output=True,text=True,timeout=60)
   assert r.returncode==0 and 'BENCH_OK' in r.stdout,(r.stdout,r.stderr)
   outputs.append(r.stdout)
  rows.append(dict(layout=layout,direct=direct,size=size,outputs=outputs))
Path('/tmp/efs-w95-cold-matrix.json').write_text(json.dumps(rows,indent=2))
print('12 two-root retained/cold cases under ASan/UBSan PASS')
