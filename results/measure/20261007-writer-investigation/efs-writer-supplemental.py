import sys,pathlib,tempfile,os,json,random,shutil
sys.path.insert(0,'/tmp/efs-writer-study-20261007/scripts');import bench_profile as b
out=pathlib.Path('/tmp/efs-writer-supplemental-20261007');out.mkdir(exist_ok=False)
binary='/tmp/efs-writer-study-results-final-20261007/efs-bench';roots=['/data1/efs/bench','/home/efs/additional-work-dir/bench'];rows=[]
work=[(nw,group,r,measure) for r in range(3) for nw in ['0','2','auto'] for group in [[0],[0,1]] for measure in [False,True]];random.Random(5707).shuffle(work)
for number,(nw,group,r,measure) in enumerate(work,1):
 dirs=[];label='root1' if len(group)==1 else 'roots2';name='overhead-w%s-%s-stats%d-r%d'%(nw,label,measure,r)
 cmd=[binary,'--bench','data','--rw','write','--qd','16','--window','64','--time','1.5','--no-direct-io','--full-paths','--skip-ceiling']+([] if nw=='auto' else ['--writers',nw])+(['--writer-stats'] if measure else [])
 try:
  for i in group:
   root=tempfile.mkdtemp(prefix='efs-writer-overhead-',dir=roots[i]);dirs.append(root);cmd+=['--storage',root]
  run=b.execute(cmd,out/name,'baseline',60);metrics=b.metric_rows(out/name/'baseline.stdout');rows.append(dict(name=name,writers=nw,roots=label,repeat=r,stats=measure,run=run,metrics=metrics));(out/'overhead.json').write_text(json.dumps(rows,indent=2));print(number,len(work),name,run['returncode'],flush=True)
 finally:
  for root in dirs:shutil.rmtree(root)
for nw in ['0','2','auto']:
 with tempfile.TemporaryDirectory(prefix='efs-writer-stat-',dir=roots[0]) as scratch, tempfile.TemporaryDirectory(prefix='efs-writer-stat-control-') as control:
  d=out/('stat-w'+nw);d.mkdir();ctl=control+'/ctl';ack=control+'/ack';os.mkfifo(ctl);os.mkfifo(ack)
  env=os.environ.copy();env['EFS_BENCH_PERF_CONTROL']=ctl;env['EFS_BENCH_PERF_ACK']=ack
  cmd=['perf','stat','-e','context-switches,cpu-migrations,task-clock','-x',',','-o',str(d/'counters.csv'),'--delay=-1','--control','fifo:%s,%s'%(ctl,ack),'--',binary,'--bench','data','--rw','write','--qd','64','--window','16','--time','2','--no-direct-io','--full-paths','--writer-stats','--skip-ceiling','--storage',scratch]+([] if nw=='auto' else ['--writers',nw]);run=b.execute(cmd,d,'stat',60,env);print('stat',nw,run['returncode'],flush=True)
print('DONE',flush=True)
