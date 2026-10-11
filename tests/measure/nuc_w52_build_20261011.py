#!/usr/bin/env python3
"""Build the frozen W52 change after matched controls and baseline finish."""
import argparse,hashlib,json,subprocess,time,traceback
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--variant',choices=('batch','clock'),default='batch');args=parser.parse_args()
base=Path('/data1/efs/devel-nuc-20261011');src=base/(args.variant+'-src');out=base/(args.variant+'-build');out.mkdir(exist_ok=args.variant=='batch')
end=time.monotonic()+7200
try:
    prerequisite=base/('data-placement/restore-exit.txt' if args.variant=='clock' else 'w52-baseline-2000/run-exit.txt')
    while not prerequisite.exists():
        assert time.monotonic()<end,'baseline wait timeout'
        if args.variant=='clock':assert not (base/'data-placement/failure.txt').exists(),'data matrix failed; preserve service for investigation'
        time.sleep(1)
    assert prerequisite.read_text().strip()=='0'
    if args.variant=='clock':
        assert (base/'data-placement/tests-exit.txt').read_text().strip()=='0'
        assert not src.exists(),'fresh clock source required'
        manifest=json.loads((base/'clock-overlay.json').read_text());archive=base/'clock-overlay.tar'
        assert manifest['commit'].startswith('f52cd2dd') and hashlib.sha256(archive.read_bytes()).hexdigest()==manifest['sha256']
        subprocess.run(['rsync','-a','--exclude=/results/','--exclude=/.git/',str(base/'batch-src')+'/',str(src)+'/'],check=True)
        subprocess.run(['tar','-xf',archive,'-C',src],check=True)
        expected=json.loads((base/'clock-core-expected.json').read_text())
        current={str(p.relative_to(src)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ('src','include') for p in (src/folder).rglob('*') if p.suffix in ('.c','.h')}
        current['Makefile']=hashlib.sha256((src/'Makefile').read_bytes()).hexdigest()
        assert current==expected,'clock candidate differs from committed core/Makefile'
        (out/'overlay.json').write_text(json.dumps(manifest,indent=2))
    identity='f52cd2dd-nuc' if args.variant=='clock' else '4afc063c-nuc'
    versions=['EFS_GIT_ID='+identity,'EFS_GIT_DESCRIBE='+identity,'EFS_GIT_BRANCH=devel-nuc']
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
