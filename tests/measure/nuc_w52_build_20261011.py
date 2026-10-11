#!/usr/bin/env python3
"""Build the frozen W52 change after matched controls and baseline finish."""
import argparse,hashlib,json,subprocess,time,traceback
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--variant',choices=('batch','clock'),default='batch');parser.add_argument('--resume-doc-sidecar',action='store_true',help='Retry the exact missing-results documentation fixture failure without changing the already clean-built core.');args=parser.parse_args()
assert not args.resume_doc_sidecar or args.variant=='clock'
resume_docs=args.resume_doc_sidecar
base=Path('/data1/efs/devel-nuc-20261011');src=base/(args.variant+'-src');out=base/(args.variant+'-build');out.mkdir(exist_ok=args.variant=='batch' or resume_docs)
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
        if resume_docs:
            assert src.exists()
            external=(src/'results').is_symlink()
            assert external or not (src/'results').exists()
            assert all((out/(phase+'-exit.txt')).read_text().strip()=='0' for phase in ('clean','all'))
            assert (out/'native-exit.txt').read_text().strip()=='2' and (out/'failure.txt').exists()
            log=(out/'native.log').read_text()
            assert 'architecture doc gate: 1/5 checks failed' in log
            assert ('link escapes the repo: ../../results/' if external else 'link target does not exist: ../../results/') in log
            if external:assert (src/'results').resolve()==(base/'batch-src/results').resolve()
            assert 'ok    regen-diff:' in log and 'ok    invariant-refs:' in log and 'ok    single-home:' in log
            assert log.rstrip().endswith('make: *** [Makefile:379: test] Error 1')
        else:assert not src.exists(),'fresh clock source required'
        manifest=json.loads((base/'clock-overlay.json').read_text());archive=base/'clock-overlay.tar'
        assert manifest['commit'].startswith('f52cd2dd') and hashlib.sha256(archive.read_bytes()).hexdigest()==manifest['sha256']
        if not resume_docs:
            subprocess.run(['rsync','-a','--exclude=/results/','--exclude=/.git/',str(base/'batch-src')+'/',str(src)+'/'],check=True)
            subprocess.run(['tar','-xf',archive,'-C',src],check=True)
        expected=json.loads((base/'clock-core-expected.json').read_text())
        current={str(p.relative_to(src)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ('src','include') for p in (src/folder).rglob('*') if p.suffix in ('.c','.h')}
        current['Makefile']=hashlib.sha256((src/'Makefile').read_bytes()).hexdigest()
        assert current==expected,'clock candidate differs from committed core/Makefile'
        (out/'overlay.json').write_text(json.dumps(manifest,indent=2))
        evidence=base/'batch-src/results';assert evidence.is_dir()
        if (src/'results').is_symlink():
            assert (src/'results').resolve()==evidence.resolve()
            (src/'results').unlink()
        (src/'results').mkdir()
        # The link checker deliberately rejects paths resolving outside this
        # checkout. A real directory with hard-linked immutable evidence keeps
        # that protection and avoids duplicating artifact payloads.
        subprocess.run(['rsync','-a','--link-dest='+str(evidence),str(evidence)+'/',str(src/'results')+'/'],check=True)
        if resume_docs:
            retained=out/('doc-sidecar-external-link-failure' if external else 'doc-sidecar-initial-failure');retained.mkdir()
            for name in ('native.log','native-exit.txt','failure.txt'):(out/name).rename(retained/name)
            if (out/'doc-sidecar.json').exists():(out/'doc-sidecar.json').rename(retained/'doc-sidecar.json')
        (out/'doc-sidecar.json').write_text(json.dumps(dict(path=str(src/'results'),source=str(evidence),kind='self-contained hard-linked evidence',core_unchanged=True,resumed_missing_sidecar=resume_docs),indent=2))
        with (out/'doc-sidecar-check.log').open('w') as log:
            rc=subprocess.run(['python3','docs/check-architecture.py'],cwd=src,stdout=log,stderr=subprocess.STDOUT,timeout=120).returncode
        assert rc==0,'self-contained documentation fixture still fails'
    identity='f52cd2dd-nuc' if args.variant=='clock' else '4afc063c-nuc'
    versions=['EFS_GIT_ID='+identity,'EFS_GIT_DESCRIBE='+identity,'EFS_GIT_BRANCH=devel-nuc']
    phases=[('clean',['make','clean']),('all',['make','-j4','all',*versions]),('native',['make','-j4','test',*versions])]
    for name,args in (phases[2:] if resume_docs else phases):
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
