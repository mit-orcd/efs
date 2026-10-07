#!/usr/bin/env python3
"""Replace the first timing run, which overlapped a CLI validation check."""
import sys,pathlib,json,tempfile,shutil,time,os
sys.path.insert(0,'/tmp/efs-nuc-sync-study-20261007/scripts')
import bench_profile as b
out=pathlib.Path('/tmp/efs-nuc-latency-20261007');rows=json.loads((out/'results.json').read_text())
assert len(rows)==56 and all(r['valid'] for r in rows)
original=rows[0];assert original['name']=='current-root1-sync0-r2'
original['excluded']='Candidate CLI smoke check overlapped startup of this run; replaced after all diagnostic workloads.'
name=original['name']+'-replacement';d=tempfile.mkdtemp(prefix='efs-latency-',dir='/data1/efs/bench')
try:
 cmd=original['command'][:];cmd[cmd.index('--storage')+1]=d
 env=os.environ.copy()
 for k in ['EFS_PERF_PATH','EFS_BENCH_PERF_CONTROL','EFS_BENCH_PERF_ACK']:env.pop(k,None)
 row=dict(name=name,phase='baseline',variant='current',root=1,sync=False,repeat=2,command=cmd,load_start=os.getloadavg(),start=time.time())
 row['run']=b.execute(cmd,out/'baseline'/name,'baseline',180,env);row['end']=time.time();row['load_end']=os.getloadavg();row['metrics']=b.metric_rows(out/'baseline'/name/'baseline.stdout')
 row['valid']=row['run']['returncode']==0 and b.valid_metrics(row['metrics']);assert row['valid']
 rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2))
 manifest=json.loads((out/'manifest.json').read_text());manifest['phase_order'].append('replacement_baseline');manifest['source_commit']='8f24defb';manifest['exclusions']=[dict(name=original['name'],reason=original['excluded'],replacement=name)];(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
 print(row['metrics'][0],flush=True)
finally:shutil.rmtree(d)
