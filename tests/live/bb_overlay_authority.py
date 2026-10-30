#!/usr/bin/env python3
"""BB real two-client authority matrix; fresh private cluster, retain failures.

Pass --source for an immutable matching-build export. Management CHOWN is
intentional trusted-network authority injection, not a privilege bypass test.
No existing cluster, kernel policy or user credential is changed.
"""
import argparse, errno, hashlib, json, os, signal, subprocess, sys, time, traceback
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2])
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();source=a.source.resolve();out=a.output.absolute()
assert not out.exists();out.mkdir(parents=True)
sys.path.insert(0,str(source/'tests'))
from k33_daemons import Cluster,Mount,mgmt,payload
assert os.geteuid()!=0,'DAC gate must run as an unprivileged client user'
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_FAULT_')}
c=Cluster(out,'cluster',[1,2,3],env);mounts=[];fds=[]
result=dict(verdict='RUNNING',source=str(source),transport=env.get('EFS_TRANSPORT','tcp'),cases=[],cleanup_errors=[])
def save():
    t=out/'result.tmp';t.write_text(json.dumps(result,indent=2)+'\n');t.replace(out/'result.json')
def check(name,fn):
    begin=time.monotonic()
    try:fn();row=dict(name=name,verdict='PASS')
    except Exception:row=dict(name=name,verdict='FAIL',error=traceback.format_exc())
    row['elapsed_s']=time.monotonic()-begin;result['cases'].append(row);save();print(json.dumps(row),flush=True)
def opened(path,flags):
    fd=os.open(path,flags,0o600);fds.append(fd);return fd
def denied(path,flags):
    try:fd=os.open(path,flags,0o600)
    except OSError as e:assert e.errno==errno.EACCES,(path,e);return
    os.close(fd);raise AssertionError('stale local authority granted '+str(path))
def observed(label,got,want):
    record=dict(label=label,actual_length=len(got),expected_length=len(want),
                actual_sha256=hashlib.sha256(got).hexdigest(),expected_sha256=hashlib.sha256(want).hexdigest())
    result.setdefault('observations',[]).append(record);save()
    if got!=want:
        (out/(label+'-actual.bin')).write_bytes(got)
        (out/(label+'-expected.bin')).write_bytes(want)
        raise AssertionError(record)

def chown_authority(ino,uid,gid):
    rc,text=mgmt(env,'raft-setattr',c.ep(1),str(ino),'6','0',str(uid),str(gid))
    assert rc==0 and 'status=0' in text,text
def transport_proof(stage):
    if result['transport']!='rdma':return
    proof=[]
    for proc in [*c.procs.values(),*(m.p for m in mounts)]:
        deadline=time.monotonic()+20;devices=[]
        while not devices and proc.poll() is None:
            for fd in (Path('/proc')/str(proc.pid)/'fd').iterdir():
                try:target=os.readlink(fd)
                except FileNotFoundError:continue
                if '/dev/infiniband/uverbs' in target:devices.append(target)
            if devices or time.monotonic()>=deadline:break
            time.sleep(.2)
        assert devices,('forced RDMA matrix process has no verbs device',stage,proc.pid)
        proof.append(dict(pid=proc.pid,verbs_devices=sorted(set(devices))))
    result.setdefault('rdma_runtime',[]).append(dict(stage=stage,processes=proof));save()
