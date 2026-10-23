#!/usr/bin/env python3
"""Exclusive NUC W96 ABBA measurements; preserve service stores, restore finally."""
import hashlib,json,os,re,signal,socket,subprocess,time
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261010');src=base/'src'
evidence=base/'w96';evidence.mkdir(exist_ok=False)
def run(args,**kw):return subprocess.run(list(map(str,args)),check=True,**kw)
def wait(fn,limit,label):
    end=time.monotonic()+limit
    while time.monotonic()<end:
        if fn():return
        time.sleep(.3)
    raise RuntimeError(label+' timed out')
def listening(p):
    try:
        with socket.create_connection(('127.0.0.1',p),timeout=.3):return True
    except OSError:return False
def snapshot():
    return dict(utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),diskstats=Path('/proc/diskstats').read_text(),meminfo=Path('/proc/meminfo').read_text(),processes=subprocess.check_output(['ps','-eo','pid,comm,args'],text=True))
stopped=False
try:
    procs=subprocess.check_output(['ps','-eo','pid,args'],text=True)
    assert not any(re.search(r'^\s*\d+\s+(?:python3|/usr/bin/python3) .*?(tiny_files.py|drive-load.py|balanced-load.py)',x) for x in procs.splitlines()),'generator active'
    identities={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (base/'wal-control',base/'wal-reserve')}
    (evidence/'identity.json').write_text(json.dumps(dict(binaries=identities,source='72a2a7ae',order='ABBA on SATA then NVMe, batch1 then16',seconds=90,reset_bytes=8*1024*1024),indent=2))
    pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(pids)==4
    (evidence/'baseline.json').write_text(json.dumps(snapshot()))
    stopped=True
    for mount in ('mnt','mnt2'):
        with (evidence/(mount+'-stop.log')).open('w') as out:
            run(['/data1/efs/src/scripts/client.sh','stop','/data1/efs/'+mount],stdout=out,stderr=subprocess.STDOUT)
        assert not os.path.ismount('/data1/efs/'+mount)
    for pid in pids:os.kill(int(pid),signal.SIGTERM)
    wait(lambda:all(not Path('/proc',pid).exists() for pid in pids),180,'regular service drain')
    assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1,'another daemon active'
    for drive,root in [('sata',base/'w96-data'),('nvme',Path('/home/efs/devel-nuc-w96-20261010'))]:
        root.mkdir(exist_ok=True)
        (evidence/(drive+'-placement.txt')).write_text(subprocess.check_output(['findmnt','-T',str(root),'-no','SOURCE,FSTYPE,OPTIONS'],text=True))
        for batch in (1,16):
            for number,variant in enumerate(('control','reserve','reserve','control')):
                cell=f'{drive}-batch{batch}-{number}-{variant}'
                assert __import__('shutil').disk_usage(root).free>80*2**30
                with (evidence/(cell+'.json')).open('w') as out,(evidence/(cell+'.jsonl')).open('w') as err:
                    run([base/('wal-'+variant),root/(cell+'.wal'),90,batch],stdout=out,stderr=err,timeout=180)
                row=json.loads((evidence/(cell+'.json')).read_text());assert row['replay_verified']
                assert row['max_reserved_slack']<1048576
                (evidence/(cell+'-host.json')).write_text(json.dumps(snapshot()))
                print(cell,row['records_s'],row['sync_mean_ms'],'PASS',flush=True)
                time.sleep(5)
    (evidence/'exit.txt').write_text('0\n')
finally:
    if stopped:
        env=dict(os.environ,EFS_CLUSTER='nuc',EFS_DIRECT_IO='1',EFS_TRANSPORT='tcp',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_BATCH_OBS='0')
        with (evidence/'restore.log').open('w') as out:
            run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=env,stdout=out,stderr=subprocess.STDOUT)
        wait(lambda:all(listening(p) for p in range(17432,17436)),180,'regular service listeners')
        assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
        (evidence/'restored.json').write_text(json.dumps(snapshot()))
        (evidence/'restore-exit.txt').write_text('0\n')
