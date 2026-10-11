#!/usr/bin/env python3
"""Preserve existing NUC stores; install the exact gate-accepted branch binaries."""
import collections,hashlib,json,os,shutil,signal,socket,subprocess,time
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261010');src=base/'src';installed=Path('/data1/efs/src');out=base/'installed-validation'
out.mkdir(exist_ok=False);names=('efsd','efs-fuse','efs-mgmt','efs-query','efs-bench')
def wait(fn,seconds,label):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if fn():return
        time.sleep(.5)
    raise RuntimeError(label+' timed out')
def listening(port):
    try:
        with socket.create_connection(('127.0.0.1',port),.3):return True
    except OSError:return False
def run(args,path,timeout=180):
    with path.open('w') as log:
        rc=subprocess.run(list(map(str,args)),stdout=log,stderr=subprocess.STDOUT,timeout=timeout).returncode
    path.with_suffix(path.suffix+'.exit').write_text(str(rc)+'\n');assert rc==0
def image(path):
    return dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(),version=subprocess.check_output([str(path),'--version'],text=True))
def samples():
    mount=Path('/data1/efs/mnt');queue=collections.deque([mount]);seen=0;files=[];root=[]
    while queue and seen<256 and len(files)<32:
        folder=queue.popleft();seen+=1
        with os.scandir(folder) as entries:
            for index,entry in enumerate(entries):
                if index==64:break
                if entry.name=='.efs':continue
                if folder==mount:root.append(entry.name)
                if entry.is_dir(follow_symlinks=False):queue.append(Path(entry.path))
                elif entry.is_file(follow_symlinks=False):
                    st=entry.stat(follow_symlinks=False)
                    if 0<st.st_size<=2**20:
                        files.append(dict(path=str(Path(entry.path).relative_to(mount)),size=st.st_size,sha256=hashlib.sha256(Path(entry.path).read_bytes()).hexdigest()))
                        if len(files)==32:break
    assert files,'no preserved nonempty payload samples found'
    return dict(root_first64_names=root,visited_directories=seen,samples=files,scope='Bounded sample, not full namespace inventory or full-payload acceptance')
stopped=False;changed=False
env=dict(os.environ,EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp',EFS_CLUSTER='nuc',EFS_DIRECT_IO='1',EFS_MD_RAFT_N='4',EFS_RAFT_FLOW='1',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_GROUP_DRAIN='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_BATCH_OBS='0')
def start(label):
    with (out/(label+'.log')).open('w') as log:
        rc=subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300).returncode
    assert rc==0
    wait(lambda:all(listening(p) for p in range(17432,17436)),180,'all installed listeners')
    assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
try:
    wait(lambda:(base/'allocation-profile/restore-exit.txt').exists(),6500,'separate profiling restoration')
    assert (base/'allocation-profile/run-exit.txt').read_text().strip()=='0'
    for stage in ('native','full-direct','full-buffered','w96-wal-failure'):
        assert (base/'closure'/(stage+'-exit.txt')).read_text().strip()=='0'
    expected=json.loads((base/'closure/binary-provenance.json').read_text())
    assert all(image(src/n)==expected[n] for n in names),'validated binaries changed'
    old={n:image(installed/n) for n in names};(out/'before-binaries.json').write_text(json.dumps(old,indent=2))
    preserved=samples();(out/'before-samples.json').write_text(json.dumps(preserved,indent=2))
    backup=out/'old-binaries';backup.mkdir()
    for n in names:shutil.copy2(installed/n,backup/n)
    run(['tar','-czf',out/'before-source.tar.gz','--exclude=./results','--exclude=./.git','--exclude=./_deps','-C',installed,'.'],out/'source-backup.log',300)
    pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(pids)==4
    for pid in pids:
        assert os.path.realpath(Path('/proc',pid,'exe'))==str(installed/'efsd')
    stopped=True
    for m in ('mnt','mnt2'):
        run([installed/'scripts/client.sh','stop','/data1/efs/'+m],out/(m+'-stop.log'))
        assert not os.path.ismount('/data1/efs/'+m)
    for pid in pids:
        proc=Path('/proc',pid)
        assert os.path.realpath(proc/'exe')==str(installed/'efsd')
        fd=os.pidfd_open(int(pid));signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
    wait(lambda:all(not Path('/proc',p).exists() for p in pids),180,'old service drain')
    assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
    run(['rsync','-a','--exclude=/results/','--exclude=/.git/',str(src)+'/',str(installed)+'/'],out/'install.log',300);changed=True
    assert all(image(installed/n)==expected[n] for n in names)
    start('start');stopped=False
    after=[]
    for record in preserved['samples']:
        path=Path('/data1/efs/mnt')/record['path'];peer=Path('/data1/efs/mnt2')/record['path']
        for target in (path,peer):
            assert target.stat().st_size==record['size']
            assert hashlib.sha256(target.read_bytes()).hexdigest()==record['sha256']
        after.append(record)
    live=[]
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():continue
        try:
            cmd=process.joinpath('cmdline').read_bytes().split(b'\0')
            if cmd and cmd[0].decode() in (str(installed/'efsd'),str(installed/'efs-fuse')):
                kind=Path(cmd[0].decode()).name;exe=process/'exe'
                assert hashlib.sha256(exe.read_bytes()).hexdigest()==expected[kind]['sha256']
                live.append(dict(pid=int(process.name),kind=kind,cmdline=[v.decode() for v in cmd if v]))
        except (FileNotFoundError,ProcessLookupError):continue
    assert sum(r['kind']=='efsd' for r in live)==4 and sum(r['kind']=='efs-fuse' for r in live)==2
    (out/'after.json').write_text(json.dumps(dict(binaries=expected,live=live,exact_sample_matches_on_both_mounts=after),indent=2))
    (out/'run-exit.txt').write_text('0\n')
except BaseException:
    import traceback
    (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
    if stopped:
        # A partial installation cannot silently restart a mixed engine set.
        if not changed:
            for n in names:
                if (out/'old-binaries'/n).exists():shutil.copy2(out/'old-binaries'/n,installed/n)
        start('restore-after-failure')
