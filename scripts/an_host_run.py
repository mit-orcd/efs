#!/usr/bin/env python3
"""Visible owner of isolated AN build/pilot/matrix; never operates existing stores.

Run in detached screen. A command failure stops the sequence, preserving every
source, fixture and capture. No automatic retry or cleanup of retained trees.
"""
import argparse
import itertools
import json
import os
from pathlib import Path
import random
import shutil
import socket
import subprocess
import sys
import time


def main(a):
    base=a.base.resolve();src=base/'src';measure=base/'measure'
    assert socket.gethostname().split('.')[0]=='fcstor014'
    assert str(base).startswith('/tmp/efs-taskAN-')
    assert not measure.exists() and not a.out.exists()
    a.out.mkdir(parents=True)
    sys.path.insert(0,str(src/'scripts'))
    from task_i_host_sample import foreign_jobs
    with (a.out/'controller.log').open('w',buffering=1) as log:
        def stage(s):(a.out/'stage').write_text(s+'\n');print(s,flush=True)
        def run(name,argv,cwd=src,env=None,timeout=None):
            with (a.out/(name+'.log')).open('w') as f:
                f.write(json.dumps(argv)+'\n');f.flush()
                r=subprocess.run(argv,cwd=cwd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=timeout)
            (a.out/(name+'-exit')).write_text(str(r.returncode)+'\n')
            if r.returncode:raise RuntimeError(f'{name}: exit {r.returncode}; log retained')
        stage('PREFLIGHT')
        for i in range(2):
            jobs=foreign_jobs();assert not jobs,jobs
            assert 'efs-fuse' not in Path('/proc/self/mountinfo').read_text()
            with (a.out/'preflight.jsonl').open('a') as f:f.write(json.dumps(dict(t=time.time(),foreign=jobs))+'\n')
            if not i:time.sleep(20)
        env=dict(os.environ)
        for name in list(env):
            if name.startswith('EFS_') or name in ('LD_PRELOAD','ASAN_OPTIONS','LSAN_OPTIONS','TSAN_OPTIONS'):env.pop(name)
        prefix=Path.home()/'orcd/scratch/efs/orcd-test/deps/libfuse'
        lib=prefix/('lib64' if (prefix/'lib64/pkgconfig').is_dir() else 'lib')
        env.update(EFS_FUSE_PREFIX=str(prefix),PKG_CONFIG_PATH=str(lib/'pkgconfig'),
                   LD_LIBRARY_PATH=str(lib),EFS_GIT_ID=a.commit+'+ANprobe',
                   EFS_GIT_DESCRIBE=a.commit+'+ANprobe',EFS_GIT_BRANCH='agent/devel-task-an')
        monitor=subprocess.Popen([sys.executable,str(src/'scripts/task_i_host_sample.py'),str(base),
                                  str(a.out/'ownership.jsonl')],stdout=log,stderr=subprocess.STDOUT)
        try:
            stage('BUILD')
            shutil.copytree(src,measure)
            run('probe',[sys.executable,str(src/'scripts/an_put_probe.py'),str(measure)])
            flags=subprocess.check_output(['pkg-config','--libs','fuse3'],env=env,text=True).strip()
            run('build',['make','-j8','all','tests/faults/efsd',f'FUSE_LIBS={flags} -Wl,-rpath,{lib}'],measure,env)
            run('stat-build',['gcc','-O2','-g','-pthread','-Iinclude','-Isrc/client','-Isrc/common',
                             'scripts/an_stat_client.c','libefs.a','-lm','-ldl','-libverbs','-o','an-stat-client'],measure,env)
            run('identity',['./efsd','--version'],measure,env)
            run('environment',['bash','-c','hostname; uname -a; gcc --version; pkg-config --modversion fuse3; ibv_devinfo -d mlx5_2; df -h /tmp'],measure,env)
            stage('PILOT')
            def row(name,writers,transport,down,seconds):
                jobs=foreign_jobs(base);assert not jobs,jobs
                argv=[sys.executable,'scripts/an_put_measure.py','--work',str(base/'fixtures'/name),
                      '--out',str(a.out/name),'--writers',str(writers),'--transport',transport,
                      '--device','mlx5_2','--seconds',str(seconds)]
                if down:argv.append('--down')
                run(name,argv,measure,env,timeout=600)
                result=json.loads((a.out/name/'result.json').read_text())
                assert result['verdict']=='PASS',result
                observed=[json.loads(x) for x in (a.out/'ownership.jsonl').read_text().splitlines()]
                assert observed and not any(x['foreign_jobs'] or x['background_jobs'] for x in observed), 'host coverage failed; workload result is not an isolated measurement'
            row('pilot',1,'tcp',False,15)
            if a.matrix:
                stage('MATRIX')
                for repeat in range(3):
                    cases=list(itertools.product((1,8,32),('tcp','rdma'),(False,True)))
                    random.Random(20261029+repeat).shuffle(cases)
                    for writers,transport,down in cases:
                        name=f'r{repeat}-{transport}-w{writers}-'+('down' if down else 'healthy')
                        row(name,writers,transport,down,60)
            stage('FINISHED')
        finally:
            monitor.terminate();monitor.wait(timeout=10)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--commit',required=True);p.add_argument('--matrix',action='store_true')
    a=p.parse_args();rc=1
    try:main(a);rc=0
    finally:
        if a.out.exists():(a.out/'controller-exit').write_text(str(rc)+'\n')
