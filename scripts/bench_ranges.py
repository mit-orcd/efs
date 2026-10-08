#!/usr/bin/env python3
"""Private shared-container positional-I/O contention matrix; unsafe modes are diagnostic."""
import argparse
import datetime
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
def ints(value):
    result=[int(n) for n in value.split(',')]
    if not result or any(n<1 or n>64 for n in result):raise argparse.ArgumentTypeError('workers must be 1..64')
    return result
def parser():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,default=ROOT/'efs-bench')
    p.add_argument('--storage-root',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--qds',type=ints,default=[1,4,8])
    p.add_argument('--accesses',default='disjoint,hot-read,hot-locked,hot-unlocked')
    p.add_argument('--io-modes',default='direct,buffered')
    p.add_argument('--seconds',type=int,default=20)
    p.add_argument('--steps',type=int,default=10000000)
    p.add_argument('--container-bytes',type=int,default=1024**3)
    p.add_argument('--payload-size',type=int,default=65536)
    p.add_argument('--persist',choices=['none','each','batch'],default='batch')
    p.add_argument('--split-write',action='store_true')
    p.add_argument('--punch-before-write',action='store_true')
    p.add_argument('--repeat',type=int,default=1)
    p.add_argument('--timeout',type=int,default=300)
    p.add_argument('--dry-run',action='store_true')
    return p
def cases(a):
    accesses=a.accesses.split(',');modes=a.io_modes.split(',')
    if any(s not in ['disjoint','hot-read','hot-locked','hot-unlocked'] for s in accesses):raise ValueError('invalid access')
    if any(s not in ['direct','buffered'] for s in modes):raise ValueError('invalid I/O mode')
    if not 0<=a.seconds<=3600 or not 1<=a.steps<=10000000 or not 1<=a.repeat<=10 or a.timeout<1:raise ValueError('invalid duration/cap/repeat')
    if not 8192<=a.payload_size<=1048576 or a.payload_size%4096 or not 65536<=a.container_bytes<=4*1024**3 or a.container_bytes%4096:raise ValueError('invalid geometry')
    for qd,access,mode,repeat in itertools.product(a.qds,accesses,modes,range(a.repeat)):
        args=['--bench','ranges','--access',access,'--qd',str(qd),'--steps',str(a.steps),'--seconds',str(a.seconds),
              '--container-bytes',str(a.container_bytes),'--payload-size',str(a.payload_size),'--persist',a.persist,
              '--direct-io' if mode=='direct' else '--no-direct-io']
        if access in ['hot-locked','hot-unlocked']:
            if a.split_write:args+=['--split-write']
            if a.punch_before_write:args+=['--punch-before-write']
        yield dict(name=f'{access}-qd{qd}-{mode}-r{repeat+1}',args=args,access=access,qd=qd,mode=mode)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def report(out,rows):
    text=['# Shared-container range observations','',
          'Unsafe hot-unlocked cases are diagnostics, not successful storage protocols. Split writes deliberately expose a multi-call publication window. No crash recovery or durable index is modeled. Direct and buffered I/O run in separate cases.',
          '', '| Access | Workers | I/O | Status | Ops/s | Cycle p99 ms | Cycle max ms | Lock p99 ms | Mixed-record observations |',
          '|---|---:|---|---|---:|---:|---:|---:|---:|']
    for row in rows:
        f=row.get('fields',{})
        text.append(f"| {row['access']} | {row['qd']} | {row['mode']} | {row['status']} | {float(f.get('ops_s',0)):.0f} | {int(f.get('cycle_p99_us',0))/1000:.3f} | {int(f.get('cycle_max_us',0))/1000:.3f} | {int(f.get('lock_p99_us',0))/1000:.3f} | {f.get('torn_observations','—')} |")
    (out/'SUMMARY.md').write_text('\n'.join(text)+'\n');(out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
def main():
    a=parser().parse_args();matrix=list(cases(a));binary=a.binary.resolve()
    if a.dry_run:
        for c in matrix:print(c['name'],json.dumps(c['args']))
        return 0
    parent=a.storage_root.resolve();out=a.output.resolve()
    if not parent.is_dir() or not binary.is_file():raise ValueError('existing scratch parent and built binary required')
    out.mkdir(parents=True,exist_ok=False)
    files=['src/bench/storage_ranges.c','src/bench/storage_ranges.h','src/bench/latency.h','scripts/bench_ranges.py','src/client/efs_bench.c','Makefile']
    manifest=dict(arguments=vars(a),binary_sha256=digest(binary),sources={n:digest(ROOT/n) for n in files},uname=os.uname(),started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    (out/'manifest.json').write_text(json.dumps(manifest,default=str,indent=2)+'\n');shutil.copy2(binary,out/'efs-bench')
    facts=subprocess.run(['findmnt','-T',str(parent),'-o','TARGET,SOURCE,FSTYPE,OPTIONS'],text=True,capture_output=True)
    (out/'filesystem.txt').write_text(facts.stdout+facts.stderr)
    rows=[]
    for i,c in enumerate(matrix,1):
        case=out/c['name'];case.mkdir();scratch=Path(tempfile.mkdtemp(prefix='w90-ranges-',dir=parent))
        cmd=[str(binary),*c['args'],'--storage',str(scratch)]
        (case/'command.json').write_text(json.dumps(cmd,indent=2)+'\n');print(f'[{i}/{len(matrix)}] {c["name"]}',flush=True)
        try:
            p=subprocess.run(cmd,text=True,capture_output=True,timeout=a.timeout)
            stdout,stderr=p.stdout,p.stderr
            line=next((s for s in stdout.splitlines() if s.startswith('BENCH_OK ')),None)
            f=dict(re.findall(r'(\w+)=([^\s]+)',line or ''))
            diagnostic=c['access']=='hot-unlocked'
            valid=p.returncode==0 and f.get('errors')=='0' and (diagnostic or (f.get('verified_records')==f.get('records') and f.get('torn_observations')=='0'))
            c.update(status=('OBSERVATION' if diagnostic else 'PASS') if valid else 'FAIL',fields=f,returncode=p.returncode)
        except subprocess.TimeoutExpired as e:
            stdout=e.stdout or b'';stderr=e.stderr or b''
            if isinstance(stdout,bytes):stdout=stdout.decode(errors='replace')
            if isinstance(stderr,bytes):stderr=stderr.decode(errors='replace')
            c.update(status='TIMEOUT',fields={},returncode=None)
        (case/'stdout.txt').write_text(stdout);(case/'stderr.txt').write_text(stderr)
        rows.append(c);report(out,rows)
        if c['status'] in ['FAIL','TIMEOUT']:
            print(f'Failed case retained: {scratch}',flush=True);return 1
        if list(scratch.iterdir()):raise RuntimeError('completed case left files behind')
        scratch.rmdir();print(c['status'],flush=True)
    return 0
if __name__=='__main__':
    try:raise SystemExit(main())
    except ValueError as e:raise SystemExit(str(e))
