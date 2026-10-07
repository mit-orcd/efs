import sys,pathlib,tempfile,json,random,shutil,itertools
sys.path.insert(0,'/tmp/efs-nuc-sync-study-20261007/scripts');import bench_profile as b
out=pathlib.Path('/tmp/efs-nuc-io-study-20261007');out.mkdir(exist_ok=False)
roots=['/data1/efs/bench','/data2/efs/bench'];bins={'before':'/tmp/efs-nuc-before-20261007','fair':'/tmp/efs-nuc-fair-20261007'}
work=list(itertools.product(range(5),bins,[False,True],[False,True],range(2)));random.Random(22117).shuffle(work);rows=[]
for n,(r,v,direct,sync,rootidx) in enumerate(work,1):
 name=f'{v}-{"direct" if direct else "buffered"}-sync{int(sync)}-root{rootidx+1}-r{r}'
 d=tempfile.mkdtemp(prefix='efs-nuc-study-',dir=roots[rootidx])
 try:
  cmd=[bins[v],'--bench','data','--rw','write','--qd','16','--window','64','--time','1.5','--direct-io' if direct else '--no-direct-io','--full-paths','--skip-ceiling','--storage',d]+(['--sync'] if sync else [])
  run=b.execute(cmd,out/name,'baseline',60);metrics=b.metric_rows(out/name/'baseline.stdout')
  rows.append(dict(name=name,variant=v,repeat=r,direct=direct,sync=sync,roots='root'+str(rootidx+1),run=run,metrics=metrics));(out/'results.json').write_text(json.dumps(rows,indent=2));print(f'{n}/{len(work)} {name} rc={run["returncode"]}',flush=True)
 finally:shutil.rmtree(d)
print('DONE',flush=True)
