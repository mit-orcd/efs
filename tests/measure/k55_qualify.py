#!/usr/bin/env python3
"""Source-bound K55 qualification, private fcstor015 fixtures only.

No original Task AI failure is opened as a store, modified, archived or deleted.
Run from a committed tools export, passing an independently completed candidate
export and old production C source. Every live slow case must succeed, including
I0; a source test's permission to report other refusals is not used to qualify.
No performance claims, deadline/oracle changes, or repeat-until-green.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import threading
import time
import traceback


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('private','source','output','old_source'):p.add_argument(n,type=Path)
    p.add_argument('commit');a=p.parse_args();a.private=a.private.resolve();a.source=a.source.resolve()
    assert a.private.name=='efs-k55-20261028'
    assert not a.output.exists() and str(a.output.resolve()).startswith(str(a.private)+'/')
    assert (a.source/'EXPORT_COMPLETE').read_text().strip()==a.commit
    a.output.mkdir();state=dict(verdict='RUNNING',source=a.commit,commands=[],stages=[])
    lock=threading.Lock()
    def save():
        temp=a.output/'state.tmp';temp.write_text(json.dumps(state,indent=2)+'\n');temp.replace(a.output/'state.json')
    save()
    spec=importlib.util.spec_from_file_location('guard',Path(__file__).with_name('rdma_registry_cluster.py'))
    guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
    def quiet():
        found=guard.foreign_work(a.private)
        if found:raise RuntimeError('foreign EFS/test work; never stop it: '+str(found))
    env={k:v for k,v in os.environ.items() if not k.startswith('EFS_')}
    env.update(EFS_GIT_ID=a.commit,EFS_GIT_DESCRIBE=a.commit,EFS_GIT_BRANCH='agent/devel-k55',
               EFS_FUSE_PREFIX=os.environ['EFS_FUSE_PREFIX'],EFS_RDMA_DEV='mlx5_0')
    runtime=a.private/'deps'
    env.update(CC=str(runtime/'gcc-private-tsan'),
               LD_LIBRARY_PATH=str(runtime/'tsan-root/usr/lib64')+':'+env.get('LD_LIBRARY_PATH',''))
    def run(name,cmd,transport='tcp',extra=None,timeout=900,expected=0):
        quiet()
        tmp=a.private/'tmp'/name;tmp.mkdir(parents=True)
        e=dict(env,EFS_TRANSPORT=transport,TMPDIR=str(tmp),**(extra or {}))
        with lock:state['current']=name;save()
        def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        log=a.output/(name+'.log');start=time.monotonic()
        with log.open('x') as f:
            f.write(json.dumps(dict(argv=list(map(str,cmd)),source=a.commit,transport=transport,
                                   tmp=str(tmp),extra_env=extra or {}))+'\n');f.flush()
            result=subprocess.run(list(map(str,cmd)),cwd=a.source,env=e,stdout=f,
                                  stderr=subprocess.STDOUT,timeout=timeout,
                                  preexec_fn=no_core if expected<0 else None)
        text=log.read_text(errors='replace')
        row=dict(name=name,rc=result.returncode,expected=expected,transport=transport,
                 elapsed_s=time.monotonic()-start,log_sha256=sha(log))
        with lock:state['commands'].append(row);save()
        if result.returncode!=expected:raise RuntimeError(f'{name}: rc {result.returncode}, expected {expected}; retained {log}')
        if 'NOTRUN' in text or 'SKIP' in text:raise RuntimeError(name+': required gate did not execute')
        return row,text
    try:
        quiet();time.sleep(20);quiet()
        prefix=Path(env['EFS_FUSE_PREFIX'])
        libs=subprocess.check_output(['pkg-config','--libs','fuse3'],env=env,text=True).strip()
        run('build',['make','-j8',f'FUSE_LIBS={libs} -Wl,-rpath,{prefix}/lib','all','tests/faults/efsd',
                     'tests/test_data_commit','tests/test_data_commit_index','tests/test_rdma_xprt',
                     'tests/test_rdma_segmented'],timeout=1800)
        state['binary_sha256']={n:sha(a.source/n) for n in ('efsd','efs-fuse','efs-mgmt','tests/faults/efsd')}
        state['product_sha256']=sha(a.source/'src/client/data_commit.c');save()
        run('unit-native',[a.source/'tests/test_data_commit'])
        run('unit-index',[a.source/'tests/test_data_commit_index'])
        cc=[env['CC'],'-D_GNU_SOURCE','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-pthread',
            '-I'+str(a.source/'include'),'-I'+str(a.source/'src/client')]
        test=a.source/'tests/test_data_commit.c'
        for label,flags,e in (
            ('asan-ubsan',['-fsanitize=address,undefined','-fno-omit-frame-pointer'],
             {'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}),
            ('tsan',['-fsanitize=thread'],{'TSAN_OPTIONS':'halt_on_error=1:exitcode=66'})):
            binary=a.output/('unit-'+label)
            run('build-'+label,cc+flags+[test,'-o',binary])
            run('unit-'+label,[binary],extra=e)
        original=test.read_text();anchor='#include "../src/client/data_commit.c"'
        assert original.count(anchor)==1
        negative=a.output/'old-production-test.c'
        negative.write_text(original.replace(anchor,'#include '+json.dumps(str(a.old_source.resolve()))))
        run('build-old-control',cc+[negative,'-o',a.output/'old-proof'])
        _,text=run('old-negative-control',[a.output/'old-proof','slow-proof'],expected=-signal.SIGABRT)
        assert 'sweep deadline exhausted' in text and 'efs_client_data_commit(970)==EFS_OK' in text
        state['stages'].append('component-positive-sanitizers-and-old-negative-PASS');save()
        def slow_case(transport,repeat):
            name=f'b6-slow-{transport}-r{repeat}';art=a.output/(name+'-artifacts')
            run(name,['python3','tests/test_b6_unavailable.py'],transport=transport,
                extra=dict(EFS_B6_MODES='slow',EFS_B6_KEEP='1',EFS_B6_ARTIFACTS=str(art)),timeout=600)
            doc=json.loads((art/'b6-result-slow.json').read_text());m=doc['modes']['slow']
            assert doc['verdict']=='PASS' and not doc['cleanup_errors'],doc
            assert doc['bounds']['commit_slow_ms']==65000 and doc['bounds']['fsync_s']==75.0
            assert all(v[0]=='ok' and v[1]<=75 for v in m['fsync'].values()),m
            assert m['fsync_marked']>=1 and m['acknowledged']==22 and not m['refused_files'],m
            return dict(name=name,result_sha256=sha(art/'b6-result-slow.json'),fsync=m['fsync'],
                        marked=m['fsync_marked'],acknowledged=m['acknowledged'])
        # Planned repetitions: both cases must pass, never select a good repeat.
        for repeat in (1,2):
            cases=[slow_case(t,repeat) for t in ('tcp','rdma')]
            state['stages'].extend(cases);save()
        for transport in ('tcp','rdma'):
            _,text=run('k35-'+transport,['python3','tests/test_k35_fsync_degraded.py'],transport=transport,
                       extra={'EFS_K35_KEEP':'1'},timeout=600)
            assert 'K35 fsync with a member down:' in text and 'PASS' in text
        run('rdma-xprt',[a.source/'tests/test_rdma_xprt'],transport='rdma')
        for mode in ('normal','interrupt','tcp-violation','deadline','burst','burst-asym','churn'):
            _,text=run('rdma-segmented-'+mode,[a.source/'tests/test_rdma_segmented',mode],transport='rdma')
            assert 'PASS' in text
        roots=[a.private/'gc-a',a.private/'gc-b']
        for r in roots:r.mkdir()
        _,text=run('gc-posix-rdma',['python3','tests/live/gc_reclamation.py','--transport','rdma',
                                 '--root',roots[0],'--root',roots[1],'--port','19920','--posix'],
                   transport='rdma',timeout=3600)
        assert 'GC reclamation PASS' in text and 'POSIX suite: 218/218 pass' in text
        state['stages'].append('live-TCP-RDMA-durability-and-GC-POSIX-PASS');save()
        quiet();state['current']='make-test-all';save()
        e=dict(env,EFS_TRANSPORT='rdma',EFS_TEST_TARGET='test-all',EFS_TEST_JOBS='8',EFS_BUILD_JOBS='8',
               TMPDIR=str(a.private/'full-tmp'),EFS_B6_KEEP='1',EFS_K35_KEEP='1')
        start=time.monotonic()
        with (a.output/'make-test-all.log').open('x') as f:
            rc=subprocess.run(['bash','scripts/make_test_summary.sh'],cwd=a.source,env=e,stdout=f,
                              stderr=subprocess.STDOUT,timeout=7200).returncode
        summary=(a.source/'summary.txt').read_text()
        state.update(gate_rc=rc,gate_summary=summary,gate_elapsed_s=time.monotonic()-start)
        if rc not in (0,3) or not summary.startswith('TOTAL ') or ' FAIL 0 ' not in summary:
            raise RuntimeError('full gate is not passing executed tests; preserve and diagnose: '+summary)
        state['verdict']='PASS_SCOPED' if rc==0 else 'PASS_EXECUTED_WITH_HOST_LIMITS'
    except BaseException:
        state.update(verdict='FAIL',error=traceback.format_exc());raise
    finally:save()


if __name__=='__main__':main()
