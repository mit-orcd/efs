#!/usr/bin/env python3
"""NUC-only continuation: named build, operator gates, W96 recovery and W97 controls."""
import hashlib,json,os,signal,socket,subprocess,time
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261010');src=base/'src';out=base/'closure';out.mkdir(exist_ok=True)
assert not (out/'build.log').exists(),'completed/active stages must not be overwritten'
env=dict(os.environ,EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_BATCH_OBS='0')
def wait(fn,limit,label):
    until=time.monotonic()+limit
    while time.monotonic()<until:
        if fn():return
        time.sleep(.3)
    raise RuntimeError(label+' timed out')
def listening(port):
    try:
        with socket.create_connection(('127.0.0.1',port),.3):return True
    except OSError:return False
def run(name,args,timeout=1800,required=True,**kw):
    print('START',name,time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),flush=True)
    with (out/(name+'.log')).open('w') as log:
        p=subprocess.run(list(map(str,args)),cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout,**kw)
    (out/(name+'-exit.txt')).write_text(str(p.returncode)+'\n')
    print('END',name,p.returncode,flush=True)
    if required:assert p.returncode==0,name+' failed; retained raw logs'
    return p.returncode
stopped=False
try:
    wait(lambda:(base/'w96/restore-exit.txt').exists(),2400,'W96 drive comparison restoration')
    assert (base/'w96/exit.txt').read_text().strip()=='0'
    defs=['EFS_GIT_ID=3df1931032b8-nuc','EFS_GIT_DESCRIBE=3df19310-nuc','EFS_GIT_BRANCH=devel-nuc']
    run('build',['make','-j6','all',*defs],timeout=900)
    (out/'binary-provenance.json').write_text(json.dumps({n:dict(sha256=hashlib.sha256((src/n).read_bytes()).hexdigest(),version=subprocess.check_output([str(src/n),'--version'],text=True)) for n in ('efsd','efs-fuse','efs-mgmt','efs-bench')},indent=2))
    run('native',['make','-j6','test',*defs],timeout=1800)
    pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(pids)==4
    stopped=True
    for mount in ('mnt','mnt2'):
        run(mount+'-stop',['/data1/efs/src/scripts/client.sh','stop','/data1/efs/'+mount],timeout=180)
        assert not os.path.ismount('/data1/efs/'+mount)
    for pid in pids:os.kill(int(pid),signal.SIGTERM)
    wait(lambda:all(not Path('/proc',p).exists() for p in pids),180,'regular service drain')
    run('w78-processes',['python3',src/'tests/test_server_processes.py'],timeout=120)
    run('w70-w80-integrity',['python3',src/'tests/live/fragment_integrity.py'],timeout=300)
    for mode in ('direct','buffered'):
        run('w79-'+mode,['python3',src/'tests/live/raft_read_freshness.py','--mode',mode],timeout=240)
    for mode in ('direct','buffered'):
        run('full-'+mode,['python3',src/'tests/live/gc_reclamation.py','--mode',mode,'--transport','tcp','--port','26590','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2','--root','/data1/efs','--root','/home/efs','--posix','--sync-writes','--sync-write-fault','--append-sync-boundary','--append-replay-race','--stop-dirty-recovery','--shutdown-mutations','--wal-tail-recovery'],timeout=1800,required=False)
    run('w96-wal-failure',['python3',src/'tests/live/gc_reclamation.py','--mode','direct','--port','26590','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2','--root','/data1/efs','--root','/home/efs','--kv-wal-persistence-only'],timeout=600,required=False)
    for drive,root in [('nvme',Path('/home/efs/devel-nuc-w96-20261010')),('sata',base/'w96-data')]:
        for index,mode in enumerate(('grow','ring','ring','grow')):
            assert not os.path.ismount('/data1/efs/mnt')
            assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1,'retained daemon would contaminate raw control'
            cell=f'w97-{drive}-{index}-{mode}'
            run(cell,['python3',src/'tests/measure/nuc_durable_io.py',root/cell,out/cell,'--mode',mode,'--seconds','480'],timeout=650)
    (out/'run-exit.txt').write_text('0\n')
except BaseException:
    import traceback
    (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
    if stopped:
        assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2')),'private mount still active; refuse wrong-service restore'
        normal=dict(env,EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
        with (out/'restore.log').open('w') as log:
            result=subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=normal,stdout=log,stderr=subprocess.STDOUT,timeout=300)
        assert result.returncode==0,'service restore failed'
        wait(lambda:all(listening(p) for p in range(17432,17436)),180,'restored service listeners')
        assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
        (out/'restore-exit.txt').write_text('0\n')
