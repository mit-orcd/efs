import os,subprocess,traceback,hashlib,json
from pathlib import Path
b=Path('/data1/efs/nuc-b239-qualification');src=b/'san-source';out=b/'flow-sanitizers';out.mkdir()
try:
 assert (b/'sanitizers/run-exit.txt').read_text().strip()=='0'
 manifest=json.loads((b/'v030-b239dc7d-inputs.json').read_text())
 for n in ['src/server/raft_host.c','include/efs/raft_flow.h','tests/test_host_flow.py','tests/test_append_receipt.c']:assert hashlib.sha256((src/n).read_bytes()).hexdigest()==manifest[n],n
 cc=out/'cc';cc.write_text('#!/bin/sh\nexec gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "$@"\n');cc.chmod(0o700)
 env=dict(os.environ,CC=str(cc),ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
 for n,c in [('host-flow',['python3','tests/test_host_flow.py']),('append-receipt',['./tests/test_append_receipt'])]:
  with (out/(n+'.log')).open('w') as f:rc=subprocess.run(c,cwd=src,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=180).returncode
  (out/(n+'-exit.txt')).write_text(str(rc));assert rc==0,(n,rc)
 (out/'run-exit.txt').write_text('0')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());(out/'run-exit.txt').write_text('1');raise
