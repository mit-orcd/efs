"""Bounded two-mount expiry churn. Invoked only by the owned live fixture."""
import concurrent.futures
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import time
from expiry_lifecycle import exact


def exercise(mount,peer_mount,port,work,wait,physical_files,restart,seconds):
    source=Path(__file__).resolve().parents[2]
    cli=source/'efs-mgmt'
    checks={'requested_seconds':seconds,'workers':4,'payload_bytes':16384,'rounds':0,
            'created':0,'renewed':0,'replacements':0,'expected_expired':0,'restarts':0,
            'verdict':'RUNNING'}
    def save():
        (work/'expiry-soak.json').write_text(json.dumps(checks,indent=2))
    def rpc(tag,ino,extra=b''):
        payload=bytes((0 if ino&1 else 2,tag))+struct.pack('>QQ',ino,1)+extra
        until=time.monotonic()+20
        while True:
            for node in range(4):
                with socket.create_connection(('127.0.0.1',port+node),timeout=15) as s:
                    s.sendall(struct.pack('>IB',len(payload)+1,95)+payload)
                    n=struct.unpack('>I',exact(s,4))[0];reply=exact(s,n)
                assert reply[0]==96 and len(reply)==33
                rc,hint,rev,value,other=struct.unpack('=iiQQQ',reply[1:])
                if not rc:return rev,value,other
                assert rc in (-13,-15),('soak RPC',tag,ino,rc)
            assert time.monotonic()<until,('soak RPC deadline',tag,ino)
            time.sleep(.1)
    def policy(path,enabled):
        ino=path.stat().st_ino;rev,_,_=rpc(60,ino)
        rpc(58,ino,struct.pack('>IQQQB',0,rev,10_000_000_000,120_000_000_000,3 if enabled else 0))
    def record(path):return rpc(61,path.stat().st_ino)
    base=mount/'expiry-soak';base.mkdir();policy(base,True)
    keep=base/'keepers';keep.mkdir();policy(keep,False)
    started=time.monotonic();next_restart=started+300
    def snapshot():
        rows=[]
        for node in range(4):
            text=subprocess.check_output([str(cli),'gc-status',f'127.0.0.1:{port+node}'],text=True,timeout=25)
            assert 'expiry_available=1' in text and 'expiry_enabled=1' in text
            assert 'expiry_clock_healthy=1' in text and 'expiry_clock_uncertain=0' in text
            rows.append({'node':node+1,'status':text})
        usage=shutil.disk_usage(work);fs=os.statvfs(work)
        rss=[]
        for proc in Path('/proc').glob('[0-9]*'):
            try:
                cmd=(proc/'cmdline').read_bytes()
                if bytes(str(source/'efsd'),'utf8') not in cmd:continue
                text=(proc/'status').read_text();m=re.search(r'^VmRSS:\s+(\d+)',text,re.M)
                if m:rss.append(int(m.group(1)))
            except (OSError,PermissionError):pass
        observation={'elapsed_seconds':round(time.monotonic()-started,3),'nodes':rows,
                     'free_bytes':usage.free,'free_inodes':fs.f_favail,'daemon_rss_kib':rss}
        assert usage.free>2*1024**3 and fs.f_favail>10000,observation
        assert not rss or max(rss)<512*1024,observation
        with (work/'expiry-soak-observations.jsonl').open('a') as f:f.write(json.dumps(observation)+'\n')
    def worker(args):
        epoch,index=args;root=mount if index%2==0 else peer_mount
        directory=root/'expiry-soak';keepers=directory/'keepers'
        token=f'{epoch}-{index}';payload=(token.encode()+b'!')*16384;payload=payload[:16384]
        def create(path):
            with path.open('xb',buffering=0) as f:
                f.write(payload);os.fsync(f.fileno());ino=os.fstat(f.fileno()).st_ino
            return ino
        expire=directory/f'expire-{token}';ei=create(expire)
        renewed=directory/f'renew-{token}';ri=create(renewed);rev,old,_=record(renewed)
        deadline=time.time_ns()+90_000_000_000
        rpc(59,ri,struct.pack('>IQQQ',os.getuid(),rev,0,deadline))
        original=directory/f'reuse-{token}';oi=create(original)
        moved=directory/f'moved-{token}';original.rename(moved)
        replacement=keepers/f'replacement-{token}';ki=create(replacement);replacement.rename(original)
        renamed=directory/f'rename-{token}';ni=create(renamed)
        destination=directory/f'renamed-{token}';renamed.rename(destination)
        assert record(original)[1]==0,'replacement must retain no deadline'
        return {'expires':[(mount/'expiry-soak'/p.name,ino) for p,ino in ((expire,ei),(moved,oi),(destination,ni))],
                'renewed':mount/'expiry-soak'/renewed.name,'renewed_ino':ri,
                'replacement':mount/'expiry-soak'/original.name,'replacement_ino':ki,
                'payload':payload,'old_deadline':old,'new_deadline':deadline}
    try:
        save();snapshot()
        while time.monotonic()-started<seconds:
            epoch=checks['rounds']
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                batch=list(pool.map(worker,[(epoch,i) for i in range(4)]))
            checks['created']+=20;checks['renewed']+=4;checks['replacements']+=4;checks['expected_expired']+=12
            due=max(x['old_deadline'] for x in batch)
            # Restart only with no pending writes and all descriptors closed.
            if time.monotonic()>=next_restart:
                restart();checks['restarts']+=1;next_restart=time.monotonic()+300
            wait(lambda:time.time_ns()>due+2_000_000_000,'soak beyond original deadlines',35)
            wait(lambda:all(not p.exists() for row in batch for p,_ in row['expires']),'soak expiration namespace drain',60)
            wait(lambda:all(not physical_files(ino) for row in batch for _,ino in row['expires']),'soak expiry physical drain',90)
            for row in batch:
                assert time.time_ns()<row['new_deadline'],'soak missed renewal verification window'
                assert row['renewed'].read_bytes()==row['payload'],'renewed data lost/corrupt'
                assert row['replacement'].read_bytes()==row['payload'],'replacement lost/corrupt'
                row['renewed'].unlink();row['replacement'].unlink()
            wait(lambda:all(not physical_files(row['renewed_ino']) and not physical_files(row['replacement_ino']) for row in batch),'soak survivor cleanup',90)
            checks['rounds']+=1;checks['elapsed_seconds']=round(time.monotonic()-started,3)
            save();snapshot()
            print(f"expiry soak round={checks['rounds']} created={checks['created']} expired={checks['expected_expired']} restarts={checks['restarts']} elapsed={checks['elapsed_seconds']}s PASS",flush=True)
        # Always prove one final cold restart, including a short developer soak.
        restart();checks['restarts']+=1
        assert sorted(p.name for p in base.iterdir())==['keepers'] and not any(keep.iterdir())
        snapshot();keep.rmdir();base.rmdir();checks['verdict']='PASS';save()
        print('expiry two-mount soak PASS',flush=True)
    except BaseException:
        checks['verdict']='FAIL';save();raise
