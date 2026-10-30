#!/usr/bin/env python3
"""Untouched product comparison for a failed AN pilot; counters unavailable.

Reuses the pristine source export, never the failed instrumented fixture.
Product C hashes must match the probe manifest's BEFORE hashes. The workload,
authority, byte checks and time bounds stay identical; only counter collection
is explicitly unavailable on this control. No failed attempt is overwritten.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time


def main(a):
    base=a.base.resolve();src=base/'src'
    assert socket.gethostname().split('.')[0]=='fcstor014'
    assert str(base).startswith('/tmp/efs-taskAN-') and not a.out.exists()
    assert not (src/'an-probe-manifest.json').exists()
    expected=json.loads((base/'measure/an-probe-manifest.json').read_text())['before']
    actual={n:hashlib.sha256((src/n).read_bytes()).hexdigest() for n in expected}
    assert actual==expected,'product source does not match untouched exported base'
    a.out.mkdir(parents=True)
    (a.out/'product-source.json').write_text(json.dumps(actual,indent=2)+'\n')
    sys.path.insert(0,str(src/'scripts'))
    from task_i_host_sample import foreign_jobs
    for i in range(2):
        jobs=foreign_jobs();assert not jobs,jobs
        assert 'efs-fuse' not in Path('/proc/self/mountinfo').read_text()
        with (a.out/'preflight.jsonl').open('a') as f:f.write(json.dumps(dict(t=time.time(),foreign=jobs))+'\n')
        if not i:time.sleep(20)
    env={n:v for n,v in os.environ.items() if not n.startswith('EFS_') and n!='LD_PRELOAD'}
    prefix=Path.home()/'orcd/scratch/efs/orcd-test/deps/libfuse'
    lib=prefix/('lib64' if (prefix/'lib64/pkgconfig').is_dir() else 'lib')
    env.update(EFS_FUSE_PREFIX=str(prefix),PKG_CONFIG_PATH=str(lib/'pkgconfig'),
               LD_LIBRARY_PATH=str(lib),EFS_GIT_ID=a.product_commit,
               EFS_GIT_DESCRIBE=a.product_commit,EFS_GIT_BRANCH='devel-untouched-AN-control')
    def run(name,cmd,cwd=src):
        with (a.out/(name+'.log')).open('w') as log:
            log.write(json.dumps(cmd)+'\n');log.flush()
            r=subprocess.run(cmd,cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=600)
        (a.out/(name+'-exit')).write_text(str(r.returncode)+'\n')
        return r.returncode
    with (a.out/'monitor.log').open('w') as log:
        monitor=subprocess.Popen([sys.executable,str(src/'scripts/task_i_host_sample.py'),str(base),
                                  str(a.out/'ownership.jsonl')],stdout=log,stderr=subprocess.STDOUT)
        try:
            (a.out/'stage').write_text('BUILD\n')
            flags=subprocess.check_output(['pkg-config','--libs','fuse3'],env=env,text=True).strip()
            assert run('build',['make','-j8','all','tests/faults/efsd',f'FUSE_LIBS={flags} -Wl,-rpath,{lib}'])==0
            assert run('stat-build',['gcc','-O2','-g','-pthread','-Iinclude','-Isrc/client','-Isrc/common',
                                    'scripts/an_stat_client.c','libefs.a','-lm','-ldl','-libverbs','-o','an-stat-client'])==0
            assert run('identity',['./efsd','--version'])==0
            (a.out/'stage').write_text('CONTROL\n')
            command=[sys.executable,'scripts/an_put_measure.py','--counterless-control',
                   '--work',str(base/'fixtures'/a.out.name),'--out',str(a.out/'control'),
                   '--writers','1','--transport','tcp','--device','mlx5_2','--seconds','15']
            if a.diagnostic:command.append('--diagnostic')
            if a.trace_store:command.append('--trace-store')
            rc=run('control',command)
            (a.out/'stage').write_text('FINISHED\n')
            return rc
        finally:
            monitor.terminate();monitor.wait(timeout=10)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--product-commit',required=True)
    p.add_argument('--diagnostic',action='store_true')
    p.add_argument('--trace-store',action='store_true')
    a=p.parse_args();rc=1
    try:rc=main(a)
    finally:
        if a.out.exists():(a.out/'controller-exit').write_text(str(rc)+'\n')
    raise SystemExit(rc)
