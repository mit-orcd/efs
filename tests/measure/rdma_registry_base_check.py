#!/usr/bin/env python3
"""One untouched-base B6 slow/full check, only after the Task AI full gate.

No performance rerun, no oracle changes, no retry-to-green. Independent source
export is required. Failures and fixture paths remain on the assigned host.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time
import traceback


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('private',type=Path); ap.add_argument('base',type=Path)
    ap.add_argument('prior',type=Path); ap.add_argument('out',type=Path)
    a=ap.parse_args(); a.private=a.private.resolve(); a.base=a.base.resolve(); a.out=a.out.resolve()
    if a.out.exists() or not all(str(p).startswith(str(a.private)+'/') for p in (a.base,a.out)):
        ap.error('fresh output and base below private root required')
    a.out.mkdir(); state=dict(verdict='WAITING_GATE',commands=[])
    def save(): (a.out/'state.json').write_text(json.dumps(state,indent=2)+'\n')
    save()
    tools=Path(__file__).resolve().parent
    spec=importlib.util.spec_from_file_location('cluster',tools/'rdma_registry_cluster.py')
    helper=importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    def quiet():
        found=helper.foreign_work(a.private)
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit(): continue
            try: argv=[x.decode(errors='replace') for x in (proc/'cmdline').read_bytes().split(b'\0') if x]
            except FileNotFoundError: continue
            if argv and Path(argv[0]).name in ('efsd','efs-fuse') and any(str(a.private) in v for v in argv):
                found.append(dict(pid=int(proc.name),owned_residual=argv[:4]))
        if found: raise RuntimeError('test host not clear: '+str(found))
    env=dict(os.environ,EFS_GIT_ID='486aa3a46',EFS_GIT_DESCRIBE='486aa3a46',
             EFS_GIT_BRANCH='devel-base-taskAI',EFS_RDMA_DEV='mlx5_0',EFS_TRANSPORT='rdma')
    def run(name,cmd,allowed=(0,),timeout=900):
        quiet(); state.update(verdict='RUNNING',current=name); save()
        with (a.out/(name+'.log')).open('x') as log:
            log.write(json.dumps(dict(argv=list(map(str,cmd)),cwd=str(a.base)))+'\n');log.flush()
            start=time.monotonic()
            rc=subprocess.run(list(map(str,cmd)),cwd=a.base,env=env,stdout=log,
                              stderr=subprocess.STDOUT,timeout=timeout).returncode
        state['commands'].append(dict(name=name,rc=rc,elapsed_s=time.monotonic()-start));save()
        if rc not in allowed:raise RuntimeError(f'{name}: {rc}')
    try:
        deadline=time.monotonic()+14400
        while True:
            old=json.loads((a.prior/'state.json').read_text())
            if old['verdict'] not in ('RUNNING','WAITING_MATRIX'):break
            if time.monotonic()>deadline:raise RuntimeError('bounded gate wait expired')
            time.sleep(30)
        summary=(a.private/'native/summary.txt').read_text()
        if not summary.startswith('TOTAL ') or 'TOTAL 0 ' in summary:
            raise RuntimeError('prior full gate did not finish real test commands')
        state['prior_gate_summary']=summary
        # Compare every product input plus the exact failed test/helpers before
        # building the independent base. Build ids intentionally differ.
        files=[Path('Makefile'),Path('tests/test_b6_unavailable.py'),Path('tests/k33_daemons.py')]
        for folder in ('src','include'):
            files += [p.relative_to(a.base) for p in (a.base/folder).rglob('*') if p.suffix in ('.c','.h')]
        h=hashlib.sha256()
        for path in sorted(files):
            b=(a.base/path).read_bytes()
            if b!=(a.private/'native'/path).read_bytes():raise RuntimeError('base input differs: '+str(path))
            h.update(str(path).encode()+b'\0'+b)
        state.update(base_source='486aa3a46',native_source='d1bf8e2bd',
                     identical_inputs=len(files),input_sha256=h.hexdigest(),
                     comparison_limit='One serial check/mode; not a repeat of the 8-job full gate')
        prefix=Path(env['EFS_FUSE_PREFIX'])
        libs=subprocess.check_output(['pkg-config','--libs','fuse3'],env=env,text=True).strip()
        run('build',['make','-j8',f'FUSE_LIBS={libs} -Wl,-rpath,{prefix}/lib',
                     'all','tests/faults/efsd'],timeout=1800)
        tmp=a.private/'base-tmp';tmp.mkdir();env['TMPDIR']=str(tmp)
        for mode in ('slow','full'):
            env.update(EFS_B6_MODES=mode,EFS_B6_ARTIFACTS=str(a.out/('artifacts-'+mode)))
            run('b6-'+mode,['python3','tests/test_b6_unavailable.py'],allowed=(0,1),timeout=600)
        state['verdict']='COMPLETE_RECORDED'
    except BaseException:
        state.update(verdict='FAIL',error=traceback.format_exc());raise
    finally:save()


if __name__=='__main__':main()
