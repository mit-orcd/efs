#!/usr/bin/env python3
"""Strict AN native qualification after the measurement host becomes idle.

Never runs alongside a matrix, never modifies an existing fixture, never
retries a failed command. Logs and all explicit test KEEP fixtures stay local.
Full-gate host NOTRUNs retain exit 3 and require individual review.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time


def main(a):
    src=a.source.resolve();out=a.out.resolve()
    assert socket.gethostname().split('.')[0]=='fcstor014'
    assert str(src).startswith('/tmp/efs-taskAN-') and src.name=='src'
    assert not (src/'an-probe-manifest.json').exists(), 'native source required'
    assert not out.exists()
    assert int((a.prior/'controller-exit').read_text())==0, 'matrix must close successfully first'
    sys.path.insert(0,str(src/'scripts'))
    from task_i_host_sample import foreign_jobs
    for i in range(2):
        jobs=foreign_jobs();assert not jobs,jobs
        assert 'efs-fuse' not in Path('/proc/self/mountinfo').read_text()
        if not i:time.sleep(20)
    out.mkdir(parents=True)
    scratch=src.parent/'qualification-tmp';scratch.mkdir()
    env={n:v for n,v in os.environ.items() if not n.startswith('EFS_') and n!='LD_PRELOAD'}
    prefix=Path.home()/'orcd/scratch/efs/orcd-test/deps/libfuse'
    lib=prefix/('lib64' if (prefix/'lib64/pkgconfig').is_dir() else 'lib')
    env.update(EFS_FUSE_PREFIX=str(prefix),PKG_CONFIG_PATH=str(lib/'pkgconfig'),LD_LIBRARY_PATH=str(lib),
               EFS_GIT_ID=a.commit,EFS_GIT_DESCRIBE=a.commit,EFS_GIT_BRANCH='agent/devel-task-an',
               EFS_TEST_JOBS='8',EFS_BUILD_JOBS='8',EFS_TEST_TARGET='test-all',EFS_RDMA_DEV='mlx5_2',EFS_TRANSPORT=a.transport)
    env['TMPDIR']=str(scratch)
    if a.cluster_scripts:
        cluster=a.cluster_scripts.resolve()
        assert cluster.is_relative_to(src.parent), 'only a private exported dependency'
        names=('cluster.sh','ct-up.sh','gw-up.sh','lib/client-stop.sh','lib/client-user.sh','lib/transfer.sh')
        hashes={name:hashlib.sha256((cluster/name).read_bytes()).hexdigest() for name in names}
        (out/'cluster-scripts.json').write_text(json.dumps(hashes,indent=2)+'\n')
        env['EFS_CLUSTER_SCRIPTS']=str(cluster)
    if a.private_tools:
        tools=a.private_tools.resolve()
        assert str(tools).startswith('/tmp/efs-taskAN-deps/')
        assert (tools/'bin/gcc').is_file() and (tools/'usr/include/valgrind/memcheck.h').is_file()
        assert (tools/'usr/lib64/libtsan.so.0').is_file()
        env.update(PATH=str(tools/'bin')+':'+env['PATH'],CC=str(tools/'bin/gcc'),
                   CPATH=str(tools/'usr/include'),
                   LD_LIBRARY_PATH=str(tools/'usr/lib64')+':'+str(lib))
    # Existing test-only retention switches change cleanup, never admission,
    # fault schedules, workload, timeout or durability policy.
    keep=set()
    for file in (src/'tests').glob('*.py'):
        keep.update(re.findall(r'\bEFS_[A-Z0-9_]*KEEP\b',file.read_text()))
    env.update({n:'1' for n in keep})
    (out/'retention-flags.json').write_text(json.dumps(sorted(keep),indent=2)+'\n')
    flags=subprocess.check_output(['pkg-config','--libs','fuse3'],env=env,text=True).strip()
    env['FUSE_LIBS']=flags+f' -Wl,-rpath,{lib}'
    monitor=subprocess.Popen([sys.executable,str(src/'scripts/task_i_host_sample.py'),str(src.parent),
                              str(out/'ownership.jsonl')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    def run(name,cmd,limit=1800):
        (out/'stage').write_text(name+'\n')
        def receipt(event,**extra):
            with (out/'stages.jsonl').open('a') as f:
                f.write(json.dumps(dict(stage=name,event=event,t=time.time(),mono_ns=time.monotonic_ns(),command=cmd,**extra))+'\n')
        receipt('start')
        with (out/(name+'.log')).open('w') as log:
            log.write(json.dumps(cmd)+'\n');log.flush()
            result=subprocess.run(cmd,cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=limit)
        (out/(name+'-exit')).write_text(str(result.returncode)+'\n')
        receipt('end',exit=result.returncode)
        return result.returncode
    def named(name,cmd):
        assert run(name,cmd,900)==0
        assert not re.search(r'\b(?:NOTRUN|SKIP)\b',(out/(name+'.log')).read_text()), 'named check did not run fully'
    try:
        assert run('build',['make','-j8','all','tests/faults/efsd'])==0
        assert run('identity',['./efsd','--version'])==0
        if a.sanitizer_preflight:
            for name in ('test_session_latch_tsan','test_apply_ring','test_rdma_memcheck'):
                named(name,[sys.executable,f'tests/{name}.py'])
            binary=out/'commit-tsan'
            named('commit-tsan-build',[env.get('CC','gcc'),'-std=gnu11','-O1','-g',
                  '-Wall','-Wextra','-Werror','-pthread','-fsanitize=thread',
                  '-Iinclude','-Isrc/client','-I.','tests/test_data_commit.c','-o',str(binary)])
            env['TSAN_OPTIONS']='halt_on_error=1 exitcode=66'
            named('commit-tsan',[str(binary),'slow-commit'])
            env.pop('TSAN_OPTIONS',None)
        if a.k38_diagnostic_repeats:
            # Predeclared fresh diagnostic fixtures, stop on first failure.
            # Neither a passing sample nor this branch is qualification.
            for repeat in range(1, a.k38_diagnostic_repeats + 1):
                named(f'k38-diagnostic-{repeat}',
                      [sys.executable, 'tests/test_k38_shared_chunk_pressure.py'])
            (out/'stage').write_text('K38_DIAGNOSTIC_FINISHED; qualification NOTRUN\n')
            return 0
        if a.slow_only:
            env['EFS_B6_MODES']='slow'
            for transport in ('tcp','rdma'):
                env['EFS_TRANSPORT']=transport
                named(transport+'-b6-slow',[sys.executable,'tests/test_b6_unavailable.py'])
            (out/'stage').write_text('SLOW_FINISHED; full gate and named repeats NOTRUN\n')
            return 0
        # test-all includes the complete fast suite; the focused production
        # store tests already ran during repair, so do not run it twice here.
        gate=run('gate',['bash','scripts/make_test_summary.sh'],3600)
        for name in ('summary.txt','timings.tsv','progress.txt','build.log','enum.log','cmds.txt','cmds.raw'):
            if (src/name).exists():shutil.copyfile(src/name,out/name)
        for file in (src/'tmp').glob('fail-*.log'):shutil.copyfile(file,out/file.name)
        assert run('identity-after-gate',['./efsd','--version'])==0
        hashes={}
        for name in ('efsd','efs-fuse','tests/faults/efsd','src/client/data_commit.c',
                     'src/server/store.c','src/server/raft_host.c','src/server/put_upload.c'):
            h=hashlib.sha256()
            with (src/name).open('rb') as source:
                for block in iter(lambda:source.read(1<<20),b''):h.update(block)
            hashes[name]=h.hexdigest()
        (out/'native-identity.json').write_text(json.dumps(hashes,indent=2)+'\n')
        assert gate in (0,3), 'full gate FAIL; retained, no retry'
        # Preserve individual hardware transport receipts as well as the
        # full gate's aggregate status, including the zero-app-TCP checks.
        env['EFS_TRANSPORT']='auto'
        named('native-declaration',['./tests/test_declaration_gate'])
        named('native-rdma-xprt',['./tests/test_rdma_xprt'])
        for mode in ('normal','interrupt','tcp-violation','deadline','burst','burst-asym','churn'):
            named('native-rdma-'+mode,['./tests/test_rdma_segmented',mode])
        if a.gate_only:
            (out/'stage').write_text('GATE_FINISHED; named repeats NOTRUN\n')
            return gate
        for transport in ('tcp','rdma'):
            env['EFS_TRANSPORT']=transport
            for repeat in range(1,6):
                for name in ('test_k35_fsync_degraded','test_k33_payload'):
                    named(f'{transport}-{name}-{repeat}',[sys.executable,f'tests/{name}.py'])
                if transport=='tcp':
                    named(f'tcp-w166-{repeat}',[sys.executable,'tests/test_w166_authority.py'])
        assert not any(str(src.parent)+'/' in line for line in Path('/proc/self/mountinfo').read_text().splitlines()), 'owned mount remains'
        (out/'stage').write_text('FINISHED\n')
        return gate
    finally:
        monitor.terminate();monitor.wait(timeout=10)
        rows=[json.loads(line) for line in (out/'ownership.jsonl').read_text().splitlines()]
        assert rows and not any(row['foreign_jobs'] or row['background_jobs'] for row in rows), 'qualification host coverage failed'
        assert not any(str(src.parent)+'/' in line for line in Path('/proc/self/mountinfo').read_text().splitlines()), 'owned mount remains'


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True);p.add_argument('--prior',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--commit',required=True)
    p.add_argument('--gate-only',action='store_true',help='early correctness gate only; all named repeats remain NOTRUN')
    p.add_argument('--slow-only',action='store_true',help='only the unchanged B6 slow-fsync oracle on TCP and RDMA')
    p.add_argument('--k38-diagnostic-repeats',type=int,choices=range(1,6),default=0,
                   help='diagnostic only: 1..5 fresh unchanged K38 runs, stop on first failure')
    p.add_argument('--private-tools',type=Path,help='verified private compiler wrapper/runtime and Valgrind headers')
    p.add_argument('--cluster-scripts',type=Path,help='private committed cluster script export; test uses isolated fake transports')
    p.add_argument('--transport',choices=('auto','tcp','rdma'),default='auto')
    p.add_argument('--sanitizer-preflight',action='store_true',help='strict sanitizer/Valgrind positive-negative controls before live checks')
    a=p.parse_args();rc=1
    assert sum(map(bool,(a.gate_only,a.slow_only,a.k38_diagnostic_repeats))) <= 1
    try:rc=main(a)
    finally:
        if a.out.exists():(a.out/'controller-exit').write_text(str(rc)+'\n')
    raise SystemExit(rc)
