#!/usr/bin/env python3
"""Resume idle append acceptance only after the explicit quota-only cold retry."""
import argparse,hashlib,json,os,subprocess,time,traceback
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--after-native-doc-retry',action='store_true');options=parser.parse_args()
base=Path('/data1/efs/devel-nuc-20261011');out=base/('clock-doc-sidecar-retry' if options.after_native_doc_retry else 'clock-placement-retry');out.mkdir(exist_ok=False)
try:
    deadline=time.monotonic()+1800
    recovery=base/'data-cold-quota-retry'
    while not (recovery/'run-exit.txt').exists():
        assert time.monotonic()<deadline,'quota retry restoration wait exceeded'
        assert not (recovery/'failure.txt').exists(),'quota cold retry failed; refuse new gates'
        time.sleep(1)
    assert (recovery/'run-exit.txt').read_text().strip()=='0'
    assert all((base/'data-placement'/n).read_text().strip()=='0' for n in ('tests-exit.txt','restore-exit.txt'))
    assert (base/'data-placement/cold-quota-initial-failure.txt').exists()
    assert not (base/'data-placement/failure.txt').exists()
    assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
    images=json.loads((base/'deployment-20261011/installed-images.json').read_text())
    for n,v in images.items():assert hashlib.sha256((Path('/data1/efs/src')/n).read_bytes()).hexdigest()==v['sha256']
    if options.after_native_doc_retry:
        built=base/'clock-build'
        while not (built/'run-exit.txt').exists():
            assert not (built/'failure.txt').exists(),'native retry failed'
            assert time.monotonic()<deadline,'native retry wait exceeded'
            time.sleep(1)
        assert (built/'run-exit.txt').read_text().strip()=='0'
        assert (built/'doc-sidecar-initial-failure/native-exit.txt').read_text().strip()=='2'
        assert json.loads((built/'doc-sidecar.json').read_text())['resumed_missing_sidecar']
        time.sleep(1) # let the successful build controller exit before ownership check
    else:assert not (base/'clock-src').exists(),'build already started; do not archive progress'
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:cmd=(proc/'cmdline').read_bytes().split(b'\0')
        except (FileNotFoundError,ProcessLookupError,PermissionError):continue
        assert not any(Path(x.decode(errors='replace')).name in (
            'nuc_w52_build_20261011.py','nuc_w52_live_20261011.py','nuc_w52_install_20261011.py',
            'nuc_w52_resume_20261011.py','nuc_w52_profile_20261011.py') for x in cmd),'old acceptance controller active'
    phases=('clock-live','clock-install','w52-clock-20000','w52-clock-long-profile')
    if not options.after_native_doc_retry:phases=('clock-build',*phases)
    for name in phases:
        p=base/name
        assert p.exists() and ((p/'failure.txt').exists() or (p/'blocked-prerequisite.json').exists())
        assert not any((p/n).exists() for n in ('images.json','native-exit.txt','mnt-stop.log','append-burst-progress.json')),'phase progressed; refuse idle archive'
        if (p/'run-exit.txt').exists():
            assert (p/'blocked-prerequisite.json').exists() and (p/'run-exit.txt').read_text().strip()=='125','phase ran; refuse idle archive'
        assert not (Path('/data1/efs/mnt')/'w52-clock-20000-20261011').exists(),'long workload exists'
    suffix='-blocked-doc-sidecar' if options.after_native_doc_retry else '-blocked-placement'
    for name in phases:(base/name).rename(base/(name+suffix))
    jobs=[]
    commands=[('build',['nuc_w52_build_20261011.py','--variant','clock']),
        ('live',['nuc_w52_live_20261011.py','--variant','clock']),
        ('install',['nuc_w52_install_20261011.py','--variant','clock']),
        ('append',['nuc_w52_resume_20261011.py','--count','20000','--name','w52-clock-20000','--after','clock-install','--before-2000']),
        ('profile',['nuc_w52_profile_20261011.py','--variant','clock'])]
    if options.after_native_doc_retry:commands=commands[1:]
    for phase,args in commands:
        script=base/args[0];command=['python3',str(script),*args[1:]]
        log=(base/('clock-'+phase+('-doc-sidecar-retry-controller.log' if options.after_native_doc_retry else '-placement-retry-controller.log'))).open('w')
        job=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,start_new_session=True,close_fds=True);log.close()
        jobs.append(dict(phase=phase,pid=job.pid,command=command,script_sha256=hashlib.sha256(script.read_bytes()).hexdigest()))
    (out/'queued.json').write_text(json.dumps(dict(jobs=jobs,images_before=images,scope='Same frozen f52cd2dd overlay and native/live-before-install gates; earlier blocked outputs retained. Root-scoped server candidate excluded.'),indent=2))
    (out/'run-exit.txt').write_text('0\n')
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());raise
