from pathlib import Path
import subprocess,json,hashlib,tarfile,time,os
root=Path('/data1/efs/amd-common2bf-20261012');root.mkdir(exist_ok=False)
source=root/'source';source.mkdir()
archive=Path('/tmp/qualification-v030-2bf7e257.tar.gz')
assert hashlib.sha256(archive.read_bytes()).hexdigest()=='1769df92cb0f4d9f6bd747d3314f2b2816b88d847d401088b900b9f444878cd2'
with tarfile.open(archive) as t:t.extractall(source,filter='data')
inputs=json.loads(Path('/tmp/qualification-v030-2bf7e257.inputs.json').read_text())
bad=[p for p,h in inputs['files'].items() if not (source/p).is_file() or hashlib.sha256((source/p).read_bytes()).hexdigest()!=h]
(root/'source-check.json').write_text(json.dumps({'commit':inputs['commit'],'files':len(inputs['files']),'mismatches':bad},indent=2))
assert not bad,bad[:3]
env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1';env['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
results=[]
def run(name,cmd,cwd=source,extra=None):
 start=time.time()
 with (root/(name+'.log')).open('w') as log:
  rc=subprocess.run(cmd,cwd=cwd,env=dict(env,**(extra or {})),stdout=log,stderr=subprocess.STDOUT).returncode
 row={'name':name,'command':cmd,'cwd':str(cwd),'commit':inputs['commit'],'exit':rc,'duration_seconds':time.time()-start}
 results.append(row);(root/'results.json').write_text(json.dumps(results,indent=2));(root/(name+'.exit')).write_text(str(rc)+'\n');return rc
args=['EFS_GIT_ID='+inputs['commit'],'EFS_GIT_BRANCH=qualification-v030-tcp-direct','EFS_GIT_DESCRIBE=v0.3.0-pre-alpha-candidate']
if run('normal-build',['make','-j8',*args,'all'])==0:
 run('normal-native',['make',*args,'test'])
 run('early-ack-raft',['./tests/test_raft'],extra={'EFS_TEST_EARLY_READ_ACK':'1'})
# Native fault build is separate from normal images; no mounted work on AMD.
fault=root/'fault-source';subprocess.run(['cp','-a',str(source),str(fault)],check=True)
run('fault-clean',['make',*args,'clean'],fault)
if run('fault-build',['make','-j8',*args,'EFS_FAULTS=1','all'],fault)==0:
 run('fault-native',['make',*args,'EFS_FAULTS=1','test'],fault)
# Previous full Raft sanitizers at52 remain unchanged-source evidence; run
# sanitizers for actual changed host hint extractor on this new frozen source.
wrapper=root/'san-tools';wrapper.mkdir()
cc=wrapper/'cc';cc.write_text('#!/bin/sh\nexec gcc -fsanitize=address,undefined -fno-omit-frame-pointer "$@"\n');cc.chmod(0o755)
for test in ['test_lookup_read_group.py','test_readindex_pair.py','test_readindex_remote_pair.py']:
 run('san-'+test.removesuffix('.py'),['python3','tests/'+test],extra={'PATH':str(wrapper)+':'+env['PATH']})
(root/'finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
