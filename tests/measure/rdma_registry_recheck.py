#!/usr/bin/env python3
"""Run one corrected full gate after AI's independent base checks finish.

Requires a completed fresh source export and a working, private matching TSan
runtime. Keeps the original failing gate intact. No product configuration,
timeouts, test oracle or system library is changed.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time
import traceback


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('private','source','base_check','output'):p.add_argument(name,type=Path)
    p.add_argument('commit');a=p.parse_args();a.private=a.private.resolve()
    if a.output.exists() or any(not str(x.resolve()).startswith(str(a.private)+'/')
                               for x in (a.source,a.base_check,a.output)):
        p.error('fresh output and owned paths below private root required')
    a.output.mkdir();state=dict(verdict='WAITING_BASE_CHECK',source=a.commit)
    def save():(a.output/'state.json').write_text(json.dumps(state,indent=2)+'\n')
    save()
    try:
        deadline=time.monotonic()+14400
        while True:
            base=json.loads((a.base_check/'state.json').read_text())
            if base['verdict'] not in ('WAITING_GATE','RUNNING'):
                if base['verdict']!='COMPLETE_RECORDED':raise RuntimeError('base comparison incomplete: '+str(base))
                break
            if time.monotonic()>deadline:raise RuntimeError('bounded base wait expired')
            time.sleep(30)
        assert (a.source/'EXPORT_COMPLETE').read_text().strip()==a.commit
        spec=importlib.util.spec_from_file_location('cluster',Path(__file__).with_name('rdma_registry_cluster.py'))
        helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
        found=helper.foreign_work(a.private)
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():continue
            try:argv=[x.decode(errors='replace') for x in (proc/'cmdline').read_bytes().split(b'\0') if x]
            except FileNotFoundError:continue
            if argv and Path(argv[0]).name in ('efsd','efs-fuse') and any(str(a.private) in v for v in argv):
                found.append(dict(pid=int(proc.name),owned_residual=argv[:4]))
        if found:raise RuntimeError('host has unresolved EFS/test work: '+str(found))
        mounts=[s for s in Path('/proc/self/mountinfo').read_text().splitlines() if str(a.private) in s]
        if mounts:raise RuntimeError('owned mount remains: '+str(mounts))
        cc=a.private/'deps/gcc-private-tsan';lib=a.private/'deps/tsan-root/usr/lib64'
        assert cc.is_file() and (lib/'libtsan.so.0.0.0').is_file()
        tmp=a.private/'recheck-tmp';tmp.mkdir()
        env=dict(os.environ,CC=str(cc),EFS_GIT_ID=a.commit,EFS_GIT_DESCRIBE=a.commit,
                 EFS_GIT_BRANCH='agent/devel-task-ai',EFS_TEST_TARGET='test-all',
                 EFS_TEST_JOBS='8',EFS_BUILD_JOBS='8',EFS_TRANSPORT='rdma',
                 EFS_RDMA_DEV='mlx5_0',TMPDIR=str(tmp),
                 LD_LIBRARY_PATH=str(lib)+':'+os.environ.get('LD_LIBRARY_PATH',''))
        state.update(verdict='RUNNING',current='make-test-all',base_check=base);save()
        started=time.monotonic()
        with (a.output/'make-test-all.log').open('x') as log:
            rc=subprocess.run(['bash',str(a.source/'scripts/make_test_summary.sh')],cwd=a.source,
                              env=env,stdout=log,stderr=subprocess.STDOUT,timeout=7200).returncode
        summary=(a.source/'summary.txt').read_text()
        state.update(verdict='COMPLETE_RECORDED',rc=rc,elapsed_s=time.monotonic()-started,
                     summary=summary,gate_pass=rc==0)
    except BaseException:
        state.update(verdict='FAIL',error=traceback.format_exc());raise
    finally:save()


if __name__=='__main__':main()
