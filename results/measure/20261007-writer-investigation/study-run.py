import sys, pathlib, tempfile, json, random, os, shutil, hashlib, time
sys.path.insert(0,'/tmp/efs-writer-study-20261007/scripts')
import bench_profile as b
base=pathlib.Path('/tmp/efs-writer-study-results-final-20261007');base.mkdir(exist_ok=False)
binary=pathlib.Path('/tmp/efs-writer-study-20261007/efs-bench')
shutil.copy2(binary,base/'efs-bench')
a=b.parser().parse_args(['--modes','data','--data-rw','split','--data-full-paths','--writer-stats','--writers','0,2,auto','--qds','1,16,64','--data-size','64M','--time','1.5','--skip-ceiling','--frequency','99','--storage-root','/data1/efs/bench','--storage-root','/home/efs/additional-work-dir/bench'])
cases=b.cases_for(a,16); roots=[p.resolve() for p in a.storage_root]
manifest={'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'cases':cases,'time':1.5,'repeats':3,'seed':3707,'baseline_instrumentation':False,'perf_instrumentation':True}
(base/'manifest.json').write_text(json.dumps(manifest,indent=2))
for name,cmd in [('cpu',['lscpu']),('devices',['lsblk','-o','NAME,TYPE,SIZE,ROTA,FSTYPE,MOUNTPOINTS']),('load-before',['sh','-c','uptime; ps -eo pid,comm,pcpu --sort=-pcpu | head -15']),('root1',['findmnt','-T',str(roots[0])]),('root2',['findmnt','-T',str(roots[1])])]:b.execute(cmd,base,name,20)
results={c['name']:dict(c,runs={},repeats=[],metrics=[],status='PASS') for c in cases}
work=[(c,r) for r in range(3) for c in cases];random.Random(3707).shuffle(work)
work += [(c,'perf') for c in random.Random(3708).sample(cases,len(cases))]
for number,(c,r) in enumerate(work,1):
 d=base/c['name']/('perf' if r=='perf' else 'repeat-%d'%r);d.mkdir(parents=True)
 dirs=[]; ctl=None;cmd=[str(base/'efs-bench')]+c['args']+['--skip-ceiling']
 if r!='perf':cmd.remove('--writer-stats')
 for i in c['root_indices']:
  root=pathlib.Path(tempfile.mkdtemp(prefix='efs-writer-study-',dir=roots[i]));dirs.append(root);cmd+=['--storage',str(root)]
 env=os.environ.copy();env.pop('EFS_BENCH_PERF_CONTROL',None);env.pop('EFS_BENCH_PERF_ACK',None);env.pop('EFS_PERF_PATH',None)
 workload=cmd[:]
 if r=='perf':
  ctl=tempfile.TemporaryDirectory(prefix='efs-writer-control-');control=ctl.name+'/ctl';ack=ctl.name+'/ack';os.mkfifo(control);os.mkfifo(ack)
  env['EFS_BENCH_PERF_CONTROL']=control;env['EFS_BENCH_PERF_ACK']=ack
  cmd=['perf','record','-e','cycles','-F','99','-g','--call-graph','dwarf','-o',str(d/'perf.data'),'--delay=-1','--control','fifo:%s,%s'%(control,ack),'--']+cmd
 try:
  run=b.execute(cmd,d,'perf' if r=='perf' else 'baseline',60,env)
  rows=b.metric_rows(d/('perf.stdout' if r=='perf' else 'baseline.stdout'))
  run.update(command=workload,metrics_valid=b.valid_metrics(rows),metrics=rows)
  res=results[c['name']]
  if r=='perf':run.update(call_graph='dwarf',reports_state='PENDING');res['runs']['perf']=run
  else:run['repeat']=r;res['repeats'].append(run);res['runs'].setdefault('baseline',run);res['metrics']=rows
  if run['returncode'] or not run['metrics_valid']:res['status']='FAIL'
  print('%d/%d %s repeat=%s %s'%(number,len(work),c['name'],r,res['status']),flush=True)
  (base/'results.json').write_text(json.dumps(list(results.values()),indent=2))
 finally:
  for root in dirs:shutil.rmtree(root)
  if ctl:ctl.cleanup()
b.execute(['sh','-c','uptime; ps -eo pid,comm,pcpu --sort=-pcpu | head -15'],base,'load-after',20)
b.report_all('/usr/bin/perf',base,list(results.values()),a)
print('DONE',flush=True)
