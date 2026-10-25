#!/usr/bin/env python3
"""Reproducible Task C ASan/UBSan mutation gate. Raw inputs remain external.

The build tree is separate so this gate cannot silently reuse uninstrumented
objects or alter an existing deployment. Each target writes its current input
before decoding; failures retain the exact bytes and do not stop other targets.
"""
import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('targets', ROOT/'tests/test_decoder_inputs.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--seconds', type=int, default=1200)
p.add_argument('--extended-seconds', type=int, default=7200)
p.add_argument('--jobs', type=int, default=2)
p.add_argument('--out', type=Path)
p.add_argument('--targets', nargs='+', choices=(*module.TARGETS, 'apply'), default=(*module.TARGETS, 'apply'))
a = p.parse_args()
if min(a.seconds,a.extended_seconds,a.jobs) < 1: p.error('durations and jobs must be positive')
out = (a.out or Path(tempfile.mkdtemp(prefix='efs-decoders-'))).resolve()
out.mkdir(parents=True,exist_ok=True)
build = out/'build'
if build.exists(): p.error('output already contains a build; use a fresh output directory')
shutil.copytree(ROOT,build,ignore=shutil.ignore_patterns('.git','artifacts','results','*.o','*.a','__pycache__'))
flags='-O1 -g -std=c99 -D_GNU_SOURCE -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all'
env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:detect_leaks=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
with (out/'build.log').open('w') as f:
    result=subprocess.run(['make','-j'+str(a.jobs),'decoder-bins','CC='+os.environ.get('CC','gcc'),
        'CFLAGS='+flags,'LDFLAGS=-lpthread -lm -ldl -libverbs -fsanitize=address,undefined'],cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT)
if result.returncode: raise SystemExit('FAIL: sanitized decoder build; '+str(out/'build.log'))
def run(target):
    work=out/target;work.mkdir()
    duration=a.extended_seconds if target in ('declaration','apply') else a.seconds
    start=time.monotonic()
    with (work/'run.log').open('w') as f:
        try:
            command=([str(build/'tests/decoders/apply'),str(duration)] if target=='apply' else
                     [str(build/'tests/decoders/check'),target,str(duration),str(work)])
            r=subprocess.run(command,env=env,
                stdout=f,stderr=subprocess.STDOUT,timeout=duration+60)
            rc=r.returncode
        except subprocess.TimeoutExpired: rc=124
    return {'target':target,'rc':rc,'seconds':round(time.monotonic()-start,3)}
with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
    rows=list(pool.map(run,a.targets))
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
files=sorted(p for p in out.rglob('*') if p.is_file() and build not in p.parents and p.name!='SHA256SUMS')
(out/'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+str(f.relative_to(out))+'\n' for f in files))
for row in rows: print(('PASS' if row['rc']==0 else 'FAIL')+' '+json.dumps(row))
print('Evidence: '+str(out),flush=True)
raise SystemExit(any(row['rc'] for row in rows))
