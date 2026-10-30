#!/usr/bin/env python3
"""Sequential BB qualification in a fresh, matching-build private source export."""
import argparse, hashlib, json, os, resource, subprocess, time, traceback
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for key in ('private','source','base','output'):p.add_argument(key,type=Path)
p.add_argument('commit');a=p.parse_args()
private=a.private.resolve();source=a.source.resolve();base=a.base.resolve();out=a.output.absolute()
assert private.name=='efs-taskBB-20261029' and private.stat().st_uid==os.getuid()
assert private in source.parents and private in base.parents and private in out.parents
assert not out.exists();out.mkdir()
state=dict(verdict='RUNNING',source=a.commit,commands=[])
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_')}
env.update(EFS_GIT_ID=a.commit,EFS_GIT_DESCRIBE=a.commit,EFS_GIT_BRANCH='agent/devel-bb',EFS_TRANSPORT='tcp')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save():
 t=out/'state.tmp';t.write_text(json.dumps(state,indent=2)+'\n');t.replace(out/'state.json')
def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def run(name,argv,extra=None,timeout=900,negative=False):
 state['current']=name;save();tmp=private/'tmp'/name;tmp.mkdir(parents=True)
 e=dict(env,TMPDIR=str(tmp),**(extra or {}));start=time.monotonic();log=out/(name+'.log')
 with log.open('x') as f:
  f.write(json.dumps(dict(argv=list(map(str,argv)),extra=extra or {},tmp=str(tmp)))+'\n');f.flush()
  rc=subprocess.run(list(map(str,argv)),cwd=source,env=e,stdout=f,stderr=subprocess.STDOUT,timeout=timeout,preexec_fn=no_core).returncode
 text=log.read_text(errors='replace');row=dict(name=name,rc=rc,elapsed_s=time.monotonic()-start,sha256=sha(log));state['commands'].append(row);save()
 if negative:
  assert rc and ('SIGABRT' in text or 'Assertion' in text) and 'error: ' not in text,(name,rc,text[-2000:])
 else:
  assert rc==0,(name,rc,text[-3000:])
 return text
try:
 run('build',['make','-j8','all','tests/faults/efsd','test-bins'],timeout=1800)
 state['binary_sha256']={n:sha(source/n) for n in ('efsd','efs-fuse','efs-mgmt','tests/faults/efsd')};save()
 for case in ('overlay','mutations','unlink'):
  run('base-'+case,['python3','tests/test_client_metadata_authority.py','--source',base/'src/client/ops.c','--case',case],negative=True)
 run('base-permission',['python3','tests/test_readdir_permission_authority.py'],extra={'EFS_FUSE_SOURCE':str(base/'src/client/efs_fuse.c')},negative=True)
 for name in ('client_metadata_authority','inode_pending_eof','attribute_authority_errno','readdir_permission_authority','sparse_metadata','stage_evict_lru'):
  run('unit-'+name,['python3','tests/test_'+name+'.py'])
 run('unit-asan-ubsan',['python3','tests/test_client_metadata_authority.py'],extra={'CC':'gcc -fsanitize=address,undefined -fno-omit-frame-pointer','ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'})
 run('matrix',['python3','tests/live/bb_overlay_authority.py','--source',source,'--output',out/'matrix'],timeout=600)
 matrix=json.loads((out/'matrix/result.json').read_text());assert matrix['verdict']=='PASS' and not matrix['cleanup_errors'],matrix
 for name in ('gc-a','gc-b'):(private/name).mkdir()
 text=run('gc-posix',['python3','tests/live/gc_reclamation.py','--transport','tcp','--root',private/'gc-a','--root',private/'gc-b','--port','19950','--posix'],timeout=3600)
 assert 'GC reclamation PASS' in text and 'POSIX suite: 218/218 pass' in text and 'POSIX 2-client: 72/72 pass' in text,text[-3000:]
 state['current']='full-gate';save();tmp=private/'full-tmp';tmp.mkdir();e=dict(env,TMPDIR=str(tmp),EFS_TEST_TARGET='test-all',EFS_TEST_JOBS='8',EFS_BUILD_JOBS='8')
 start=time.monotonic()
 with (out/'full-gate.log').open('x') as f:rc=subprocess.run(['bash','scripts/make_test_summary.sh'],cwd=source,env=e,stdout=f,stderr=subprocess.STDOUT,timeout=7200).returncode
 state.update(gate_rc=rc,gate_elapsed_s=time.monotonic()-start,gate_summary=(source/'summary.txt').read_text());save()
 assert rc in (0,3) and state['gate_summary'].startswith('TOTAL ') and ' FAIL 0 ' in state['gate_summary'],state['gate_summary']
 state['verdict']='PASS_SCOPED' if rc==0 else 'PASS_EXECUTED_WITH_HOST_LIMITS'
except BaseException:
 state.update(verdict='FAIL',error=traceback.format_exc());raise
finally:save()
