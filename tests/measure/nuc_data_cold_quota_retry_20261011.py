#!/usr/bin/env python3
"""Retry only the retained third-cell cold gate after an explicit quota refusal."""
import gzip,hashlib,json,os,re,signal,socket,subprocess,time,traceback
from pathlib import Path

session=Path('/data1/efs/devel-nuc-20261011');base=session/'data-placement'
cell=base/'2-sata';out=session/'data-cold-quota-retry';out.mkdir(exist_ok=False)
src=Path('/data1/efs/src');mount=base/'mnt';port=26770
identity=json.loads((base/'identity.json').read_text())
env=dict(os.environ,**identity['flags']);servers=[];handles=[];stopped=False
def wait(fn,seconds,label):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if fn():return
        time.sleep(.5)
    raise RuntimeError(label+' timed out')
def run(args,**kwargs):return subprocess.run(list(map(str,args)),check=True,**kwargs)
def listening(p):
    try:
        with socket.create_connection(('127.0.0.1',p),.3):return True
    except OSError:return False
def same_images():
    expected=json.loads((session/'deployment-20261011/installed-images.json').read_text())
    for name,value in expected.items():
        assert hashlib.sha256((src/name).read_bytes()).hexdigest()==value['sha256']
def old_samples():
    for row in json.loads((session/'final-service.json').read_text())['existing_payload_samples_equal_on_both_mounts']:
        for m in ('mnt','mnt2'):
            assert hashlib.sha256((Path('/data1/efs',m)/row['path']).read_bytes()).hexdigest()==row['sha256']
