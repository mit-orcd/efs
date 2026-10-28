#!/usr/bin/env python3
"""K48: a real DIR move after READDIR's row cut must not omit a name.

Owned TCP loopback fixture; failure retains stores/logs. No application retry
or timeout relaxation. All 96 payloads are fsynced before enumeration.
"""
import concurrent.futures, hashlib, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, FUSE, mgmt

def lane(name):
    h=2166136261
    for b in name.encode():h=((h^b)*16777619)&0xffffffff
    return h%64

work=Path(tempfile.mkdtemp(prefix='efs-k48-',dir=os.environ.get('TMPDIR')))
hold=work/'hold';hold.touch()
pause=work/'pause'
env=dict(os.environ,EFS_TRANSPORT='tcp',EFS_DIR_SPREAD_MIN='64')
c=Cluster(work,'k48',[1,2,3],env);mount=c.dir/'mnt-a';mount.mkdir()
fp=None;log=None;passed=False;cleanup_error=None
try:
    assert EFSD.exists() and FUSE.exists() and os.access('/dev/fuse',os.R_OK|os.W_OK),'FUSE/fault build required'
    for i in (1,2,3):
        extra=dict(EFS_FAULT_DIR_SERVE_FILE=str(hold),EFS_FAULT_READDIR_LAYOUT_FILE=str(pause))
        if i==3:extra['EFS_FAULT_DIR_CORRUPT_FILE']=str(work/'damage')
        c.start(i,join=1 if i>1 else None,extra_env=extra)
    c.wait_voters((1,2,3),lambda i,v:v=={0:(1,7),2:(1,7)})
    log=(c.dir/'mnt-a.log').open('w')
    fp=subprocess.Popen([str(FUSE),c.ep(1),str(mount),'-f'],stdout=log,stderr=subprocess.STDOUT,env=env)
    end=time.monotonic()+30
    while not os.path.ismount(mount) and time.monotonic()<end:
        assert fp.poll() is None;time.sleep(.05)
    assert os.path.ismount(mount)
    for attempt in range(32):
        target=mount/f'target-{attempt}';target.mkdir();ino=target.stat().st_ino
        if ino & 1:break
    else:raise AssertionError('no group-0 directory allocated')
    group=0
    leader=c.leader_placement(1)[group]
    if leader==3:
        rc,out=mgmt(env,'raft-step-down',c.ep(3),str(group));assert rc==0,out
        end=time.monotonic()+15
        while time.monotonic()<end:
            leader=c.leader_placement(1)[group]
            if leader in (1,2):break
            time.sleep(.05)
    assert leader in (1,2),'target needs a healthy-quorum leader before damage'
    first=next(f'a{i:05d}' for i in range(1000) if lane(f'a{i:05d}')==1)
    rest=[f'z{i:05d}' for i in range(20000) if lane(f'z{i:05d}')==63][:95]
    expected=[first]+rest;assert len(expected)==96
    def put(name):
        with (target/name).open('wb',buffering=0) as f:
            f.write(hashlib.sha256(name.encode()).digest());os.fsync(f.fileno())
    for name in expected[:64]:put(name)
    (work/'damage').touch()
    def command(kind):
        owner=c.leader_placement(1)[group]
        rc,out=mgmt(env,'raft-dir',c.ep(owner),str(ino),kind)
        print('DIR',kind,rc,out.strip(),flush=True);assert rc==0 and 'rc=0 ' in out,out
    command('begin');command('begin')
    c.wait_log(3,'INCOMPATIBLE.*apply halted, group not served',timeout=30)
    for name in expected[64:]:put(name)
    pause.write_text(str(ino)+'\n')
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        listing=pool.submit(lambda:sorted(p.name for p in target.iterdir()))
        end=time.monotonic()+5
        while not Path(str(pause)+'.ready').exists() and time.monotonic()<end:time.sleep(.01)
        assert Path(str(pause)+'.ready').exists(),'READDIR pause did not fire'
        command('migrate')
        Path(str(pause)+'.release').touch()
        names=listing.result(timeout=30)
    assert any(f'released inode={ino} ready=1' in c.log(i) for i in (1,2)), 'pause expired before migration completed'
    missing=sorted(set(expected)-set(names));extra=sorted(set(names)-set(expected))
    print('K48',dict(inode=ino,count=len(names),missing=missing,extra=extra,first=first),flush=True)
    assert names==sorted(expected),(len(names),missing,extra)
    for name in expected:assert (target/name).read_bytes()==hashlib.sha256(name.encode()).digest(),name
    for _ in range(8):assert sorted(p.name for p in target.iterdir())==sorted(expected)
    passed=True
    print('K48 PASS: halt, consensus migration at row cut, 96 exact names/payloads and repeated listings',flush=True)
finally:
    Path(str(pause)+'.release').touch()
    if fp:
        stop=subprocess.run([str(FUSE.parent/'scripts/client.sh'),'stop',str(mount)],capture_output=True,text=True,env=env,timeout=90)
        print('client stop',stop.returncode,stop.stdout,stop.stderr,flush=True)
        if stop.returncode:cleanup_error=stop.stdout+stop.stderr;passed=False
        if stop.returncode and fp.poll() is None:
            subprocess.run(['fusermount3','-u',str(mount)],capture_output=True);fp.terminate()
        try:fp.wait(timeout=30)
        except subprocess.TimeoutExpired:fp.kill();fp.wait()
    if log:log.close()
    c.stop_all()
    print('K48 fixture',work,'passed',passed,flush=True)
    if passed and not os.environ.get('EFS_K33_KEEP'):shutil.rmtree(work)

    if cleanup_error:raise AssertionError('clean client stop failed: '+cleanup_error)
