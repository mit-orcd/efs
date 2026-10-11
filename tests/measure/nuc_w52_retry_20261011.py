#!/usr/bin/env python3
"""Resume only the demonstrated append-peer fixture failure, after clean restoration."""
import hashlib,json,subprocess,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011')
out=base/'batch-fixture-retry';out.mkdir(exist_ok=False)
def wait(predicate,seconds,label):
    end=time.monotonic()+seconds
    while not predicate():
        assert time.monotonic()<end,label+' timeout'
        time.sleep(1)
def launch(script,log,*args):
    with (base/log).open('w') as stream:
        proc=subprocess.Popen(['python3',str(base/script),*map(str,args)],stdout=stream,
                              stderr=subprocess.STDOUT,start_new_session=True)
    (out/(script+'.pid')).write_text(str(proc.pid)+'\n')
    return proc.pid
try:
    live=base/'batch-live'
    wait(lambda:(live/'restore-exit.txt').exists(),7200,'first gate restoration')
    wait(lambda:all(not Path('/proc',str(p)).exists() for p in (11750,11900,29257)),30,'first controllers exited')
    assert (live/'restore-exit.txt').read_text().strip()=='0'
    assert (live/'failure.txt').exists() and not (live/'run-exit.txt').exists()
    assert (live/'full-direct-exit.txt').read_text().strip()=='1'
    text=(live/'full-direct.log').read_text()
    assert 'FileNotFoundError' in text and "'/data1/efs/mnt2/append-burst'" in text
    assert (live/'full-buffered-exit.txt').read_text().strip()=='0','do not auto-retry an engine/other buffered failure'
    assert (base/'batch-install/failure.txt').exists()
    assert not (base/'batch-install/source-backup-exit.txt').exists(),'installation unexpectedly advanced'
    assert not list((base/'w52-fixed-20000').iterdir()),'long workload unexpectedly advanced'
    expected=json.loads((base/'deployment-20261011/installed-images.json').read_text())
    for name,image in expected.items():
        assert hashlib.sha256((Path('/data1/efs/src')/name).read_bytes()).hexdigest()==image['sha256']
    for sample in json.loads((base/'final-service.json').read_text())['existing_payload_samples_equal_on_both_mounts']:
        for mount in ('mnt','mnt2'):
            assert hashlib.sha256((Path('/data1/efs',mount)/sample['path']).read_bytes()).hexdigest()==sample['sha256']
    for directory in ('batch-live','batch-install','w52-long-profile'):
        (base/directory).rename(base/(directory+'-initial-failed'))
    for name in ('batch-live-controller.log','batch-install-controller.log','w52-profile-controller.log'):
        (base/name).rename(base/(name+'.initial-failed'))
    live_pid=launch('nuc_w52_live_retry_20261011.py','batch-live-controller.log',
                    '--resume-buffered',base/'batch-live-initial-failed')
    install_pid=launch('nuc_w52_install_20261011.py','batch-install-controller.log')
    profile_pid=launch('nuc_w52_profile_20261011.py','w52-profile-controller.log')
    (out/'queued.json').write_text(json.dumps(dict(live=live_pid,install=install_pid,profile=profile_pid,
        limits='Original failure retained; buffered acceptance inherited only after exact fixture/image checks. Long append controller remains waiting, never interrupted.'),indent=2))
    (out/'run-exit.txt').write_text('0\n')
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());raise
