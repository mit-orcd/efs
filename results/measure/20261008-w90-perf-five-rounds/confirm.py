#!/usr/bin/env python3
import pathlib,subprocess,json,os,re,time
root=pathlib.Path('/data1/efs/bench/w90-perf5-20261008')
for version in ['baseline','round5-final']:
 out=root/f'confirm8-{version}';out.mkdir();scratch=root/'scratch'/out.name;scratch.mkdir()
 os.mkfifo(out/'control');os.mkfifo(out/'ack')
 cmd=['perf','stat','--delay=-1','--control',f'fifo:{out}/control,{out}/ack','-e','task-clock,cycles,instructions,context-switches,cpu-migrations,page-faults','-o',str(out/'perf-stat.txt'),'--',str(root/version/'efs-bench'),'--bench','churn','--storage',str(scratch),'--layout','punch','--capacity-bytes',str(16*1024**3),'--container-bytes',str(1024**3),'--payload-size','65536','--occupancy','95','--qd','8','--steps','10000000','--seconds','60','--warmup','1000','--read-percent','50','--persist','batch','--batch-size','32','--direct-io']
 (out/'command.json').write_text(json.dumps(cmd,indent=2))
 with (out/'stdout.txt').open('w') as stdout,(out/'stderr.txt').open('w') as stderr:
  p=subprocess.run(cmd,env=dict(os.environ,EFS_BENCH_PHASE_MARKERS='1',EFS_BENCH_PERF_CONTROL=str(out/'control'),EFS_BENCH_PERF_ACK=str(out/'ack')),stdout=stdout,stderr=stderr,timeout=600)
 assert p.returncode==0,(version,p.returncode)
 f=dict(re.findall(r'(\w+)=([^\s]+)',next(s for s in (out/'stdout.txt').read_text().splitlines() if s.startswith('BENCH_OK '))))
 assert f['errors']=='0' and f['live_objects']==f['verified_objects']
 (out/'results.json').write_text(json.dumps(f,indent=2));assert not list(scratch.iterdir());scratch.rmdir();print('CONFIRM_PASS',version,f['ops_s'],f['write_p99_us'],flush=True)