def stop_private():
    if os.path.ismount(mount):run([src/'scripts/client.sh','stop',mount],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    for p in servers:
        if p.poll() is None:p.terminate()
    for p in servers:p.wait(timeout=180)
    servers.clear()
    for h in handles:h.close()
    handles.clear()
def states():
    rows=[]
    for n in range(1,4):
        raw=subprocess.check_output([str(src/'efs-mgmt'),'raft-status',f'127.0.0.1:{port+n-1}'],text=True,timeout=15)
        for m in re.finditer(r'group (\d+) hosted=(\d+) role=(\w+) leader=(-?\d+) term=(\d+) commit=(\d+) applied=(\d+) voters=(0x[0-9a-f]+)',raw):
            g,hosted,role,leader,term,commit,applied,voters=m.groups()
            rows.append(dict(node=n,group=int(g),hosted=int(hosted),role=role,leader=int(leader),term=int(term),commit=int(commit),applied=int(applied),voters=int(voters,16)))
    assert len(rows)==6 and all(r['hosted']==1 and r['voters']==7 for r in rows)
    return rows
try:
    wait(lambda:(base/'restore-exit.txt').exists() and not Path('/proc/51392').exists(),900,'original normal restoration')
    assert (base/'restore-exit.txt').read_text().strip()=='0'
    assert 'RuntimeError: server exited' in (base/'failure.txt').read_text()
    assert 'Storage quota 80.00 GiB exceeds safe maximum' in (cell/'n1-cold.log').read_text()
    assert not (cell/'complete.json').exists() and not (base/'tests-exit.txt').exists()
    for tag in ('0-sata','1-nvme'):
        accepted=json.loads((base/tag/'complete.json').read_text());assert accepted['errors']==[] and accepted['cold_checks']==64
    final=json.loads((cell/'load-final.json').read_text());assert final['errors']==[]
    cuts=json.loads((cell/'catch-up.json').read_text())['cuts']
    assert all((cell/'late-profile'/n).read_text().strip()==v for n,v in [('perf-exit.txt','0'),('strace-exit.txt','124')])
    same_images();old_samples()
    assert all(listening(p) for p in range(17432,17436)) and all(not listening(p) for p in range(port,port+3))
    pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(pids)==4
    (out/'plan.json').write_text(json.dumps(dict(identity=identity,quota_before='80G',quota_after='60G',timed_workload_changed=False,original_failure=(base/'failure.txt').read_text(),regular_pids=pids),indent=2))
    stopped=True
    for m in ('mnt','mnt2'):
        run([src/'scripts/client.sh','stop',Path('/data1/efs',m)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        assert not os.path.ismount('/data1/efs/'+m)
    for pid in pids:
        assert os.path.realpath('/proc/'+pid+'/exe')==str(src/'efsd')
        fd=os.pidfd_open(int(pid));signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
    wait(lambda:all(not Path('/proc',p).exists() for p in pids),180,'normal daemon drain')
    commands=[]
    for n in range(1,4):
        root=Path(f'/data1/efs/w97-data-placement-data-20261011-2/n{n}')
        metadata=Path(f'/data1/efs/w97-data-placement-meta-20261011-2/n{n}')
        assert root.is_dir() and metadata.is_dir()
        command=[src/'efsd','--node-id',n,'--addr','127.0.0.1','--port',port+n-1,'--storage',root,'--quota','60G','--direct-io','--meta-storage',metadata]
        if n>1:command+=['--join',f'127.0.0.1:{port}']
        commands.append(list(map(str,command)))
        log=(cell/f'n{n}-cold-quota-retry.log').open('w');handles.append(log)
        p=subprocess.Popen(commands[-1],env=env,stdout=log,stderr=log);servers.append(p)
        wait(lambda:listening(port+n-1) if p.poll() is None else (_ for _ in ()).throw(RuntimeError('cold retry daemon exited')),120,'cold retry listener')
    (out/'commands.json').write_text(json.dumps(commands,indent=2))
    wait(lambda:all(r['applied']>=cuts[str(r['group'])] and r['commit']>=cuts[str(r['group'])] for r in states()),120,'cold retry captured cuts')
    log=(cell/'fuse-cold-quota-retry.log').open('w');handles.append(log)
    run([src/'scripts/client.sh',f'127.0.0.1:{port}',mount],env=env,stdout=log,stderr=log)
    locations=json.loads((cell/'placement.json').read_text())['groups'];checked=[]
    for w in range(8):
        group='0' if w<4 else '2';n=final['counts'][w];assert n>=8
        for i in list(range(4))+list(range(n-4,n)):
            path=Path(locations[group][(w%4)*512+i//1000]['path'])/f'f{w}-{i}'
            assert path.is_relative_to(mount)
            size=1+((w*65537+i)*1103515245+12345)%512
            expected=(hashlib.sha256(f'{w}:{i}'.encode()).digest()*16)[:size]
            assert path.read_bytes()==expected;checked.append(str(path))
    (cell/'cold-verification.json').write_text(json.dumps(dict(exact_files=64,paths=checked,quota_retry=str(out)),indent=2))
    stop_private()
    for n in range(1,4):
        p=cell/f'n{n}.log';rows=[]
        for line in p.read_text(errors='replace').splitlines():
            if 'batch-obs {' in line:rows.append(json.loads(line.split('batch-obs ',1)[1]))
        (cell/f'n{n}-observations.jsonl').write_text('\n'.join(json.dumps(r) for r in rows)+'\n')
        with gzip.open(cell/f'n{n}.log.gz','wb') as f:f.write(p.read_bytes())
        p.unlink() # only diagnostic logs, never stores
    (cell/'complete.json').write_text(json.dumps(dict(tag='2-sata',files=sum(final['counts']),errors=[],cold_checks=64,storage='/data1/efs/w97-data-placement-data-20261011-2',start_mono=json.loads((cell/'load-start.json').read_text())['mono_us']/1e6,seconds=600,cold_quota_retry='60G'),indent=2))
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
    stop_private()
    if stopped:
        normal=dict(env,EFS_MD_RAFT_N='4',EFS_BATCH_OBS='0',EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
        with (out/'restore.log').open('w') as log:
            run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=normal,stdout=log,stderr=subprocess.STDOUT,timeout=300)
        wait(lambda:all(listening(p) for p in range(17432,17436)),180,'restored regular listeners')
        assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
        same_images();old_samples();(out/'restore-exit.txt').write_text('0\n')
if not (out/'failure.txt').exists():
    (base/'failure.txt').rename(base/'cold-quota-initial-failure.txt')
    (base/'tests-exit.txt').write_text('0\n')
    (base/'restore-exit.txt').write_text('0\n')
    (out/'run-exit.txt').write_text('0\n')
