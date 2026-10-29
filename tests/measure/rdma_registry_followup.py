#!/usr/bin/env python3
"""Task AI continuation: wait for the existing matrix, then churn and gates.

Never launches or retries the IOR matrix. Stops on its failure, a live foreign
workload, or a failed follow-up. Raw output remains below the private root.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import signal
import time
import traceback


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('private',type=Path); p.add_argument('matrix',type=Path)
    p.add_argument('output',type=Path)
    a=p.parse_args(); private=a.private.resolve(); out=a.output.resolve()
    tools=Path(__file__).resolve().parent
    if out.exists() or not str(out).startswith(str(private)+'/'):
        p.error('fresh private output required')
    out.mkdir(); state={'verdict':'WAITING_MATRIX','commands':[]}
    def save(): (out/'state.json').write_text(json.dumps(state,indent=2)+'\n')
    save()
    spec=importlib.util.spec_from_file_location('ai_cluster',tools/'rdma_registry_cluster.py')
    helper=importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    env=dict(os.environ,EFS_RDMA_DEV='mlx5_0',TMPDIR=str(private/'tmp'),
             EFS_GIT_ID='d1bf8e2bd',EFS_GIT_DESCRIBE='d1bf8e2bd',
             EFS_GIT_BRANCH='agent/devel-task-ai')
    def quiet():
        foreign=helper.foreign_work(private)
        if foreign: raise RuntimeError(f'foreign work/unknown state: {foreign}')
    def run(name,cmd,cwd=None,transport='rdma',timeout=600,allowed=(0,)):
        quiet(); state.update(verdict='RUNNING',current=name); save()
        start=time.monotonic()
        with (out/(name+'.log')).open('x') as f:
            f.write(json.dumps({'argv':list(map(str,cmd)),'cwd':str(cwd),'transport':transport})+'\n'); f.flush()
            rc=subprocess.run(list(map(str,cmd)),cwd=cwd,env=dict(env,EFS_TRANSPORT=transport),
                              stdout=f,stderr=subprocess.STDOUT,timeout=timeout).returncode
        state['commands'].append(dict(name=name,rc=rc,elapsed_s=time.monotonic()-start)); save()
        if rc not in allowed: raise RuntimeError(f'{name}: rc {rc}; retained output, no retry')
    try:
        deadline=time.monotonic()+14400
        while True:
            m=json.loads((a.matrix/'state.json').read_text())
            if m['verdict']!='RUNNING':
                if m['verdict']!='PASS_SCOPED': raise RuntimeError('matrix did not pass: '+str(m))
                break
            if time.monotonic()>deadline: raise RuntimeError('bounded matrix wait expired')
            time.sleep(30)
        quiet()
        native=private/'native'; probe=private/'probe'
        cc=['cc','-O3','-g','-fno-omit-frame-pointer','-std=gnu11','-D_GNU_SOURCE',
            '-Wall','-Wextra','-Werror','-I'+str(native/'include'),'-I'+str(native/'src/common')]
        source=tools/'rdma_registry_streams.c'
        for label,tree in (('native',native),('probe',probe)):
            run('build-streams-'+label,cc+[source,tree/'libefs.a','-lpthread','-lm','-ldl','-libverbs',
                                         '-o',out/('streams-'+label)])
        run('generate-wake',['python3',tools/'rdma_registry_probe.py','wake',
                             probe/'src/common/rdma.c',out/'wake-rdma.c'])
        run('build-wake-object',cc+['-c',out/'wake-rdma.c','-o',out/'rdma.o'])
        # ar replaces the original rdma.o member, preserving every other object.
        (out/'wake-libefs.a').write_bytes((probe/'libefs.a').read_bytes())
        run('archive-wake-object',['ar','r',out/'wake-libefs.a',out/'rdma.o'])
        run('build-streams-wake',cc+[source,out/'wake-libefs.a','-lpthread','-lm','-ldl','-libverbs',
                                    '-o',out/'streams-wake'])
        run('streams-tcp-smoke',[out/'streams-native','1','1000','2'],transport='tcp')
        run('streams-rdma-smoke',[out/'streams-native','1','1000','2'])
        for repeat in (1,2,3):
            for streams in (1,8,32):
                for label,transport in (('native','tcp'),('native','rdma'),('probe','rdma'),('wake','rdma')):
                    run(f'streams-{label}-{transport}-{streams}-r{repeat}',
                        [out/('streams-'+label),str(streams),'25000','32'],transport=transport,timeout=600)
        for mode in ('normal','interrupt','tcp-violation','deadline','burst','burst-asym','churn'):
            run('segmented-'+mode,[native/'tests/test_rdma_segmented',mode])
        run('rdma-xprt',[native/'tests/test_rdma_xprt'])
        for test in ('test_rdma_handoff.py','test_rdma_progress_ready.py','test_rdma_fork_poller.py'):
            run(test.removesuffix('.py'),['python3',native/'tests'/test],timeout=600)
        roots=[private/'gc-a',private/'gc-b']
        for r in roots: r.mkdir()
        run('gc-posix-rdma',['python3',native/'tests/live/gc_reclamation.py','--transport','rdma',
                            '--root',roots[0],'--root',roots[1],'--port','19870','--posix'],timeout=3600)
        # The existing summary runner retains all FAIL and NOTRUN classifications.
        env.update(EFS_TEST_TARGET='test-all',EFS_TEST_JOBS='8',EFS_BUILD_JOBS='8')
        run('make-test-all',['bash',native/'scripts/make_test_summary.sh'],cwd=native,
            timeout=7200,allowed=(0,3))
        state['gate_summary']=(native/'summary.txt').read_text()
        state['verdict']='COMPLETE_SCOPED' if state['commands'][-1]['rc']==0 else 'INCOMPLETE_HOST_GATES'
    except BaseException:
        state.update(verdict='FAIL',error=traceback.format_exc()); raise
    finally: save()


if __name__=='__main__': main()
