import hashlib,json,tarfile,subprocess,os,shutil,traceback
from pathlib import Path
base=Path('/home/efs/nuc-c2-qualification');out=base/'sanitizers';src=base/'san-source';out.mkdir()
try:
 assert (base/'native/run-exit.txt').read_text().strip()=='0'
 src.mkdir()
 with tarfile.open(base/'efs-v030-c2bfb9e9-source.tar.gz') as t:t.extractall(src,filter='data')
 for d in json.loads((base/'v030-c2bfb9e9-dirs.json').read_text()):(src/d).mkdir(parents=True,exist_ok=True)
 inputs=json.loads((base/'v030-c2bfb9e9-inputs.json').read_text())
 for n,h in inputs.items():assert hashlib.sha256((src/n).read_bytes()).hexdigest()==h,n
 env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
 def run(name,cmd,e=env):
  with (out/(name+'.log')).open('w') as f:rc=subprocess.run(cmd,cwd=src,env=e,stdout=f,stderr=subprocess.STDOUT,timeout=3600).returncode
  (out/(name+'-exit.txt')).write_text(str(rc));assert rc==0,(name,rc)
 flags=['EFS_GIT_ID=c2bfb9e9-san','EFS_GIT_DESCRIBE=c2bfb9e9-san','EFS_GIT_BRANCH=release-v0.3.0-pre-alpha','EFS_FAULTS=1','EXTRA_DEFS=-O1 -fsanitize=address,undefined','LDFLAGS=-lpthread -lm -ldl -libverbs -fsanitize=address,undefined']
 run('build',['make','-j4','all',*flags])
 for n in ['test_store_durability','test_append_transaction','test_meta_apply','test_query_disk','test_query_delta','test_query_disk_io','test_kv_lsm','test_query_catalog','test_name_index','test_pack_ledger_recovery']:
  run(n,['./tests/'+n])
 run('append-catchup-host',['python3','tests/test_append_catchup_host.py'],dict(env,CC='gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'))
 run('append-receipt',['./tests/test_append_receipt'])
 cc=out/'cc';cc.write_text('#!/bin/sh\nexec gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "$@"\n');cc.chmod(0o700)
 run('host-flow',['python3','tests/test_host_flow.py'],dict(env,CC=str(cc)))
 run('capture-mixed-cut',['python3','tests/test_query_capture_retry.py'],dict(env,CC='gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'))
 run('fold',['python3','tests/test_fold_observation.py'],dict(env,FOLD_TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'))
 run('wal',['python3','tests/test_kv_wal_faults.py'],dict(env,WAL_TEST_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'))
 with (out/'fold-old-control.log').open('w') as f:oldrc=subprocess.run(['python3','tests/test_fold_observation.py'],cwd=src,env=dict(env,FOLD_TEST_WRITE_SOURCE=str(base/'w112-revert-control.c')),stdout=f,stderr=subprocess.STDOUT,timeout=180).returncode
 (out/'fold-old-control-exit.txt').write_text(str(oldrc));assert oldrc!=0
 assert "sent[i]=='N'" in (out/'fold-old-control.log').read_text(), 'old source must fail overwrite byte assertion'
 for n,h in inputs.items():assert hashlib.sha256((src/n).read_bytes()).hexdigest()==h,n
 (out/'run-exit.txt').write_text('0')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());(out/'run-exit.txt').write_text('1');raise
