from pathlib import Path
import os,sys,time,subprocess,json
source=Path(sys.argv[1]);bulk=Path('/home/erbmi1/orcd/scratch/efs/orcd-test/fcstor006');rev=(source/'.orcd-test-revision').read_text().strip();tag=rev[:12];root=Path('/tmp/efs-v031-prerequisites-'+tag);root.mkdir()
rt='/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-b239dc7de104/sanitizer-runtime/usr/lib64'
cc=str(source/'.profile-cc');env=dict(os.environ);env.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
args=['CC='+cc,'EFS_GIT_ID='+tag,'EFS_GIT_BRANCH=release-v0.3.1-pre-alpha','EFS_GIT_DESCRIBE='+tag];rows=[]
def run(name,cmd,cwd=source,extra=None):
 start=time.monotonic()
 with (bulk/('v031-'+tag+'-'+name+'.log')).open('w') as out:rc=subprocess.run(cmd,cwd=cwd,env=dict(env,**(extra or {})),stdout=out,stderr=subprocess.STDOUT).returncode
 rows.append(dict(name=name,exit=rc,command=cmd,duration_seconds=time.monotonic()-start));(bulk/('v031-'+tag+'-prerequisites.json')).write_text(json.dumps(rows,indent=2)+'\n');print(name,rc,flush=True)
 assert rc==0,(name,rc)
run('early-ack',['./tests/test_raft'],extra={'EFS_TEST_EARLY_READ_ACK':'1'})
fault=root/'fault';subprocess.run(['cp','-a',str(source),str(fault)],check=True)
run('fault-clean',['make',*args,'clean'],fault)
run('fault-build',['make','-j8',*args,'EFS_FAULTS=1','all'],fault)
run('fault-native',['make','-j8',*args,'EFS_FAULTS=1','test'],fault)
tools=root/'san-tools';tools.mkdir();sancc=tools/'cc';sancc.write_text('#!/bin/sh\nexec /usr/bin/cc -L'+rt+' -Wl,-rpath,'+rt+' -fsanitize=address,undefined -fno-omit-frame-pointer "$@"\n');sancc.chmod(0o755)
for test in ['test_host_inbox','test_readindex_operation','test_lookup_read_group','test_readindex_pair','test_readindex_remote_pair','test_gc_inode_fanout','test_repair_gc_host','test_repair_host']:
 run('san-'+test,['python3','tests/'+test+'.py'],extra={'PATH':str(tools)+':'+env['PATH'],'CC':str(sancc)})
san=root/'raft-san';subprocess.run(['cp','-a',str(source),str(san)],check=True)
run('raft-san-clean',['make',*args,'clean'],san)
sanargs=[x for x in args if not x.startswith('CC=')]+['CC='+str(sancc),'EXTRA_DEFS=-fsanitize=address,undefined']
run('raft-san-build',['make','-j8',*sanargs,'tests/test_raft'],san)
run('raft-san-native',['./tests/test_raft'],san)
run('raft-san-early-ack',['./tests/test_raft'],san,{'EFS_TEST_EARLY_READ_ACK':'1'})
