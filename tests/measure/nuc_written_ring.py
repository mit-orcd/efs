#!/usr/bin/env python3
"""NUC controls with fully written extents: NVMe/SATA/SATA/NVMe, no tracing."""
import argparse,os,signal,socket,subprocess,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261010')
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--profile',type=Path,default=base/'allocation-profile')
parser.add_argument('--out',type=Path,default=base/'written-ring')
parser.add_argument('--seconds',type=int,default=480)
args=parser.parse_args();out=args.out;out.mkdir(exist_ok=False)
assert 60<=args.seconds<=480
def wait(fn,seconds,label):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if fn():return
        time.sleep(.5)
    raise RuntimeError(label+' timeout')
def listening(port):
    try:
        with socket.create_connection(('127.0.0.1',port),.3):return True
    except OSError:return False
def run(name,args,timeout):
    print('START',name,time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),flush=True)
    with (out/(name+'.log')).open('w') as log:
        rc=subprocess.run(list(map(str,args)),stdout=log,stderr=subprocess.STDOUT,timeout=timeout).returncode
    (out/(name+'-exit.txt')).write_text(str(rc)+'\n');print('END',name,rc,flush=True);assert rc==0
stopped=False
try:
    wait(lambda:(args.profile/'restore-exit.txt').exists(),6500,'profile restoration')
    assert (args.profile/'run-exit.txt').read_text().strip()=='0'
    pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(pids)==4
    for p in pids:assert os.path.realpath(Path('/proc',p,'exe'))=='/data1/efs/src/efsd'
    stopped=True
    for mount in ('mnt','mnt2'):
        run(mount+'-stop',['/data1/efs/src/scripts/client.sh','stop','/data1/efs/'+mount],180)
        assert not os.path.ismount('/data1/efs/'+mount)
    for p in pids:
        assert os.path.realpath(Path('/proc',p,'exe'))=='/data1/efs/src/efsd'
        fd=os.pidfd_open(int(p));signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
    wait(lambda:all(not Path('/proc',p).exists() for p in pids),180,'service drain')
    for i,drive in enumerate(('nvme','sata','sata','nvme')):
        assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
        root=Path('/home/efs/devel-nuc-w96-20261010') if drive=='nvme' else base/'w96-data'
        cell=f'{out.name}-{i}-{drive}'
        run(cell,['python3',base/'nuc_durable_written.py',root/cell,out/cell,'--mode','written-ring','--seconds',str(args.seconds)],650)
    (out/'run-exit.txt').write_text('0\n')
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
    if stopped:
        assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
        env=dict(os.environ,EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp',EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
        with (out/'restore.log').open('w') as log:
            rc=subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300).returncode
        assert rc==0
        wait(lambda:all(listening(p) for p in range(17432,17436)),180,'restored listeners')
        assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
        (out/'restore-exit.txt').write_text('0\n')
