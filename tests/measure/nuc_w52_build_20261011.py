#!/usr/bin/env python3
"""Build the frozen W52 change after matched controls and baseline finish."""
import hashlib,json,subprocess,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011');src=base/'batch-src';out=base/'batch-build'
end=time.monotonic()+7200
try:
    while not (base/'w52-baseline-2000/run-exit.txt').exists():
        assert time.monotonic()<end,'baseline wait timeout'
        time.sleep(1)
    assert (base/'w52-baseline-2000/run-exit.txt').read_text().strip()=='0'
    versions=['EFS_GIT_ID=4afc063c-nuc','EFS_GIT_DESCRIBE=4afc063c-nuc','EFS_GIT_BRANCH=devel-nuc']
    for name,args in (('clean',['make','clean']),('all',['make','-j4','all',*versions]),('native',['make','-j4','test',*versions])):
        with (out/(name+'.log')).open('w') as log:
            rc=subprocess.run(args,cwd=src,stdout=log,stderr=subprocess.STDOUT,timeout=1800).returncode
        (out/(name+'-exit.txt')).write_text(str(rc)+'\n');assert rc==0,(name,rc)
    images={n:dict(sha256=hashlib.sha256((src/n).read_bytes()).hexdigest(),
        version=subprocess.check_output([str(src/n),'--version'],text=True))
        for n in ('efsd','efs-fuse','efs-mgmt','efs-query','efs-bench')}
    (out/'images.json').write_text(json.dumps(images,indent=2))
    (out/'source-sha256.json').write_text(json.dumps({str(p.relative_to(src)):hashlib.sha256(p.read_bytes()).hexdigest()
        for parent in ('src','include','tests') for p in (src/parent).rglob('*')
        if p.suffix in ('.c','.h','.py')},indent=2))
    (out/'run-exit.txt').write_text('0\n')
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());raise
