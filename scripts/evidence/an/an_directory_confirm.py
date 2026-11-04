#!/usr/bin/env python3
"""Five fresh W130/K48 confirmations after a reviewed native AN protocol.

No build, original-store access or retry. Stops at the first failed check and
retains each test's fixture. Run only on the approved isolated fcstor014.
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

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--prior',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();src=a.source.resolve();out=a.out.resolve();rc=1
assert socket.gethostname().split('.')[0]=='fcstor014'
assert str(src).startswith('/tmp/efs-taskAN-') and src.name=='src' and not out.exists()
assert (a.prior/'controller-exit').read_text().strip()=='0'
assert json.loads((a.prior/'closed-review.json').read_text())['named_checks']==25
for name,want in json.loads((a.prior/'native-identity.json').read_text()).items():
    assert hashlib.sha256((src/name).read_bytes()).hexdigest()==want,name
sys.path.insert(0,str(src/'scripts'))
from task_i_host_sample import foreign_jobs
for i in range(2):
    jobs=foreign_jobs();assert not jobs,jobs
    if not i:time.sleep(20)
out.mkdir(parents=True)
scratch=src.parent/(out.name+'-tmp');scratch.mkdir()
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_') and k not in ('LD_PRELOAD','ASAN_OPTIONS','LSAN_OPTIONS','TSAN_OPTIONS')}
prefix=Path.home()/'orcd/scratch/efs/orcd-test/deps/libfuse'
lib=prefix/('lib64' if (prefix/'lib64/pkgconfig').is_dir() else 'lib')
assert lib.is_dir()
env.update(EFS_TRANSPORT='tcp',EFS_K33_KEEP='1',TMPDIR=str(scratch),
           EFS_FUSE_PREFIX=str(prefix),PKG_CONFIG_PATH=str(lib/'pkgconfig'),LD_LIBRARY_PATH=str(lib))
monitor=subprocess.Popen([sys.executable,str(src/'scripts/task_i_host_sample.py'),str(src.parent),str(out/'ownership.jsonl')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
try:
    for repeat in range(1,6):
        for label,test in (('w130','test_w130_dir_fault'),('k48','test_k48_readdir')):
            name=f'{label}-{repeat}';(out/'stage').write_text(name+'\n')
            command=[sys.executable,f'tests/{test}.py']
            with (out/(name+'.log')).open('w') as log:
                log.write(json.dumps(command)+'\n');log.flush()
                result=subprocess.run(command,cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
            (out/(name+'-exit')).write_text(str(result.returncode)+'\n')
            assert result.returncode==0,(name,result.returncode)
            text=(out/(name+'.log')).read_text()
            assert 'NOTRUN' not in text and 'SKIP' not in text
    (out/'stage').write_text('FINISHED\n');rc=0
finally:
    monitor.terminate();monitor.wait(timeout=10)
    rows=[json.loads(s) for s in (out/'ownership.jsonl').read_text().splitlines()]
    if not rows or any(r['foreign_jobs'] or r['background_jobs'] for r in rows):rc=1
    if any(str(src.parent)+'/' in s for s in Path('/proc/self/mountinfo').read_text().splitlines()):rc=1
    (out/'controller-exit').write_text(str(rc)+'\n')
raise SystemExit(rc)
