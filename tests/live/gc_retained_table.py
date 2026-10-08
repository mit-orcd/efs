#!/usr/bin/env python3
"""Reopen a harness-owned GC fixture and measure metadata after competing jobs stop."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--fixture',type=Path,required=True)
p.add_argument('--drain-only',action='store_true',help='complete a large IOR fixture deletion interrupted by its smoke-test timeout')
p.add_argument('--idle-seconds',type=int,default=60)
a=p.parse_args();assert 10<=a.idle_seconds<=600
source=Path(__file__).resolve().parents[2]
work=a.fixture.resolve();manifest=json.loads((work/'manifest.json').read_text())
assert work.parent==Path('/data1/efs') and work.name.startswith('gc-direct-')
assert manifest['gates'].get('gc_bulk_gib')==10 or (a.drain_only and manifest['gates'].get('ior_binary') and manifest['gates'].get('ior_ranks'))
for name,expected in manifest['binary_sha256'].items():
    assert hashlib.sha256((source/name).read_bytes()).hexdigest()==expected,('binary differs from retained fixture',name)
roots=[Path(x) for x in manifest['roots']];port=int(manifest['port']);assert 1024<=port<=65531
assert [str(root.resolve()) for root in roots]==manifest['real_roots']
assert roots[0]==work/'n1' and roots[1]==work/'n2'
assert roots[2].parent.parent==Path('/data2/efs') and roots[2].parent.name.startswith('gc-direct-')
for number in range(port,port+4):
    with socket.socket() as sock:sock.bind(('127.0.0.1',number))
mount=work/'quiet-mnt';mount.mkdir(exist_ok=True)
assert subprocess.run(['mountpoint','-q',str(mount)]).returncode!=0
output=work/('quiet-probe-'+str(time.time_ns()));output.mkdir()
env=dict(os.environ,EFS_MD_RAFT_N='4',EFS_TRANSPORT='tcp')
servers=[];client=None;logs=[]
def run(args,**kw):return subprocess.run([str(x) for x in args],check=True,env=env,timeout=kw.pop('timeout',60),**kw)
def wait(fn,seconds=120):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if fn():return
        time.sleep(.5)
    raise AssertionError('retained fixture did not settle')
def statuses():
    return [run([source/'efs-mgmt','gc-status',f'127.0.0.1:{port+i}'],capture_output=True,text=True).stdout for i in range(4)]
def drained():
    import re
    rows=[dict(re.findall(r'(\w+)=(\d+)',line)) for text in statuses() for line in text.splitlines() if line.startswith('group=') and 'sampled=1' in line]
    return {r['group'] for r in rows}=={'0','2'} and all(r['pending_estimate']==r['reap_seen']==r['orphan_seen']=='0' for r in rows)
try:
    for i,root in enumerate(roots):
        log=open(output/f'n{i+1}.log','ab');logs.append(log)
        args=[source/'efsd','--node-id',i+1,'--addr','127.0.0.1','--port',port+i,'--storage',root,'--quota','16G','--direct-io']
        if i:args+=['--join',f'127.0.0.1:{port}']
        servers.append(subprocess.Popen([str(x) for x in args],env=env,stdout=log,stderr=log))
    time.sleep(3)
    log=open(output/'fuse.log','ab');logs.append(log)
    client=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{port}','default',str(mount),'-f'],env=env,stdout=log,stderr=log)
    wait(lambda:client.poll() is None and subprocess.run(['mountpoint','-q',str(mount)]).returncode==0)
    wait(drained,1800)
    if a.drain_only:
        import re
        current=statuses()
        usage=sum(int(re.search(r'data_usage_bytes=(\d+)',text).group(1)) for text in current)
        assert usage==0,('drained IOR queues but retained physical usage',usage)
        (output/'drain-complete.json').write_text(json.dumps({'data_usage_bytes':usage,'status':current,'original_fixture':str(work)},indent=2))
    # Retain any still-live witness data. Only mdlat creates/deletes its own names.
    before=statuses();samples=[];offsets=[(output/f'n{i+1}.log').stat().st_size for i in range(4)]
    started=time.monotonic();deadline=started+a.idle_seconds
    while time.monotonic()<deadline:time.sleep(min(10,max(0,deadline-time.monotonic())))
    for i in range(3):
        result=run(['python3',source/'tests/measure/md_latency.py',mount],capture_output=True,text=True,timeout=180)
        (output/f'mdlat-{i}.log').write_text(result.stdout+result.stderr);samples.append(result.stdout)
    slow=[]
    for i,offset in enumerate(offsets):
        with (output/f'n{i+1}.log').open('rb') as inp:inp.seek(offset);text=inp.read().decode(errors='replace')
        slow.extend({'node':i+1,'line':line} for line in text.splitlines() if 'gc-pass ' in line)
    (output/'summary.json').write_text(json.dumps({'elapsed_seconds':time.monotonic()-started,'source_fixture':str(work),'before_gc':before,'after_gc':statuses(),'metadata_samples':samples,'slow_passes':slow},indent=2))
    run([source/'scripts/client.sh','stop',mount]);client.wait(timeout=15);client=None
    print('Retained-table quiet metadata probe PASS: '+str(output),flush=True)
finally:
    if client:
        subprocess.run(['fusermount3','-uz',str(mount)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        client.terminate()
        try:client.wait(timeout=5)
        except subprocess.TimeoutExpired:client.kill();client.wait()
    for proc in servers:
        if proc.poll() is None:proc.terminate()
    for proc in servers:
        try:proc.wait(timeout=20)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
    for log in logs:log.close()
