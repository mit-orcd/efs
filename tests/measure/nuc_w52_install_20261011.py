#!/usr/bin/env python3
"""Install the W52 candidate only after native and both live gates pass."""
import argparse,hashlib,json,os,signal,socket,subprocess,time,traceback
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--variant',choices=('batch','clock'),default='batch');args=parser.parse_args()
base=Path('/data1/efs/devel-nuc-20261011');src=base/(args.variant+'-src');dst=Path('/data1/efs/src')
out=base/(args.variant+'-install');out.mkdir(exist_ok=False)
def wait(fn,seconds,label):
    end=time.monotonic()+seconds
    while not fn():
        assert time.monotonic()<end,label+' timeout'
        time.sleep(.5)
def run(label,args,seconds=300):
    with (out/(label+'.log')).open('w') as log:
        rc=subprocess.run(list(map(str,args)),stdout=log,stderr=subprocess.STDOUT,timeout=seconds).returncode
    (out/(label+'-exit.txt')).write_text(str(rc)+'\n');assert rc==0,(label,rc)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def verify_images(root,images):
    for name,image in images.items():assert digest(root/name)==image['sha256'],name
env=dict(os.environ,EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp',
    EFS_CLUSTER='nuc',EFS_DIRECT_IO='1',EFS_MD_RAFT_N='4',
    EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_BATCH_OBS='0')
def start(label):
    with (out/(label+'.log')).open('w') as log:
        rc=subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],
            env=env,stdout=log,stderr=subprocess.STDOUT,timeout=360).returncode
    assert rc==0,(label,rc)
    def listeners():
        for port in range(17432,17436):
            try:
                with socket.create_connection(('127.0.0.1',port),.3):pass
            except OSError:return False
        return True
    wait(listeners,180,label+' actual listeners')
    assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
def samples():
    prior=json.loads((base/'final-service.json').read_text())['existing_payload_samples_equal_on_both_mounts']
    for sample in prior:
        for mount in ('mnt','mnt2'):
            assert digest(Path('/data1/efs',mount)/sample['path'])==sample['sha256']
    return prior
stopped=changed=False
try:
    def live_restored():
        for phase in ('build','live'):
            assert not (base/(args.variant+'-'+phase+'/failure.txt')).exists(),phase+' failed; refuse installation'
        return (base/(args.variant+'-live/restore-exit.txt')).exists()
    wait(live_restored,14400,'live gate restoration')
    for relative in (args.variant+'-build/run-exit.txt',args.variant+'-live/run-exit.txt',
        args.variant+'-live/full-direct-exit.txt',args.variant+'-live/full-buffered-exit.txt'):
        assert (base/relative).read_text().strip()=='0',relative
    images=json.loads((base/(args.variant+'-build/images.json')).read_text());verify_images(src,images)
    built=json.loads((base/(args.variant+'-build/source-sha256.json')).read_text())
    core={name:value for name,value in built.items() if name.startswith(('src/','include/')) and Path(name).suffix in ('.c','.h')}
    current={str(p.relative_to(src)):digest(p) for folder in ('src','include') for p in (src/folder).rglob('*') if p.suffix in ('.c','.h')}
    assert current==core,'candidate core changed since clean/native acceptance'
    old=json.loads((base/'deployment-20261011/installed-images.json').read_text());verify_images(dst,old)
    before=samples();assert Path('/data1/efs/.mkfs-done').is_file()
    run('source-backup',['tar','-czf',out/'old-source.tar.gz','--exclude=./results',
        '--exclude=./.git','--exclude=./_deps','-C',dst,'.'])
    pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split()
    assert len(pids)==4
    for pid in pids:assert os.path.realpath('/proc/'+pid+'/exe')==str(dst/'efsd')
    stopped=True
    for mount in ('mnt','mnt2'):
        run(mount+'-stop',[dst/'scripts/client.sh','stop','/data1/efs/'+mount],180)
        assert not os.path.ismount('/data1/efs/'+mount)
    for pid in pids:
        fd=os.pidfd_open(int(pid));signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
    wait(lambda:all(not Path('/proc',p).exists() for p in pids),180,'old daemon drain')
    assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
    changed=True
    run('install',['rsync','-a','--exclude=/results/','--exclude=/.git/',str(src)+'/',str(dst)+'/'])
    verify_images(dst,images);start('start');samples()
    (out/'installed-source-sha256.json').write_text(json.dumps({str(p.relative_to(dst)):digest(p)
        for folder in ('src','include','tests') for p in (dst/folder).rglob('*') if p.suffix in ('.c','.h','.py')},indent=2))
    processes=[]
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:
            exe=Path(os.readlink(proc/'exe'))
            if exe in (dst/'efsd',dst/'efs-fuse'):
                assert digest(proc/'exe')==images[exe.name]['sha256']
                processes.append(dict(pid=int(proc.name),kind=exe.name))
        except (FileNotFoundError,ProcessLookupError,PermissionError):continue
    assert sum(p['kind']=='efsd' for p in processes)==4
    assert sum(p['kind']=='efs-fuse' for p in processes)==2
    for port in range(17432,17436):
        with socket.create_connection(('127.0.0.1',port),3):pass
    (out/'after.json').write_text(json.dumps(dict(images=images,processes=processes,
        preserved_samples_on_both_mounts=before),indent=2))
    (out/'images.json').write_text(json.dumps(images,indent=2))
    (out/'run-exit.txt').write_text('0\n');stopped=False
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
    if stopped:
        # Never restart a partially installed image set or force-discard writes.
        assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
        assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1, 'retain live processes for investigation'
        if changed:
            run('rollback',['tar','-xzf',out/'old-source.tar.gz','-C',dst])
        verify_images(dst,old);start('rollback-start');samples()
