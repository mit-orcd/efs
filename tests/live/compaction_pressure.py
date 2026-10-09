#!/usr/bin/env python3
"""Private AMD direct/TCP production-cap pressure gate. Retains all evidence.
Requires a fault build. Never touches installed stores, services or mounts.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

source=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('work',type=Path)
p.add_argument('--probe',type=Path,required=True)
p.add_argument('--mount',type=Path,help='borrowed empty unmounted approved FUSE path')
a=p.parse_args();work=a.work.resolve();work.mkdir(parents=True,exist_ok=False)
env=dict(os.environ,EFS_MD_RAFT_N='3',EFS_TRANSPORT='tcp',EFS_ADMISSION_PRESSURE='1',
         EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_RAFT_OBS='1',EFS_FAULT_PRESSURE_SEED='1')
for base in range(25400,25600,3):
    sockets=[]
    try:
        for port in range(base,base+3):
            s=socket.socket();sockets.append(s);s.bind(('127.0.0.1',port))
        break
    except OSError:continue
    finally:
        for s in sockets:s.close()
else:raise RuntimeError('no free fixture ports')
procs=[];logs=[];fuse=None;generation=0;park=work/'park';seq=0
raw=(work/'samples.jsonl').open('w')
acks=(work/'acks.log').open('w')
manifest={'transport':'tcp','mode':'direct','uid':os.getuid(),'base':base,'cap_bytes':1<<30}
(work/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
def mgmt(n,*args):
    return subprocess.run([str(source/'efs-mgmt'),*map(str,args),f'127.0.0.1:{base+n}'],env=env,text=True,
                          stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
def status(n):
    r=mgmt(n,'raft-status');assert r.returncode==0,r.stdout
    rows=re.findall(r'group (\d+) hosted=1 role=(\w+) leader=(-?\d+) term=(\d+) commit=(\d+) applied=(\d+)',r.stdout)
    assert len(rows)==2,r.stdout
    return [dict(zip(('group','role','leader','term','commit','applied'),
                     [int(x) if x.lstrip('-').isdigit() else x for x in row])) for row in rows]
def sample(phase):
    records=[];tick=time.time()
    for n,proc in enumerate(procs):
        assert proc.poll() is None,'owned daemon exited'
        text=(work/f'n{n+1}.log').read_text()
        def latest(prefix):
            rows=[line for line in text.splitlines() if prefix in line]
            return {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',rows[-1])} if rows else None
        rss=re.search(r'VmRSS:\s+(\d+)',Path(f'/proc/{proc.pid}/status').read_text())
        disk={}
        for kind in ('kv','log'):
            disk[kind+'_physical_bytes']=sum(f.stat().st_size for f in (work/f'n{n+1}/mdraft'/kind).rglob('*') if f.is_file())
        records.append(dict(disk=disk,node=n+1,phase=phase,ts=tick,rss_kb=int(rss[1]),
                            groups=status(n),kv=latest('kv-obs:'),pressure=latest('pressure-obs:'),
                            timing=latest('raft-obs: wait_timeouts=')))
    commits={g:max(r['groups'][g]['commit'] for r in records) for g in range(2)}
    for r in records:
        r['replication_lag']=max(commits[g]-r['groups'][g]['applied'] for g in range(2))
        raw.write(json.dumps(r)+'\n')
        assert r['disk']['log_physical_bytes']<4<<30,'physical log exceeded explicit 4 GiB fixture gate'
        assert r['rss_kb']<2*1024*1024,'replica exceeded explicit 2 GiB RSS gate'
        q=r['pressure']
        if q:
            assert q['inbox_hi']<=32<<20 and q['proposal_bytes']<=8<<20
            assert q['outbox_hi']<=3*(16<<20)
            assert q['pending_n']<=2048 and q['pending_work']<=256<<20
    raw.flush();return records

def wait(fn,seconds=60):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if fn():return
        time.sleep(.2)
    raise AssertionError('fixture timeout')
def start(n):
    f=(work/f'n{n+1}.log').open('a');logs.append(f)
    e=dict(env)
    if n==2:e.update(EFS_FAULT_COMPACT_STALL='1',EFS_FAULT_COMPACT_FILE=str(park))
    cmd=[str(source/'efsd'),'--node-id',str(n+1),'--addr','127.0.0.1','--port',str(base+n),
         '--storage',str(work/f'n{n+1}'),'--quota','4G','--writers','0','--direct-io']
    if n:cmd+=['--join',f'127.0.0.1:{base}']
    procs.append(subprocess.Popen(cmd,env=e,stdout=f,stderr=f))
    wait(lambda:'listening' in (work/f'n{n+1}.log').read_text())
def mount_client(node):
    global fuse
    assert a.mount and a.mount.is_dir() and not os.path.ismount(a.mount) and not any(a.mount.iterdir())
    f=(work/f'fuse-{len(logs)}.log').open('w');logs.append(f)
    fuse=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{base+node}','pressure-oracle',str(a.mount)],env=env,stdout=f,stderr=f)
    wait(lambda:os.path.ismount(a.mount))
    wait(lambda:subprocess.run(['timeout','3','stat',str(a.mount)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0)
def stop_client():
    global fuse
    if fuse:
        subprocess.run([str(source/'scripts/client.sh'),'stop',str(a.mount)],env=env,check=True,timeout=100)
        assert fuse.wait(15)==0;fuse=None
def oracle_write():
    mount_client(0)
    for i in range(32):
        value=bytes([i+1])*(256<<10)
        started=time.monotonic()
        with (a.mount/f'pressure-{i}').open('wb') as out:
            out.write(value);out.flush();os.fsync(out.fileno())
        with (work/'fuse-acks.jsonl').open('a') as record:
            record.write(json.dumps({'file':i,'bytes':len(value),'fsync_us':int((time.monotonic()-started)*1e6)})+'\n')
    for i in range(8):
        path=a.mount/f'cleanup-{i}'
        with path.open('wb') as out:
            out.write(bytes([i+1])*4096);out.flush();os.fsync(out.fileno())
        path.unlink()
    stop_client()
def oracle_read(node):
    mount_client(node)
    for i in range(32):assert (a.mount/f'pressure-{i}').read_bytes()==bytes([i+1])*(256<<10)
    stop_client()
def write(count,phase):
    global seq
    leaders=[n for n in range(3) if status(n)[0]['role']=='LEADER'];assert len(leaders)==1,leaders
    proc=subprocess.Popen([str(a.probe),'write',str(base+leaders[0]),str(seq),str(count)],env=env,stdout=acks,stderr=acks)
    end=time.monotonic()+180
    while proc.poll() is None:
        sample(phase)
        if time.monotonic()>end:proc.terminate();proc.wait();raise AssertionError('writer timeout')
        time.sleep(1)
    assert proc.returncode==0,proc.returncode
    seq+=count
try:
    assert os.getuid()!=0,'EFS fixture must run unprivileged'
    start(0);start(1)
    wait(lambda:any(mgmt(n,'raft-mkfs').returncode==0 for n in range(2)))
    start(2);time.sleep(6)
    assert all(g['role']=='FOLLOWER' for g in status(2)),status(2)
    park.touch()
    cap=False
    for batch in range(24):
        write(64,'fill')
        records=sample('fill')
        kv=records[2]['kv']
        if kv and kv['l0_bytes']>=1<<30:cap=True;break
    assert cap,'did not reach unchanged production cap'
    for batch in range(32):
        write(32,'held-cap')
        if batch==0 and a.mount:oracle_write()
    records=sample('held-cap')
    assert records[2]['pressure']['recv_rejected']>0,'follower did not reject new log entries'
    assert all(g['role']=='FOLLOWER' for g in records[2]['groups'])
    park.unlink()
    def recovered():
        records=sample('recover')
        return all(r['replication_lag']==0 and r['kv'] and r['kv']['l0_bytes']<768<<20 for r in records)
    wait(recovered,180)
    assert 'raft-snap: import diff group=0 rc=0' in (work/'n3.log').read_text(),'InstallSnapshot import not observed'
    if a.mount:oracle_read(2)
    # Deliberate crash belongs to this fixture only, after every oracle fsync.
    for proc in procs:proc.kill()
    for proc in procs:assert proc.wait(30)==-9
    procs.clear()
    for n in range(3):
        subprocess.run([str(a.probe),'verify',str(work/f'n{n+1}/mdraft/kv'),'0',str(seq)],env=env,check=True,stdout=acks,stderr=acks,timeout=60)
    if a.mount:
        for n in range(3):start(n)
        time.sleep(5);oracle_read(1)
        for proc in procs:proc.terminate()
        for proc in procs:assert proc.wait(30)==0
        procs.clear()
    # Emit the strict twelve-field format only for complete observations.
    with (work/'samples.tsv').open('w') as out:
        writer=csv.writer(out,delimiter='\t',lineterminator='\n')
        writer.writerow(['ts','node','mt_bytes','l0_bytes','n_l0','n_l1','rss_kb','lag_max',
                         'pump_hold_max_us','apply_sleep_n','backpressure_n','role'])
        for line in (work/'samples.jsonl').read_text().splitlines():
            r=json.loads(line);kv=r['kv'];q=r['pressure']
            if kv is None or q is None:continue
            writer.writerow([r['ts'],r['node'],kv['mt_bytes'],kv['l0_bytes'],kv['l0'],kv['l1'],
                             r['rss_kb'],r['replication_lag'],
                             (r['timing'] or {}).get('pump_hold_max','NA'),'NA',q['recv_rejected'],
                             ','.join(g['role'] for g in r['groups'])])
    subprocess.run(['python3',str(source/'tests/measure/w23_samples.py'),'cap-proof',str(work/'samples.tsv')],check=True)
    (work/'PASS').write_text(f'{seq} acknowledged 1 MiB writes; real 1 GiB follower L0 cap; healthy quorum; InstallSnapshot recovery; three cold exact-value oracles; FUSE oracle={bool(a.mount)}\n')
    print((work/'PASS').read_text(),flush=True)
finally:
    park.unlink(missing_ok=True)
    stop_client()
    for proc in procs:
        if proc.poll() is None:proc.terminate()
    for proc in procs:proc.wait(30)
    for f in logs:f.close()
    raw.close();acks.close()
