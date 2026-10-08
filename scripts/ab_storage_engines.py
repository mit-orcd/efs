#!/usr/bin/env python3
"""Isolated xorinox VM A/B. Never changes installed efsd or /mnt/efs services."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import time

NODES=['192.168.122.111','192.168.122.112','192.168.122.113']
GW='192.168.122.114'
BIN='/tmp/efs-engine-ab-bin'
PORT=27432
BASE='/data1/efs/bench/engine-ab-20261008'
MOUNT=BASE+'/mnt'

def main():
    global BASE,MOUNT
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ssh-key',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--base',default=BASE)
    p.add_argument('--resume',action='store_true',help='skip completed cases; requires cleanly stopped private group')
    p.add_argument('--seconds',type=int,default=0,choices=range(0,121))
    p.add_argument('--repeat',type=int,default=2,choices=range(1,5))
    a=p.parse_args()
    if not a.base.startswith('/data1/efs/bench/') or '..' in Path(a.base).parts:raise ValueError('private benchmark root required')
    BASE=a.base;MOUNT=BASE+'/mnt'
    a.output.mkdir(parents=True,exist_ok=a.resume)
    def remote(ip,cmd,timeout=120):
        argv=['ssh','-J','xorinox_efs','-i',str(a.ssh_key),'-o','BatchMode=yes',f'admin@{ip}',cmd]
        with (a.output/'commands.jsonl').open('a') as f:f.write(json.dumps({'ip':ip,'command':cmd})+'\n')
        r=subprocess.run(argv,text=True,capture_output=True,timeout=timeout)
        if r.returncode:raise RuntimeError(f'{ip}: {cmd}\n{r.stdout}\n{r.stderr}')
        return r.stdout+r.stderr
    def as_efs(ip,argv,timeout=120):return remote(ip,shlex.join(['sudo','-n','-H','-u','efs',*argv]),timeout)
    def start(name,engine,mode,restart=False):
        for i,ip in enumerate(NODES,1):
            root=f'{BASE}/{name}'
            remote(ip,shlex.join(['sudo','-n','install','-d','-o','efs','-g','efs',root]))
            argv=['env','EFS_MD_RAFT_N=3','EFS_TRANSPORT=tcp',BIN+'/efsd','--node-id',str(i),'--addr',ip,
                  '--port',str(PORT),'--storage',root+'/store','--quota','2G','--writers','2',
                  '--storage-engine',engine,'--direct-io' if mode=='direct' else '--no-direct-io']
            if i>1:argv+=['--join',NODES[0]+':'+str(PORT)]
            script='nohup '+shlex.join(argv)+' > '+shlex.quote(root+('/restart.log' if restart else '/server.log'))+' 2>&1 </dev/null & echo $! > '+shlex.quote(root+'/pid')
            as_efs(ip,['bash','-c',script])
        seed=NODES[0]+':'+str(PORT)
        for attempt in range(40):
            try:
                status=as_efs(NODES[0],[BIN+'/efs-mgmt','status',seed],20)
                if all(ip in status for ip in NODES):break
            except (RuntimeError,subprocess.TimeoutExpired):pass
            time.sleep(.5)
        else:raise RuntimeError('private group did not converge')
        if not restart:as_efs(NODES[0],[BIN+'/efs-mgmt','raft-mkfs',seed,'fs'],180)
        remote(GW,shlex.join(['sudo','-n','install','-d','-o','efs','-g','efs',MOUNT,BASE+'/'+name]))
        mountcmd=['env','EFS_MD_RAFT_N=3','EFS_TRANSPORT=tcp',BIN+'/efs-fuse',seed,'fs',MOUNT,'-o','allow_other']
        as_efs(GW,['bash','-c',shlex.join(mountcmd)+' > '+shlex.quote(BASE+'/'+name+'/client.log')+' 2>&1'],90)
        for attempt in range(40):
            if remote(GW,'mountpoint -q '+shlex.quote(MOUNT)+' && echo mounted || true').strip()=='mounted':break
            time.sleep(.5)
        else:raise RuntimeError('private mount did not appear')
        return status
    def stop(name):
        as_efs(GW,[BIN+'/efs-fuse','--stop',MOUNT],90)
        as_efs(GW,['fusermount3','-u',MOUNT])
        for ip in NODES:
            root=f'{BASE}/{name}'
            # Verify exact private storage root before signalling the recorded PID.
            code="import os,signal,time;from pathlib import Path;p=int(Path("+repr(root+'/pid')+").read_text());cmd=Path('/proc/%d/cmdline'%p).read_bytes();assert "+repr((root+'/store').encode())+" in cmd;os.kill(p,signal.SIGTERM)"
            as_efs(ip,['python3','-c',code])
        for ip in NODES:
            code="import time;from pathlib import Path;p=int(Path("+repr(f'{BASE}/{name}/pid')+").read_text());f=Path('/proc/%d/cmdline'%p);deadline=time.monotonic()+45\nwhile f.exists() and f.read_bytes():\n if time.monotonic()>deadline:raise RuntimeError('private daemon did not stop')\n time.sleep(.2)"
            as_efs(ip,['python3','-c',code],60)
    # Refuse reuse before mkfs: an existing daemon on the private port could
    # otherwise answer our readiness probe and receive initialization commands.
    previous=json.loads((a.output/'results.json').read_text()) if a.resume else []
    assert all(row['status']=='PASS' for row in previous)
    completed={row['name'] for row in previous}
    names=[f'{engine}-{mode}-r{r}' for r in range(1,a.repeat+1)
           for engine in ('files','containers') for mode in ('buffered','direct')
           if f'{engine}-{mode}-r{r}' not in completed]
    for ip in NODES:
        if remote(ip,shlex.join(['ss','-H','-ltn',f'sport = :{PORT}'])).strip():
            raise RuntimeError(f'{ip}: private test port {PORT} already occupied')
    for ip in [*NODES,GW]:
        paths=[BASE+'/'+name for name in names]
        code="import os;paths="+repr(paths)+";assert not any(os.path.lexists(p) for p in paths),'existing case directory; choose a fresh --base'"
        remote(ip,shlex.join(['python3','-c',code]))
    if remote(GW,'mountpoint -q '+shlex.quote(MOUNT)+' && echo mounted || true').strip():
        raise RuntimeError('private mount already occupied')
    rows=previous
    baseline={ip:remote(ip,'pgrep -a -x efsd || true') for ip in NODES}
    baseline[GW]=remote(GW,'pgrep -a -x efs-fuse || true')
    if a.resume:
        assert baseline==json.loads((a.output/'installed-before.json').read_text()),'installed processes changed before resume'
    else:(a.output/'installed-before.json').write_text(json.dumps(baseline,indent=2)+'\n')
    order=[('files','buffered'),('containers','direct'),('containers','buffered'),('files','direct')]
    for repeat in range(1,a.repeat+1):
        for engine,mode in (order if repeat%2 else order[::-1]):
            name=f'{engine}-{mode}-r{repeat}'
            if name in completed:continue
            out=a.output/name;out.mkdir()
            print('START '+name,flush=True)
            (out/'status-start.txt').write_text(start(name,engine,mode))
            phases=[]
            for phase in ('create','read','churn','verify'):
                path=f'{BASE}/{name}/{phase}.json'
                result=as_efs(GW,['python3',BIN+'/storage_engine_ab.py',phase,'--mount',MOUNT,
                    '--state',f'{BASE}/{name}/expected.json','--output',path,
                    '--seconds',str(a.seconds if phase in ('read','churn') else 0)],600)
                row=json.loads(result.strip().splitlines()[-1]);phases.append(row);(out/(phase+'.json')).write_text(json.dumps(row,indent=2)+'\n')
            inventory=[json.loads(as_efs(ip,['python3',BIN+'/inventory.py',f'{BASE}/{name}/store']).strip()) for ip in NODES]
            (out/'inventory-live.json').write_text(json.dumps(inventory,indent=2)+'\n')
            stop(name)
            (out/'status-restart.txt').write_text(start(name,engine,mode,restart=True))
            for phase in ('verify','delete'):
                result=as_efs(GW,['python3',BIN+'/storage_engine_ab.py',phase,'--mount',MOUNT,
                    '--state',f'{BASE}/{name}/expected.json','--output',f'{BASE}/{name}/restart-{phase}.json'],600)
                row=json.loads(result.strip().splitlines()[-1]);row['phase']='restart-'+phase;phases.append(row)
                (out/('restart-'+phase+'.json')).write_text(json.dumps(row,indent=2)+'\n')
            # GC is asynchronous. Preserve its progress and require converged zero usage.
            converged=False
            for attempt in range(60):
                status=as_efs(NODES[0],[BIN+'/efs-mgmt','status',NODES[0]+':'+str(PORT)],30)
                (out/'status-delete.txt').write_text(status)
                # Use actual persisted mapping/fragment inventory, excluding metadata inode.
                inv=[]
                for ip in NODES:
                    cmd=['python3',BIN+'/inventory.py',f'{BASE}/{name}/store']
                    inv.append(json.loads(as_efs(ip,cmd).strip()))
                (out/'inventory.json').write_text(json.dumps(inv,indent=2)+'\n')
                if all(v['data_records']==0 and v['container_allocated']==0 for v in inv):converged=True;break
                time.sleep(1)
            if not converged:raise RuntimeError('GC did not retire all data records; private group retained')
            for i,ip in enumerate(NODES,1):
                for file in ('server.log','restart.log'):
                    (out/f'node{i}-{file}').write_text(remote(ip,'cat '+shlex.quote(f'{BASE}/{name}/{file}')))
                (out/f'gc-node{i}.txt').write_text(as_efs(ip,[BIN+'/efs-mgmt','gc-status',ip+':'+str(PORT)],30))
            stop(name)
            rows.append({'name':name,'engine':engine,'io':mode,'repeat':repeat,'status':'PASS','phases':phases})
            (a.output/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print('PASS '+name,flush=True)
    after={ip:remote(ip,'pgrep -a -x efsd || true') for ip in NODES};after[GW]=remote(GW,'pgrep -a -x efs-fuse || true')
    (a.output/'installed-after.json').write_text(json.dumps(after,indent=2)+'\n')
    assert baseline==after,'installed daemon/client processes changed'
    (a.output/'finished').write_text('PASS\n')
if __name__=='__main__':main()
