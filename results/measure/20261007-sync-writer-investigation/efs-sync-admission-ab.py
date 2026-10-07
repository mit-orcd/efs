import sys,pathlib,tempfile,json,random,shutil,statistics
sys.path.insert(0,'/tmp/efs-writer-study-20261007/scripts');import bench_profile as b
out=pathlib.Path('/tmp/efs-sync-admission-ab-20261007');out.mkdir(exist_ok=False)
binaries={'before':'/tmp/efs-sync-before-20261007','fair':'/tmp/efs-sync-fair-20261007'}
roots=['/data1/efs/bench','/home/efs/additional-work-dir/bench'];cases=[(16,nw,direct,group) for nw in ['auto'] for direct in [False,True] for group in [[0],[1]]]
work=[(case,r,v) for r in range(5) for case in cases for v in binaries];random.Random(17231).shuffle(work);rows=[]
for n,((qd,nw,direct,group),r,v) in enumerate(work,1):
 dirs=[];label='roots2' if len(group)>1 else 'root%d'%(group[0]+1);name='%s-%s-qd%d-w%s-%s-r%d'%(v,'direct' if direct else 'buffered',qd,nw,label,r);d=out/name
 cmd=[binaries[v],'--bench','data','--rw','write','--sync','--qd',str(qd),'--window',str(1024//qd),'--time','1.5','--direct-io' if direct else '--no-direct-io','--full-paths','--skip-ceiling']+([] if nw=='auto' else ['--writers',nw])
 try:
  for i in group:
   root=tempfile.mkdtemp(prefix='efs-handoff-ab-',dir=roots[i]);dirs.append(root);cmd+=['--storage',root]
  run=b.execute(cmd,d,'baseline',60);metrics=b.metric_rows(d/'baseline.stdout');rows.append(dict(name=name,variant=v,repeat=r,qd=qd,writers=nw,direct=direct,roots=label,run=run,metrics=metrics));(out/'results.json').write_text(json.dumps(rows,indent=2));print('%d/%d %s rc=%d'%(n,len(work),name,run['returncode']),flush=True)
 finally:
  for root in dirs:shutil.rmtree(root)
print('DONE',flush=True)
