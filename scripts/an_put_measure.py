#!/usr/bin/env python3
"""AN fixed-rate D4-shaped rewrite measurement; private fixtures, never a live mount.

One row per invocation, three independent rows per configuration. Four files
per writer mix 8 KiB packed candidates with 192/384/512 KiB ordinary files.
Each writer offers two overwrite/truncate/in-place operations per second.
Late work is reported, never turned into a faster offered rate. Acknowledged
versions are checked through a fresh FUSE process with exact bytes and EOF.
Raw counters bracket all application activity; total proof/round counts also
include fixed-rate stat and background reads and are not exclusive attribution.
"""
import argparse
import concurrent.futures
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
import k33_daemons as k
sys.path.insert(0,str(ROOT/'scripts'))
from an_put_probe import NAMES


def quantile(values,q):
    assert values
    return sorted(values)[max(0,math.ceil(len(values)*q)-1)]


def cpu(pid):
    fields=Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()
    return (int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')


def snapshots(c,alive,after_us):
    end=time.monotonic()+20
    while time.monotonic()<end:
        rows={}
        for i in alive:
            assert c.procs[i].poll() is None, f'daemon {i} exited'
            text=(c.dir/f'n{i}.log').read_text()
            found=re.findall(r'an-proof: ([^\n]+)',text)
            if not found:break
            row={n:int(v) for n,v in re.findall(r'(\w+)=(\d+)',found[-1])}
            assert all(n in row for n in NAMES),row
            if row['t_us']<=after_us:break
            rows[i]=row
        if len(rows)==len(alive):return rows
        time.sleep(.1)
    raise AssertionError('fresh counter snapshots did not arrive within 20 s')


def delta(before,after):
    assert before.keys()==after.keys()
    out={n:0 for n in NAMES}
    for i in before:
        assert after[i]['t_us']>before[i]['t_us']
        for name in NAMES:
            value=after[i][name]-before[i][name]
            assert value>=0,(i,name,before[i],after[i])
            out[name]+=value
    return out


def body(writer,slot,version,size):
    return hashlib.shake_256(f'an/{writer}/{slot}/{version}'.encode()).digest(size)


def run(args):
    work=args.work.resolve();out=args.out.resolve()
    assert not work.exists() and not out.exists(),'fresh fixture and evidence required'
    assert work.is_relative_to(Path('/tmp')) or work.is_relative_to(Path.home())
    assert not work.is_relative_to(Path('/data1'))
    work.mkdir(parents=True);out.mkdir(parents=True)
    env={key:val for key,val in os.environ.items() if not key.startswith('EFS_')}
    assert args.observer == '1' or args.counterless_control, 'counter snapshots require observation'
    env.update(EFS_TRANSPORT=args.transport,EFS_RAFT_OBS=args.observer,EFS_PACK_MIN_AGE_MS='1000',
               EFS_PACK_INTERVAL_MS='200',EFS_QUERY_REFRESH_MS='2000',
               EFS_FAULT_MEMTABLE_BYTES=str(256*1024))
    if args.device:env['EFS_RDMA_DEV']=args.device
    if args.diagnostic:env.update(EFS_PUT_TICKET_DBG='1',EFS_RPC_PROF='1')
    c=k.Cluster(work,'cluster',[1,2,3],env);mounts=[];stat=None;collector=None;statlog=None;tracers=[]
    doc=dict(verdict='FAIL',writers=args.writers,transport=args.transport,down=args.down,
             seconds=args.seconds,rate_per_writer=args.rate,fixture=str(work),observer=args.observer,
             counter_scope='all traffic in bracketing snapshots; includes stat/background proofs')
    if args.counterless_control:
        assert not (ROOT/'an-probe-manifest.json').exists(),'control must use uninstrumented product source'
        doc['counter_scope']='UNINSTRUMENTED DATA/TIMING CONTROL: authority counters unavailable'
    if args.diagnostic:doc['purpose']='DIAGNOSTIC with existing ticket/RPC tracing; not native timing evidence'
    sizes=(8192,192*1024,384*1024,512*1024)
    live={};operations=[];errors=[];lock=threading.Lock()
    try:
        for i in (1,2,3):c.start(i,join=None if i==1 else 1,quota='8G')
        c.wait_members((1,2,3),3)
        c.wait_voters((1,2,3),lambda n,gs:set(gs)=={0,2} and all(h and v==7 for h,v in gs.values()))
        # Exercise both local same-group owner proofs and remote owner proofs.
        # Node 3 is always the stopped follower, so placement is matched.
        k.place_leaders(c,1,groups=(0,))
        k.place_leaders(c,2,groups=(2,))
        if args.trace_store:
            assert args.diagnostic,'syscall tracing is diagnostic only'
            for i in (1,2,3):
                log=(out/f'strace-attach-n{i}.log').open('w')
                trace=subprocess.Popen(['strace','-f','-ttt','-yy','-s','0','-e','status=failed',
                    '-e','trace=openat,write,writev,fsync,fdatasync,ftruncate,close,mkdir,mkdirat,unlink,unlinkat,rename,renameat,renameat2',
                    '-o',str(out/f'store-n{i}.strace'),'-p',str(c.procs[i].pid)],
                    stdout=subprocess.DEVNULL,stderr=log)
                tracers.append((trace,log))
            time.sleep(1)
            assert all(p.poll() is None for p,_ in tracers),'strace could not attach to owned daemons'
        writer=k.Mount(c,1,'writer');mounts.append(writer)
        for w in range(args.writers):
            (writer.path/f'w{w}').mkdir()
            for slot,size in enumerate(sizes):
                rel=f'w{w}/f{slot}'
                with open(writer.path/rel,'wb') as f:assert f.write(body(w,slot,0,size))==size
                live[rel]=(w,slot,0,size)
        if args.down:
            c.stop(3,signal.SIGKILL)
            end=time.monotonic()+30
            while time.monotonic()<end:
                tabs=[c.members(i) for i in (1,2)]
                if all(3 in t and t[3][1]=='down' for t in tabs):break
                time.sleep(.2)
            else:raise AssertionError('private follower not reported DOWN within 30 s')
        alive=(1,2) if args.down else (1,2,3)
        statlog=(out/'stat.log').open('w')
        stat=subprocess.Popen([str(ROOT/'an-stat-client'),c.ep(1),str(args.seconds)],env=env,
                              stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=statlog,text=True)
        # The real membership API emits a startup diagnostic on stdout.
        # Preserve every line; READY is a bounded protocol marker, not an
        # assumption that the library emits no other diagnostic.
        ready=threading.Event();statrows=[]
        def collect_stat():
            for line in stat.stdout:
                statlog.write(line);statlog.flush()
                if line=='READY\n':ready.set()
                else:statrows.append(line)
        collector=threading.Thread(target=collect_stat);collector.start()
        assert ready.wait(30),'stat RPC helper did not become ready within 30 s'
        if args.counterless_control:
            time.sleep(5);before={}
        else:before=snapshots(c,alive,time.monotonic_ns()//1000)
        cpus={i:cpu(c.procs[i].pid) for i in alive}
        fuse_cpu=cpu(writer.p.pid)
        start=time.monotonic();stop=start+args.seconds
        doc.update(before=before,load_started_mono_us=int(start*1000000))
        stat.stdin.write('\n');stat.stdin.flush()
        def load(w):
            seq=0;next_at=start
            while next_at<stop:
                time.sleep(max(0,next_at-time.monotonic()))
                if time.monotonic()>=stop:break
                slot=seq%4;rel=f'w{w}/f{slot}';_,_,v,old=live[rel]
                v+=1;mode=seq%3
                size=old if mode==2 else sizes[slot]-(v%2)*min(4096,sizes[slot]//2)
                payload=body(w,slot,v,size);began=time.monotonic()
                try:
                    with open(writer.path/rel,'wb' if mode==0 else 'r+b') as f:
                        if mode==1:os.ftruncate(f.fileno(),old//2)
                        assert f.write(payload)==size,'short write'
                        if mode==1:f.truncate()
                    elapsed=time.monotonic()-began
                    if elapsed>=30:raise AssertionError(f'write/close exceeded unchanged 30 s bound: {elapsed}')
                    live[rel]=(w,slot,v,size)
                    with lock:operations.append(dict(w=w,seq=seq,mode=mode,bytes=size,
                        start_s=began-start,seconds=elapsed,offered_late_s=max(0,began-next_at)))
                except BaseException as e:
                    with lock:errors.append(dict(w=w,seq=seq,path=rel,mode=mode,version=v,
                        bytes=size,seconds=time.monotonic()-began,error=repr(e)))
                    raise
                seq+=1;next_at=start+seq/args.rate
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.writers) as pool:
            list(pool.map(load,range(args.writers)))
        stat_rc=stat.wait(timeout=40);collector.join(timeout=5);statlog.flush()
        assert not collector.is_alive() and stat_rc==0,('stat helper failed',stat_rc)
        elapsed=time.monotonic()-start
        cpu_delta={i:cpu(c.procs[i].pid)-cpus[i] for i in alive}
        fuse_delta=cpu(writer.p.pid)-fuse_cpu
        if args.counterless_control:
            time.sleep(5);after={};counts=None
        else:
            after=snapshots(c,alive,time.monotonic_ns()//1000)
            counts=delta(before,after);assert counts['fragment_ok']>0,counts
        stats=[{key:int(value) for key,value in re.findall(r'(\w+)=(-?\d+)',x)}
               for x in statrows if x.startswith('an-stat t_us=')]
        assert stats and all(x['rc']==0 for x in stats),stats
        footer=[{key:int(value) for key,value in re.findall(r'(\w+)=(\d+)',x)}
                for x in statrows if x.startswith('an-stat-total ')]
        assert len(footer)==1 and footer[0]['n']==len(stats),footer
        assert len(operations)==args.writers*math.ceil(args.seconds*args.rate), 'offered operations were missed'
        assert len(stats)==math.ceil(args.seconds*10) and footer[0]['missed']==0, 'stat slots were missed'
        (out/'operations.json').write_text(json.dumps(operations)+'\n')
        doc.update(counters=counts,before=before,after=after,cpu_seconds=cpu_delta,
                   fuse_cpu_seconds=fuse_delta,actual_s=elapsed,operations=len(operations),
                   offered_operations=args.writers*math.ceil(args.seconds*args.rate),
                   throughput_Bps=sum(x['bytes'] for x in operations)/elapsed,
                   write_p95_s=quantile([x['seconds'] for x in operations],.95),
                   write_p99_s=quantile([x['seconds'] for x in operations],.99),
                   stat_rpc_p50_us=quantile([x['latency_us'] for x in stats],.5),
                   stat_rpc_p99_us=quantile([x['latency_us'] for x in stats],.99),
                   stat_rpc_count=len(stats),stat_missed_slots=footer[0]['missed'],
                   per_fragment={n:counts[n]/counts['fragment_ok'] for n in NAMES} if counts else None,
                   batching_wait_s=0,errors=errors)
        fresh=k.Mount(c,2,'verify');mounts.append(fresh)
        for rel,(w,slot,v,size) in live.items():
            got=(fresh.path/rel).read_bytes()
            assert got==body(w,slot,v,size),('fresh read mismatch',rel,v,size,len(got))
        if args.down:
            # The member stays hard-down through all timing/counter samples
            # and independent payload verification. K54 is an existing base
            # failure of revocation with storage absent, not an AN speedup.
            # Restore only this fresh fixture's member for strict teardown;
            # do not waive either client's flush/SETTLE/revoke exit checks.
            c.restart(3,quota='8G')
            c.wait_members((1,2,3),3)
            c.wait_voters((1,2,3),lambda n,gs:set(gs)=={0,2} and all(h and v==7 for h,v in gs.values()))
            doc['member_restored_after_degraded_verification']=True
        doc['verified_files']=len(live);doc['verdict']='PASS'
        return 0
    except BaseException as e:
        doc.update(error=repr(e),errors=errors)
        raise
    finally:
        if stat and stat.poll() is None:stat.terminate();stat.wait(timeout=30)
        if collector:collector.join(timeout=5)
        if statlog:statlog.close()
        cleanup=[]
        for m in reversed(mounts):
            try:
                m.close()
                if m.p.returncode:cleanup.append(f'{m.path}: FUSE exited {m.p.returncode}')
            except BaseException as e:cleanup.append(repr(e))
        try:c.stop_all()
        except BaseException as e:cleanup.append(repr(e))
        for trace,log in tracers:
            if trace.poll() is None:trace.send_signal(signal.SIGINT)
            trace.wait(timeout=10);log.close()
        if cleanup:doc.update(verdict='FAIL',cleanup_errors=cleanup)
        for i in (1,2,3):
            p=c.dir/f'n{i}.log'
            if p.exists():
                text=p.read_text()
                (out/f'raft-observation-n{i}.log').write_text('\n'.join(
                    x for x in text.splitlines() if 'raft-obs' in x or 'an-proof:' in x or 'an-pump:' in x)+'\n')
        (out/'result.json').write_text(json.dumps(doc,indent=2)+'\n')
        (out/'operations.json').write_text(json.dumps(operations)+'\n')
        (out/'acknowledged-versions.json').write_text(json.dumps(live,indent=2)+'\n')
        print(json.dumps(doc),flush=True)
        if cleanup:raise RuntimeError(f'private fixture cleanup failed: {cleanup}')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--writers',type=int,choices=(1,8,32),required=True)
    p.add_argument('--transport',choices=('tcp','rdma'),required=True)
    p.add_argument('--device');p.add_argument('--down',action='store_true')
    p.add_argument('--counterless-control',action='store_true',help='untouched-product data/timing control only; never a primary AN counter measurement')
    p.add_argument('--observer',choices=('0','1'),default='1',help='existing EFS_RAFT_OBS setting; off only for counterless calibration')
    p.add_argument('--diagnostic',action='store_true',help='existing ticket/RPC diagnostics; exclude from native timing comparisons')
    p.add_argument('--trace-store',action='store_true',help='diagnostic failed-syscall capture on only the three owned daemons')
    p.add_argument('--seconds',type=float,default=60);p.add_argument('--rate',type=float,default=2)
    a=p.parse_args();assert 0<a.seconds<=3600 and 0<a.rate<=100
    raise SystemExit(run(a))
