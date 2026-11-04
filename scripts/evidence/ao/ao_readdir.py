#!/usr/bin/env python3
"""AO: fresh private 3-member cluster, real D4 writer, exact listing oracles.

All measurements use new directory handles. Actual Raft rounds are cluster-wide
starts during the observation window (background traffic included), not an
invented one-round-per-RPC attribution. A matched background-only window is
also recorded. Failed operations are not retried or discarded.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import signal
import sys
import threading
import time
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[3]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'scripts')]
import k33_daemons as k
from d4_progress import Run


def cpu(pid):
    s=Path(f'/proc/{pid}/stat').read_text().split(') ',1)[1].split()
    return (int(s[11])+int(s[12]))/os.sysconf('SC_CLK_TCK')


def pct(xs,q):
    return sorted(xs)[min(len(xs)-1,int(len(xs)*q))] if xs else None


class Pace:
    def __init__(self,rate):self.period=1/rate;self.local=threading.local();self.stop=threading.Event()
    def is_set(self):
        deadline=getattr(self.local,'next',time.monotonic())
        if self.stop.wait(max(0,deadline-time.monotonic())):return True
        self.local.next=max(deadline+self.period,time.monotonic()+self.period)
        return self.stop.is_set()


def spawned(argv,env,out,err,timeout):
    """No forked Python child touches FUSE buffers (K10/W121)."""
    with out.open('wb') as stdout,err.open('wb') as stderr:
        pid=os.posix_spawnp(argv[0],argv,env,file_actions=[
            (os.POSIX_SPAWN_DUP2,stdout.fileno(),1),(os.POSIX_SPAWN_DUP2,stderr.fileno(),2)])
        start=time.monotonic();fd=os.pidfd_open(pid);expired=threading.Event()
        def timeout_child():
            expired.set()
            try:signal.pidfd_send_signal(fd,signal.SIGKILL)
            except ProcessLookupError:pass
        timer=threading.Timer(timeout,timeout_child);timer.daemon=True;timer.start()
        try:
            found,status,usage=os.wait4(pid,0);ended=time.monotonic()
            assert not expired.is_set(),f'listing timeout after {timeout}s: {argv}'
            return os.waitstatus_to_exitcode(status),ended-start,usage.ru_utime+usage.ru_stime
        finally:
            timer.cancel();timer.join();os.close(fd)


def foreign_work(owned):
    found=[]
    for p in Path('/proc').glob('[0-9]*'):
        try:
            cwd=os.readlink(p/'cwd')
            if '/efs-task' in cwd.lower() and not (cwd==str(owned) or cwd.startswith(str(owned)+'/')):
                found.append(dict(pid=p.name,cwd=cwd))
        except (FileNotFoundError,PermissionError,ProcessLookupError):pass
    return found



def run(a):
    a.work.mkdir(parents=True,exist_ok=False);a.output.mkdir(parents=True,exist_ok=False)
    env={key:val for key,val in os.environ.items() if not key.startswith('EFS_')}
    env.update(EFS_TRANSPORT='tcp',EFS_RAFT_OBS='1',EFS_BATCH_OBS='1',
               EFS_PACK_MIN_AGE_MS='1000',EFS_PACK_INTERVAL_MS='200',
               EFS_QUERY_REFRESH_MS='2000',EFS_FAULT_MEMTABLE_BYTES=str(256*1024),LC_ALL='C')
    assert not foreign_work(a.work.parent), ('another agent is using the host',foreign_work(a.work.parent))
    c=k.Cluster(a.work,'cluster',(1,2,3),env)
    mount=None;threads=[];load=None;pace=None;statstop=threading.Event();stats=[]
    result=dict(verdict='RUNNING',sizes=a.sizes,repeats=a.repeats,methods=a.methods,measurements=[],errors=[],
                configuration=dict(transport='tcp',quota_per_node='8G',direct_io=True,
                                   d4_writers=4,offered_ops_per_writer=5,working_set_per_writer=16,
                                   split_threshold='product default'))
    try:
        for n in (1,2,3):c.start(n,None if n==1 else 1,quota='8G',args=('--discovery-port','0'))
        c.wait_members((1,2,3),3)
        c.wait_voters((1,2,3),lambda n,g:set(g)=={0,2} and all(v==(1,7) for v in g.values()))
        mount=k.Mount(c,1,'mnt')
        target=mount.path/'ao';target.mkdir()
        # Populate before timed D4/list windows; every failed CREATE is fatal.
        population={}
        for size in a.sizes:
            directory=target/str(size);directory.mkdir();started=time.monotonic()
            poplock=threading.Lock();created=[0]
            def populate(worker):
                for i in range(worker,size,a.populate_workers):
                    fd=os.open(directory/f'f{i:07d}',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o644)
                    os.close(fd)
                    with poplock:
                        created[0]+=1
                        if created[0]%1024==0:
                            (a.output/'progress.json').write_text(json.dumps(dict(phase='populate',size=size,created=created[0],elapsed_s=time.monotonic()-started)))
            with ThreadPoolExecutor(max_workers=a.populate_workers) as pool:list(pool.map(populate,range(a.populate_workers)))
            population[size]=dict(seconds=time.monotonic()-started,inode=directory.stat().st_ino)
            (a.output/'population.json').write_text(json.dumps(population,indent=2))
        assert not foreign_work(a.work.parent),('host overlap during population',foreign_work(a.work.parent))
        load=Run(SimpleNamespace(mount=mount.path,fg_mount=mount.path,out=a.output/'d4',working_set=16),None)
        load.dir.mkdir()
        for w in range(4):load.populate(w)
        assert not load.errors,load.errors
        pace=Pace(5);load.stop_load=pace
        def writer(w):
            try:load.writer_fixed(w)
            except BaseException as e:result['errors'].append('D4 thread: '+repr(e))
        threads=[threading.Thread(target=writer,args=(w,)) for w in range(4)]
        for t in threads:t.start()
        probe=target/'probe';probe.write_bytes(b'probe')
        def statter():
            deadline=time.monotonic();tick=0
            while not statstop.is_set():
                t0=time.monotonic()
                try:os.stat(probe);stats.append((time.monotonic(),time.monotonic()-t0))
                except OSError as e:result['errors'].append('stat: '+repr(e))
                tick+=1
                if tick%10==0:
                    foreign=foreign_work(a.work.parent)
                    if foreign:
                        result['errors'].append('host overlap: '+repr(foreign));result['timing_valid']=False
                deadline+=.1;statstop.wait(max(0,deadline-time.monotonic()))
        statthread=threading.Thread(target=statter);statthread.start();threads.append(statthread)
        helper=a.work/'getdents'
        import subprocess
        subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/measure/ao_getdents.c'),'-o',str(helper)],check=True)
        def snapshot(label):
            counters={}
            for n in (1,2,3):
                offset=(c.dir/f'n{n}.log').stat().st_size
                rc,text=k.mgmt(env,'raft-status',c.ep(n));assert rc==0,(n,rc,text)
                (a.output/f'{label}-n{n}-status.txt').write_text(text)
                with (c.dir/f'n{n}.log').open() as log:log.seek(offset);tail=log.read()
                found=re.findall(r'ao-counters g0=(\d+) g2=(\d+) dir_calls=(\d+)',tail)
                assert found,('missing probes',n,tail[-1000:])
                counters[n]=list(map(int,found[-1]))
            return counters
        def cpus():return {str(n):cpu(p.pid) for n,p in c.procs.items()}|{'fuse':cpu(mount.p.pid)}
        def counts():return sum(load.counts[x] for x in ('overwritten','truncated','rewritten'))
        time.sleep(10)
        for size in a.sizes:
            directory=target/str(size);expected={f'f{i:07d}' for i in range(size)}
            for rep in range(1,a.repeats+1):
                for method in a.methods:
                    label=f'{size}-{rep}-{method}'
                    (a.output/'progress.json').write_text(json.dumps(dict(phase='listing',size=size,repeat=rep,method=method)))
                    before=snapshot(label+'-before');c0=cpus();w0=counts();t0=time.monotonic()
                    log0=mount.logp.stat().st_size
                    argv=[str(helper),str(directory)] if method=='getdents' else ['ls','-ln','--color=never','--quoting-style=literal','--',str(directory)]
                    output=a.output/(label+'.names');err=a.output/(label+'.stderr')
                    rc,wall,child_cpu=spawned(argv,env,output,err,a.list_deadline)
                    ended=time.monotonic();c1=cpus();w1=counts();after=snapshot(label+'-after')
                    with mount.logp.open() as log:log.seek(log0);trace=log.read()
                    pages=re.findall(r'ao-page parent=(\d+) count=(\d+) status=(\d+) bytes=(\d+) us=(\d+)',trace)
                    ino=population[size]['inode'];pages=[x for x in pages if int(x[0])==ino]
                    sends=len(re.findall(rf'ao-send parent={ino} ',trace))
                    names=output.read_text().splitlines()
                    if method=='ls-l':names=[line.rsplit(None,1)[-1] for line in names if line and not line.startswith('total ')]
                    missing=expected-set(names);extra=set(names)-expected;dups=len(names)-len(set(names))
                    stat_window=[v for t,v in stats if t0<=t<=ended]
                    row=dict(size=size,repeat=rep,method=method,rc=rc,wall_s=wall,listing_process_cpu_s=child_cpu,
                             cpu_s={n:c1[n]-c0[n] for n in c0},rpc_sends=sends,page_replies=len(pages),
                             page_p50_us=pct([int(x[4]) for x in pages],.5),page_p99_us=pct([int(x[4]) for x in pages],.99),
                             counters_before=before,counters_after=after,
                             cluster_read_round_starts=sum(after[n][i]-before[n][i] for n in (1,2,3) for i in (0,1)),
                             server_dir_calls=sum(after[n][2]-before[n][2] for n in (1,2,3)),
                             writes=w1-w0,write_ops_s=(w1-w0)/(ended-t0),stat_samples=len(stat_window),
                             stat_p50_s=pct(stat_window,.5),stat_p99_s=pct(stat_window,.99),
                             missing=sorted(missing)[:30],extra=sorted(extra)[:30],duplicates=dups)
                    result['measurements'].append(row)
                    (a.output/'result.json').write_text(json.dumps(result,indent=2))
                    assert rc==0 and not missing and not extra and not dups, row
                    assert pages and not any(int(x[2])!=0 for x in pages),('page errors',row)
                    assert not load.errors and not result['errors'],load.errors+result['errors']
                    # Same-duration D4-only control: global rounds are not
                    # all caused by this listing, and cannot be subtracted as
                    # an exact count of listing-owned rounds.
                    (a.output/'progress.json').write_text(json.dumps(dict(phase='background-control',size=size,repeat=rep,method=method,seconds=wall)))
                    b=snapshot(label+'-control-before');ct=time.monotonic();time.sleep(wall);e=snapshot(label+'-control-after')
                    row['background_control']=dict(seconds=time.monotonic()-ct,cluster_round_starts=sum(e[n][i]-b[n][i] for n in (1,2,3) for i in (0,1)))
        result['verdict']='PASS'
        (a.output/'progress.json').write_text(json.dumps(dict(phase='verified',cases=len(result['measurements']))))
    except BaseException as e:result.update(verdict='FAIL',error=repr(e));raise
    finally:
        if pace:pace.stop.set()
        statstop.set()
        try:
            for t in threads:t.join(90);assert not t.is_alive(),'workload did not finish'
            if load:
                result['d4_errors']=load.errors;result['d4_counts']=load.counts
                for path,size in load.live.items():
                    if path.read_bytes()!=load.content(str(path.relative_to(load.dir)),path,size):
                        result['errors'].append('D4 final-version mismatch '+str(path))
                if load.errors or load.uncertain or result['errors']:result['verdict']='FAIL'
            result['stat_p50_s']=pct([v for t,v in stats],.5);result['stat_p99_s']=pct([v for t,v in stats],.99)
            (a.output/'stat-samples.json').write_text(json.dumps(stats))
            if mount:
                import subprocess
                stop=subprocess.run([str(ROOT/'scripts/client.sh'),'stop',str(mount.path)],env=env,text=True,capture_output=True,timeout=650)
                (a.output/'mount-stop.txt').write_text(stop.stdout+stop.stderr)
                assert stop.returncode==0 and not os.path.ismount(mount.path),'unclean unmount'
                assert mount.p.wait(timeout=30)==0;mount.log.close()
            for n,p in c.procs.items():p.send_signal(signal.SIGTERM);assert p.wait(timeout=60)==0,('daemon stop',n)
        except BaseException as e:result.update(verdict='FAIL',cleanup_error=repr(e));raise
        finally:
            for log in c.dir.glob('*.log'):(a.output/log.name).write_bytes(log.read_bytes())
            (a.output/'result.json').write_text(json.dumps(result,indent=2))
    assert result['verdict']=='PASS',result
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--sizes',type=int,nargs='+',default=[64,1024,16384,100000]);p.add_argument('--repeats',type=int,default=3)
    p.add_argument('--methods',nargs='+',choices=('getdents','ls-l'),default=['getdents','ls-l'])
    p.add_argument('--populate-workers',type=int,default=16);p.add_argument('--list-deadline',type=float,default=1200)
    args=p.parse_args();assert all(x>0 for x in args.sizes) and args.repeats>0 and args.populate_workers>0
    print(json.dumps(run(args))[:2000])
