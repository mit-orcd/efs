#!/usr/bin/env python3
"""Closed-matrix observer calibration on fresh fixtures, never alongside a run.

Three repeats at the low/high writer counts on both transports compare
native OBS=0, native OBS=1 and temporary-probe OBS=1. This measures observer
interference in application/RPC latency and CPU, not an authority optimization.
Degraded-state calibration is not claimed by these healthy controls.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import socket
import statistics
import subprocess
import sys
import time


def main(a):
    base=a.base.resolve();src=base/'src';probe=base/'measure';out=a.out.resolve()
    assert socket.gethostname().split('.')[0]=='fcstor014'
    assert str(base).startswith('/tmp/efs-taskAN-') and not out.exists()
    assert int((a.prior/'controller-exit').read_text())==0
    assert not (src/'an-probe-manifest.json').exists()
    manifest=json.loads((probe/'an-probe-manifest.json').read_text())
    for tree,kind in ((src,'before'),(probe,'after')):
        for name,want in manifest[kind].items():
            assert hashlib.sha256((tree/name).read_bytes()).hexdigest()==want,(tree,name)
    sys.path.insert(0,str(src/'scripts'))
    from task_i_host_sample import foreign_jobs
    for i in range(2):
        assert not foreign_jobs()
        assert 'efs-fuse' not in Path('/proc/self/mountinfo').read_text()
        if not i:time.sleep(20)
    out.mkdir(parents=True)
    env={n:v for n,v in os.environ.items() if not n.startswith('EFS_') and
         n not in ('LD_PRELOAD','ASAN_OPTIONS','LSAN_OPTIONS','TSAN_OPTIONS')}
    prefix=Path.home()/'orcd/scratch/efs/orcd-test/deps/libfuse'
    lib=prefix/('lib64' if (prefix/'lib64/pkgconfig').is_dir() else 'lib')
    env.update(EFS_FUSE_PREFIX=str(prefix),PKG_CONFIG_PATH=str(lib/'pkgconfig'),
               LD_LIBRARY_PATH=str(lib),EFS_GIT_ID=a.commit+'+native-calibration',
               EFS_GIT_DESCRIBE=a.commit+'+native-calibration',EFS_GIT_BRANCH='agent/devel-task-an')
    def run(name,cmd,tree,limit=900):
        (out/'stage').write_text(name+'\n')
        with (out/(name+'.log')).open('w') as log:
            log.write(json.dumps(cmd)+'\n');log.flush()
            r=subprocess.run(cmd,cwd=tree,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=limit)
        (out/(name+'-exit')).write_text(str(r.returncode)+'\n')
        assert r.returncode==0,(name,r.returncode)
    monitor=subprocess.Popen([sys.executable,str(src/'scripts/task_i_host_sample.py'),str(base),
                              str(out/'ownership.jsonl')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    cells=[]
    try:
        flags=subprocess.check_output(['pkg-config','--libs','fuse3'],env=env,text=True).strip()
        run('native-build',['make','-j8','all','tests/faults/efsd',f'FUSE_LIBS={flags} -Wl,-rpath,{lib}'],src)
        run('native-stat-build',['gcc','-O2','-g','-pthread','-Iinclude','-Isrc/client','-Isrc/common',
            'scripts/an_stat_client.c','libefs.a','-lm','-ldl','-libverbs','-o','an-stat-client'],src)
        for label,tree in (('native',src),('probe',probe)):
            run(label+'-identity',['./efsd','--version'],tree)
        for repeat in range(3):
            cases=list(itertools.product((1,32),('tcp','rdma'),('native-off','native-on','probe-on')))
            random.Random(20261030+repeat).shuffle(cases)
            for writers,transport,mode in cases:
                assert not foreign_jobs(base)
                name=f'r{repeat}-{transport}-w{writers}-{mode}'
                tree=probe if mode=='probe-on' else src
                cmd=[sys.executable,'scripts/an_put_measure.py','--work',str(base/'calibration-fixtures'/name),
                     '--out',str(out/name),'--writers',str(writers),'--transport',transport,
                     '--device','mlx5_2','--seconds','60','--observer','0' if mode=='native-off' else '1']
                if mode!='probe-on':cmd.append('--counterless-control')
                run(name,cmd,tree)
                d=json.loads((out/name/'result.json').read_text())
                assert d['verdict']=='PASS' and not d['errors'] and not d.get('cleanup_errors')
                cells.append(dict(name=name,repeat=repeat,mode=mode,writers=writers,transport=transport,
                                  **{k:d[k] for k in ('write_p95_s','write_p99_s','stat_rpc_p50_us',
                                      'stat_rpc_p99_us','throughput_Bps','cpu_seconds','fuse_cpu_seconds')}))
        comparisons=[]
        metrics=('write_p95_s','write_p99_s','stat_rpc_p50_us','stat_rpc_p99_us','throughput_Bps','fuse_cpu_seconds')
        for writers,transport,mode in itertools.product((1,32),('tcp','rdma'),('native-off','native-on','probe-on')):
            rows=[c for c in cells if (c['writers'],c['transport'],c['mode'])==(writers,transport,mode)]
            assert len(rows)==3
            values={k:[r[k] for r in rows] for k in metrics}
            for node in ('1','2','3'):values['n'+node+'_cpu_s']=[r['cpu_seconds'][node] for r in rows]
            comparisons.append(dict(writers=writers,transport=transport,mode=mode,
                metrics={k:dict(median=statistics.median(v),minimum=min(v),maximum=max(v)) for k,v in values.items()}))
        effects=[]
        for writers,transport in itertools.product((1,32),('tcp','rdma')):
            rows={r['mode']:r['metrics'] for r in comparisons if (r['writers'],r['transport'])==(writers,transport)}
            for before,after in (('native-off','native-on'),('native-on','probe-on')):
                values={}
                for metric,old in rows[before].items():
                    new=rows[after][metric]
                    values[metric]=dict(median_delta=new['median']-old['median'],
                        median_ratio=new['median']/old['median'] if old['median'] else None,
                        disjoint_repeat_ranges=(new['minimum']>old['maximum'] or new['maximum']<old['minimum']))
                effects.append(dict(writers=writers,transport=transport,before=before,after=after,metrics=values))
        (out/'summary.json').write_text(json.dumps(dict(cells=cells,comparisons=comparisons,effects=effects,
            scope='healthy 1/32 writers, both transports, three repeats; observer calibration only'),indent=2)+'\n')
        (out/'stage').write_text('FINISHED\n')
    finally:
        monitor.terminate();monitor.wait(timeout=10)
        rows=[json.loads(s) for s in (out/'ownership.jsonl').read_text().splitlines()]
        assert rows and not any(r['foreign_jobs'] or r['background_jobs'] for r in rows)
        assert not any(str(base)+'/' in s for s in Path('/proc/self/mountinfo').read_text().splitlines())


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--prior',type=Path,required=True);p.add_argument('--commit',required=True)
    a=p.parse_args();rc=1
    try:main(a);rc=0
    finally:
        if a.out.exists():(a.out/'controller-exit').write_text(str(rc)+'\n')
    raise SystemExit(rc)
