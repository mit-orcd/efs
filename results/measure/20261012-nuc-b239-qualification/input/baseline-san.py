import subprocess,json,hashlib,os,traceback
from pathlib import Path
base=Path('/data1/efs/nuc-b239-qualification');src=base/'san-source';out=base/'baseline-san';out.mkdir()
try:
 assert (base/'sanitizers/run-exit.txt').read_text().strip()=='0'
 inputs=json.loads((base/'w97-baseline-inputs.json').read_text())
 for n,row in inputs.items():
  assert hashlib.sha256((src/n).read_bytes()).hexdigest()==row['approved_sha256']
  data=(base/(Path(n).name+'.baseline')).read_bytes();assert hashlib.sha256(data).hexdigest()==row['baseline_sha256'];(src/n).write_bytes(data)
 cmd=['make','-B','tests/test_store_durability','EFS_GIT_ID=b239dc7de104-san','EFS_GIT_DESCRIBE=b239dc7de104-san','EFS_GIT_BRANCH=approved-merged','EFS_FAULTS=1','EXTRA_DEFS=-O1 -fsanitize=address,undefined','LDFLAGS=-lpthread -lm -ldl -libverbs -fsanitize=address,undefined']
 env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
 for name,c in [('build',cmd),('witness',['./tests/test_store_durability'])]:
  with (out/(name+'.log')).open('w') as f:rc=subprocess.run(c,cwd=src,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=1800).returncode
  (out/(name+'-exit.txt')).write_text(str(rc));assert rc==0,(name,rc)
 (base/'baseline-san-exit.txt').write_text('0')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());(base/'baseline-san-exit.txt').write_text('1');raise