try:
    for i in (1,2,3):c.start(i,join=None if i==1 else 1)
    c.wait_members([1,2,3],3);c.leader_placement(1)
    mounts=[Mount(c,1,'a'),Mount(c,2,'b')];A,B=[m.path for m in mounts]
    transport_proof('mounted')
    def chmod_dirty():
        fd=opened(A/'chmod',os.O_CREAT|os.O_RDWR);data=payload('bb-chmod',131077);assert os.write(fd,data)==len(data)
        try:
            os.chmod(B/'chmod',0)
            assert os.pread(fd,len(data),0)==data,'same-fd accepted bytes lost'
            assert os.fstat(fd).st_mode&0o777==0,'open getattr hid remote chmod'
            denied(A/'chmod',os.O_RDONLY)
        finally:os.chmod(B/'chmod',0o600)
        os.fsync(fd);assert (B/'chmod').read_bytes()==data
    check('remote-chmod-dirty-open',chmod_dirty)
    def chown_dirty():
        fd=opened(A/'chown',os.O_CREAT|os.O_RDWR);data=payload('bb-chown',4097);os.write(fd,data);ino=os.fstat(fd).st_ino
        try:
            chown_authority(ino,os.getuid()+10000,os.getgid()+10000)
            st=os.fstat(fd);assert (st.st_uid,st.st_gid)==(os.getuid()+10000,os.getgid()+10000),'open getattr hid remote owner'
            denied(A/'chown',os.O_RDONLY);assert os.pread(fd,len(data),0)==data
        finally:chown_authority(ino,os.getuid(),os.getgid())
        os.fsync(fd);assert (B/'chown').read_bytes()==data
    check('remote-chown-dirty-open',chown_dirty)
    def rename_link_unlink():
        fd=opened(A/'old',os.O_CREAT|os.O_RDWR);data=payload('bb-names',4097);os.write(fd,data)
        os.rename(B/'old',B/'new');assert not (A/'old').exists();assert (A/'new').stat().st_ino==os.fstat(fd).st_ino
        os.link(B/'new',B/'alias');assert os.fstat(fd).st_nlink==2
        os.unlink(B/'alias');os.unlink(B/'new');assert os.fstat(fd).st_nlink==0
        assert os.pread(fd,len(data),0)==data;os.fsync(fd)
    check('remote-rename-link-unlink-dirty-open',rename_link_unlink)
    def same_chmod():
        fd=opened(A/'same-mode',os.O_CREAT|os.O_RDWR);os.write(fd,b'x');os.fsync(fd)
        os.chmod(B/'same-mode',0o400);os.chmod(A/'same-mode',0o600)
        assert (B/'same-mode').stat().st_mode&0o777==0o600,'cached same-mode suppressed committed chmod'
    check('cached-same-mode-is-not-authority',same_chmod)
    def same_truncate():
        fd=opened(A/'same-size',os.O_CREAT|os.O_RDWR)
        with (B/'same-size').open('wb') as f:f.write(payload('bb-peer-extend',300000));f.flush();os.fsync(f.fileno())
        os.ftruncate(fd,0);assert (B/'same-size').stat().st_size==0,'cached same-size suppressed truncate'
    check('cached-same-size-is-not-authority',same_truncate)
    def shrink_reader():
        data=payload('bb-reader',300000)
        with (A/'reader').open('wb') as f:f.write(data);f.flush();os.fsync(f.fileno())
        fd=opened(B/'reader',os.O_RDONLY);assert os.pread(fd,len(data),0)==data
        os.truncate(A/'reader',65536);assert os.fstat(fd).st_size==65536,'open getattr hid peer truncate'
        observed('after-shrink',os.pread(fd,4096,131072),b'')
        os.truncate(A/'reader',300000)
        # A held descriptor's metadata EOF refreshes at OPEN/GETATTR, not
        # on every read. Establish that boundary before demanding tail bytes.
        size=os.fstat(fd).st_size
        result.setdefault('observations',[]).append(dict(label='after-grow-EOF',actual_size=size,expected_size=300000));save()
        assert size==300000,('after-grow-EOF',size)
        observed('after-grow-masked-tail',os.pread(fd,4096,131072),bytes(4096))
    check('truncate-under-open-reader',shrink_reader)
    def namespace_permission():
        (A/'deny-dir').mkdir();(A/'deny-dir/file').write_bytes(b'x');os.stat(A/'deny-dir')
        try:
            os.chmod(B/'deny-dir',0o500)
            denied(A/'deny-dir/new',os.O_CREAT|os.O_WRONLY)
        finally:os.chmod(B/'deny-dir',0o700)
    check('remote-directory-write-permission',namespace_permission)
    def rpc_failure():
        fd=opened(A/'rpc-failure',os.O_CREAT|os.O_RDWR);os.write(fd,b'accepted');os.fsync(fd)
        for i in (3,2,1):c.stop(i)
        try:
            try:new=os.open(A/'rpc-failure',os.O_RDONLY)
            except OSError as e:assert e.errno in (errno.EIO,errno.EBUSY,errno.EAGAIN),e
            else:os.close(new);raise AssertionError('new OPEN granted from cached row without authority')
        finally:
            for i in (1,2,3):c.restart(i)
            c.wait_members([1,2,3],3);c.leader_placement(1)
        assert os.pread(fd,8,0)==b'accepted'
        transport_proof('after-owned-member-restart')
    check('rpc-failure-refuses-new-open',rpc_failure)
finally:
    for fd in fds:
        try:os.close(fd)
        except OSError as e:result['cleanup_errors'].append('close '+str(e))
    for m in reversed(mounts):
        try:
            stop=subprocess.run([str(source/'scripts/client.sh'),'stop',str(m.path)],capture_output=True,text=True,timeout=45)
            (out/(m.path.name+'-stop.log')).write_text(stop.stdout+stop.stderr)
            assert stop.returncode==0,(stop.returncode,stop.stdout,stop.stderr)
            assert m.p.wait(timeout=15)==0 and not os.path.ismount(m.path)
            assert 'unmount not clean' not in m.logp.read_text(errors='replace')
        except Exception:result['cleanup_errors'].append(traceback.format_exc())
    for i,proc in list(c.procs.items()):
        try:
            if proc.poll() is None:proc.send_signal(signal.SIGTERM)
            assert proc.wait(timeout=30)==0,(i,proc.returncode)
        except Exception:result['cleanup_errors'].append(traceback.format_exc())
    for h in c.held:h.close()
    result['verdict']='PASS' if len(result['cases'])==8 and all(x['verdict']=='PASS' for x in result['cases']) and not result['cleanup_errors'] else 'FAIL'
    save();print('BB overlay authority',result['verdict'],out,flush=True)
sys.exit(0 if result['verdict']=='PASS' else 1)
