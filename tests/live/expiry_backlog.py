"""Large finite expiry queues in the owned TCP/direct/files fixture."""
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import statistics
import struct
import subprocess
import threading
import time
from expiry_lifecycle import exact


def exercise(mount,port,work,wait,physical_files,restart,count):
    cli=Path(__file__).resolve().parents[2]/'efs-mgmt'
    results={'files_per_phase':count,'payload_bytes':1024,'workers':8,'phases':[],'verdict':'RUNNING'}
    def save(): (work/'expiry-backlog.json').write_text(json.dumps(results,indent=2))
    def rpc(tag,ino,extra=b''):
        payload=bytes((0 if ino&1 else 2,tag))+struct.pack('>QQ',ino,1)+extra
        until=time.monotonic()+30
        while True:
            for node in range(4):
                with socket.create_connection(('127.0.0.1',port+node),timeout=20) as s:
                    s.sendall(struct.pack('>IB',len(payload)+1,95)+payload)
                    n=struct.unpack('>I',exact(s,4))[0];reply=exact(s,n)
                assert reply[0]==96 and len(reply)==33
                rc,hint,rev,value,other=struct.unpack('=iiQQQ',reply[1:])
                if not rc:return rev,value,other
                assert rc in (-13,-15),(tag,ino,rc)
            assert time.monotonic()<until,(tag,ino,'timeout')
            time.sleep(.1)
    def status():
        return [subprocess.check_output([str(cli),'gc-status',f'127.0.0.1:{port+i}'],text=True,timeout=30) for i in range(4)]
    def latency_probe(stop,values,errors):
        path=mount/'expiry-backlog-probe'
        try:
            while not stop.is_set():
                begin=time.monotonic()
                with path.open('wb',buffering=0) as f:f.write(b'foreground'*100);os.fsync(f.fileno())
                assert path.read_bytes()==b'foreground'*100
                path.unlink();values.append((time.monotonic()-begin)*1000)
                stop.wait(.1)
        except BaseException as e:errors.append(repr(e))
    def measure(values):
        ordered=sorted(values)
        return {'samples':len(values),'median_ms':statistics.median(values) if values else None,
                'p95_ms':ordered[min(len(ordered)-1,int(len(ordered)*.95))] if values else None,
                'max_ms':max(values) if values else None}
    try:
        for phase in ('staggered','restart','disable-reenable'):
            root=mount/('expiry-backlog-'+phase);root.mkdir();ino=root.stat().st_ino;rev,_,_=rpc(60,ino)
            rpc(58,ino,struct.pack('>IQQQB',0,rev,3600_000_000_000,7200_000_000_000,3))
            payload=b'x'*1024
            def create(i):
                path=root/f'f-{i:06d}'
                with path.open('xb',buffering=0) as f:f.write(payload);os.fsync(f.fileno());id=os.fstat(f.fileno()).st_ino
                rev,deadline,_=rpc(61,id)
                return id,rev
            start=time.monotonic()
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:records=list(pool.map(create,range(count)))
            row={'phase':phase,'created':count,'create_seconds':time.monotonic()-start}
            baseline=[];errors=[];stop=threading.Event();probe=threading.Thread(target=latency_probe,args=(stop,baseline,errors));probe.start()
            time.sleep(10);stop.set();probe.join(60);assert not probe.is_alive() and not errors,errors
            row['foreground_baseline']=measure(baseline)
            # Enough lead time to schedule the entire queue before any deadline.
            due=time.time_ns()+180_000_000_000
            def schedule(item):
                i,(id,rev)=item;deadline=due+(i%4)*10_000_000_000 if phase=='staggered' else due
                rpc(59,id,struct.pack('>IQQQ',os.getuid(),rev,0,deadline))
            start=time.monotonic()
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:list(pool.map(schedule,enumerate(records)))
            row['schedule_seconds']=time.monotonic()-start
            assert time.time_ns()<due-30_000_000_000,'queue preparation exceeded deadline lead time'
            # Protected renewal and reused-name controls must survive every mode change.
            survivor=root/'renewed'
            with survivor.open('xb',buffering=0) as f:f.write(b'renewed');os.fsync(f.fileno())
            id=survivor.stat().st_ino;rev,_,_=rpc(61,id)
            rpc(59,id,struct.pack('>IQQQ',os.getuid(),rev,0,time.time_ns()+1800_000_000_000))
            if phase=='restart':restart(True)
            if phase=='disable-reenable':
                restart(False)
                wait(lambda:time.time_ns()>due+5_000_000_000,'deadlines passed while globally disabled',240)
                assert len(list(root.iterdir()))==count+1,'disabled expiry removed files'
                assert all('expiry_enabled=0' in s for s in status())
                row['disabled_overdue_preserved']=count
                restart(True)
            else:wait(lambda:time.time_ns()>=due,'first queue deadline',240)
            started=time.monotonic();values=[];errors=[];stop=threading.Event()
            probe=threading.Thread(target=latency_probe,args=(stop,values,errors));probe.start()
            observations=[]
            try:
                while True:
                    remaining=sum(p.name.startswith('f-') for p in root.iterdir())
                    usage=os.statvfs(work)
                    rss=[]
                    for proc in Path('/proc').glob('[0-9]*'):
                        try:
                            if str(cli.parent/'efsd').encode() not in (proc/'cmdline').read_bytes():continue
                            for line in (proc/'status').read_text().splitlines():
                                if line.startswith('VmRSS:'):rss.append(int(line.split()[1]))
                        except OSError:pass
                    observation={'elapsed_seconds':time.monotonic()-started,'remaining':remaining,
                                 'free_bytes':usage.f_bavail*usage.f_frsize,'rss_kib':rss}
                    observations.append(observation)
                    assert observation['free_bytes']>2*1024**3 and (not rss or max(rss)<512*1024),observation
                    if not remaining:break
                    assert time.monotonic()-started<600,('expiry backlog not draining',remaining)
                    time.sleep(2)
            finally:stop.set();probe.join(60)
            assert not probe.is_alive() and not errors,errors
            row['drain_seconds']=time.monotonic()-started;row['foreground_drain']=measure(values)
            row['last_deadline_to_observed_empty_seconds']=max(0,(time.time_ns()-(due+(30_000_000_000 if phase=='staggered' else 0)))/1e9)
            row['observations']=observations;row['statuses']=status()
            assert survivor.read_bytes()==b'renewed','renewed survivor lost'
            # Verify actual data fragments, not only removed directory entries.
            wait(lambda:all(not physical_files(id) for id,_ in records),'large expiry physical drain',300)
            row['physical_empty_seconds']=time.monotonic()-started
            restart(True)
            assert sorted(p.name for p in root.iterdir())==['renewed'],'cold namespace mismatch'
            assert survivor.read_bytes()==b'renewed';survivor.unlink();root.rmdir()
            row['verdict']='PASS';results['phases'].append(row);save()
            print(f'expiry backlog {phase} count={count} create={row["create_seconds"]:.1f}s drain={row["drain_seconds"]:.1f}s PASS',flush=True)
        results['verdict']='PASS';save()
    except BaseException:
        results['verdict']='FAIL';save();raise
