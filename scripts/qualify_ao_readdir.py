#!/usr/bin/env python3
"""AO private current-format gate: namespace churn, leadership, halt, forwarding.

No existing cluster or store is used. All application errors fail the run.
--posix additionally runs the unchanged core suite at its default deadlines.
Private ORCD stages can select TCP/RDMA and an explicitly free 19xxx range.
Fixtures/logs remain for evidence finalization and checked cleanup by the caller.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
import k33_daemons as k


def run(a):
    a.work.mkdir(parents=True,exist_ok=False);a.output.mkdir(parents=True,exist_ok=False)
    env={key:val for key,val in os.environ.items() if not key.startswith('EFS_FAULT_')}
    env.update(EFS_TRANSPORT=a.transport,EFS_DIR_SPREAD_MIN='64')
    signal.signal(signal.SIGTERM,lambda *_:sys.exit(143)) # unwind through the finally below, as SIGINT does
    c=k.Cluster(a.work,'cluster',(1,2,3,4),env,hosts={i:a.bind for i in (1,2,3,4)})
    if a.base_port:
        for held in c.held:held.close()
        c.ports={i:a.base_port+i-1 for i in (1,2,3,4)}
        c.held=[k.hold_port(a.bind,p) for p in c.ports.values()]
    fourth_port=c.ports.pop(4) # place_leaders inspects only started members
    started=time.monotonic()
    pause=a.work/'readdir-pause';mount=None;pool=None;stop=threading.Event();errors=[];writer=None;churn_count=[0];churn_started=threading.Event()
    result=dict(verdict='RUNNING',transport=a.transport,checks=[],errors=errors)
    listing_serial=[0]
    def record(name):result['checks'].append(name);print('AO PASS',name,flush=True)
    try:
        for i in (1,2,3):c.start(i,join=None if i==1 else 1,extra_env={'EFS_FAULT_READDIR_LAYOUT_FILE':str(pause)})
        c.wait_voters((1,2,3),lambda i,v:v=={0:(1,7),2:(1,7)})
        mount=k.Mount(c,1,'mnt')
        target=mount.path/'ao-stable';target.mkdir();ino=target.stat().st_ino
        expected={f'stable-{i:04d}' for i in range(273)}
        def payload(name):return hashlib.sha256(name.encode()).digest()
        def put(name):
            with (target/name).open('wb',buffering=0) as f:
                assert f.write(payload(name))==32;os.fsync(f.fileno())
        with ThreadPoolExecutor(max_workers=8) as p:list(p.map(put,sorted(expected)))
        def listing():
            names=os.listdir(target)
            listing_serial[0]+=1
            (a.output/f'listing-{listing_serial[0]:04d}.json').write_text(json.dumps(
                dict(inode=ino,names=names,missing=sorted(expected-set(names))),indent=2)+'\n')
            assert len(names)==len(set(names)),('duplicate names',names)
            assert expected <= set(names),('missing stable names',sorted(expected-set(names)))
            assert all(n in expected or n.startswith('volatile-') for n in names),('unexpected names',names)
            return names
        assert set(listing())==expected;record('273 stable names across multiple pages')
        def churn():
            i=0
            try:
                while not stop.is_set():
                    p=target/f'volatile-{i:06d}';p.write_bytes(b'x');p.unlink();i+=1
                    churn_count[0]=i;churn_started.set()
            except BaseException as e:errors.append(repr(e))
        writer=threading.Thread(target=churn);writer.start()
        assert churn_started.wait(5),errors
        for _ in range(8):listing()
        record('stable committed names exactly once under same-directory create/unlink')
        k.place_leaders(c,1,groups=(0,2),timeout=15)
        pause.write_text(str(ino)+'\n');pool=ThreadPoolExecutor(max_workers=1)
        pending=pool.submit(listing)
        deadline=time.monotonic()+5
        while not Path(str(pause)+'.ready').exists() and time.monotonic()<deadline:time.sleep(.01)
        assert Path(str(pause)+'.ready').exists(),'row-cut pause did not fire'
        k.place_leaders(c,2,groups=(0,2),timeout=15)
        Path(str(pause)+'.release').touch();pending.result(timeout=30)
        assert any(f'released inode={ino} ready=1' in c.log(i) for i in (1,2,3)), 'pause expired'
        pause.unlink(missing_ok=True) # fault actor renames the request to .ready before pausing
        record('listing spans actual leader change after row cut')
        c.procs[3].send_signal(signal.SIGSTOP)
        try:
            for _ in range(4):listing()
        finally:c.procs[3].send_signal(signal.SIGCONT)
        record('listings with one halted replica')
        stop.set();writer.join(30);assert not writer.is_alive() and not errors,errors
        assert set(listing())==expected
        result['churn_operations']=churn_count[0]
        for name in expected:assert (target/name).read_bytes()==payload(name),name
        record('every stable payload exact after churn stops')
        c.ports[4]=fourth_port
        c.start(4,join=2);c.wait_members((1,2,3,4),4)
        # A non-host need not retain the other group's voter mask. Match
        # the established K33 hosting/readiness contract, not stale local state.
        def four_ready(i,v):
            if set(v)!={0,2}:return False
            if i==1:return v[0]==(1,7) and v[2][0]==0
            if i==4:return v[2]==(1,14) and v[0][0]==0
            return v=={0:(1,7),2:(1,14)}
        c.wait_voters((1,2,3,4),four_ready)
        for node in (1,4):
            src=0;cookie='';seen=set();pages=0
            while True:
                rc,text=k.mgmt(env,'raft-readdir',c.ep(node),str(ino),str(src),cookie)
                assert rc==0 and 'status=0 ' in text,(node,rc,text)
                m=re.search(r'count=(\d+) src=(\d+) done=(\d+) cookie=(.*?) names=(.*)',text.strip())
                assert m,(node,text)
                count,next_src,done=int(m[1]),int(m[2]),int(m[3]);next_cookie=m[4]
                names=m[5].split(',') if m[5] else []
                assert count==len(names) and set(names)<=expected,(node,text)
                seen.update(names);pages+=1
                assert pages<100,'forward cursor stuck'
                if done:break
                assert (next_src,next_cookie)>(src,cookie),'nonprogressing forward cursor'
                src,cookie=next_src,next_cookie
            assert seen==expected and pages>=2,(node,len(seen),pages)
        record('variable compact replies survive partial-group peer forwarding')
        if a.posix:
            argv=[sys.executable,str(ROOT/'tests/posix/posix_suite.py'),str(mount.path),
                  '--suite','core','--keep','--results',str(a.output/'posix.tsv'),'--tag','ao']
            with (a.output/'posix.log').open('w') as log:
                done=subprocess.run(argv,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
            assert done.returncode==0,('POSIX core failed',done.returncode)
            record('unchanged POSIX core including names_crazy_roundtrip')
        result['verdict']='PASS'
    except BaseException as e:result.update(verdict='FAIL',error=repr(e));raise
    finally:
        stop.set();Path(str(pause)+'.release').touch()
        if writer:writer.join(30)
        if pool:pool.shutdown(wait=True)
        try:
            if mount:
                done=subprocess.run([str(ROOT/'scripts/client.sh'),'stop',str(mount.path)],env=env,text=True,capture_output=True,timeout=90)
                (a.output/'mount-stop.txt').write_text(done.stdout+done.stderr)
                assert done.returncode==0 and not os.path.ismount(mount.path),'mount stop failed'
                assert mount.p.wait(timeout=30)==0;mount.log.close()
            for i,p in c.procs.items():
                if p.poll() is None:p.send_signal(signal.SIGCONT);p.send_signal(signal.SIGTERM)
                assert p.wait(timeout=60)==0,('daemon stop',i)
            for held in c.held:held.close()
        except BaseException as e:result.update(verdict='FAIL',cleanup_error=repr(e));raise
        finally:
            for p in c.procs.values():
                if p.poll() is None:p.send_signal(signal.SIGCONT)
            c.stop_all() # whatever the checked teardown above skipped: mounts, then every daemon
            if errors:result['verdict']='FAIL'
            for log in c.dir.glob('*.log'):(a.output/log.name).write_bytes(log.read_bytes())
            if a.transport == 'rdma' and result['verdict'] == 'PASS':
                evidence = {}
                for name in ('n1.log','n2.log','n3.log','n4.log','mnt.log'):
                    text=(a.output/name).read_text(errors='replace')
                    counters=re.findall(r'app_tx=(\d+)/(\d+) app_rx=(\d+)/(\d+).*?tcp_measured=(\d+).*?post_setup_tcp=(\d+)/(\d+) violations=(\d+)',text)
                    row=dict(connections=len(counters),
                             app_frames=sum(int(c[0])+int(c[2]) for c in counters),
                             tcp_measured=sum(int(c[4]) for c in counters),
                             post_setup_tcp_bytes=sum(int(c[5])+int(c[6]) for c in counters),
                             violations=sum(int(c[7]) for c in counters))
                    evidence[name]=row
                    if ('transport rdma:' not in text or
                        'RDMA transport up segmented-v1' not in text or
                        not row['app_frames'] or row['violations'] or
                        row['post_setup_tcp_bytes']):
                        errors.append('RDMA transport evidence failed '+name+' '+repr(row))
                result['rdma_evidence']=evidence
                if errors:result['verdict']='FAIL'
            result['elapsed_s']=time.monotonic()-started
            (a.output/'result.json').write_text(json.dumps(result,indent=2))
    assert result['verdict']=='PASS',result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--transport',choices=('tcp','rdma'),default='tcp')
    p.add_argument('--bind',default='127.0.0.1');p.add_argument('--base-port',type=int)
    p.add_argument('--posix',action='store_true');a=p.parse_args()
    if a.base_port:assert 19000<=a.base_port<=19996,'private ORCD ports only'
    run(a)
