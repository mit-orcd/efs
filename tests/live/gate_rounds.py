#!/usr/bin/env python3
"""Serial NUC acceptance matrix; each round retains its command, log and result.

Run in an isolated built source tree. A unit-only round does not close a live
gate, and a pass does not establish another host/transport's acceptance.
Failures stop the matrix; --start resumes explicitly after diagnosis.
"""
import argparse
import hashlib
import json
import signal
import shutil
from pathlib import Path
import subprocess
import time

source=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--start',type=int,default=1)
parser.add_argument('--end',type=int,default=30)
args=parser.parse_args()
assert 1<=args.start<=args.end<=30
args.output.mkdir(parents=True,exist_ok=True)
# Freeze inputs before launching a matrix. Editing the development tree during
# a long run must not change later fixtures, compiler inputs or traceback text.
snapshot=args.output/'source'
shutil.copytree(source,snapshot,ignore=shutil.ignore_patterns('.git','logs','__pycache__'))
source=snapshot

def gc(mode,*flags):
    return ['python3','tests/live/gc_reclamation.py','--mode',mode,'--port','20510',*flags]
rounds=[
 ('W54/W38 authoritative fold and cold GC',gc('direct','--fold-audit')),
 ('W54/W38 buffered fold and cold GC',gc('buffered','--fold-audit')),
 ('W59 sustained direct writes and cold bytes',gc('direct','--sequential')),
 ('W59 sustained buffered writes and cold bytes',gc('buffered','--sequential')),
 ('W69 immediate local mutation stats direct',gc('direct','--memo')),
 ('W69 immediate local mutation stats buffered',gc('buffered','--memo')),
 ('W56 root rename/name reuse direct',gc('direct','--rename-cache')),
 ('W56 root rename/name reuse buffered',gc('buffered','--rename-cache')),
 ('W60 bounded mixed demand/prefetch direct',gc('direct','--mixed-reads')),
 ('W60 bounded mixed demand/prefetch buffered',gc('buffered','--mixed-reads')),
 ('D17 cold dense/sparse allocation direct',gc('direct','--allocation')),
 ('D17 cold dense/sparse allocation buffered',gc('buffered','--allocation')),
 ('W68 real worker retirement direct',gc('direct','--workers')),
 ('W68 real worker retirement buffered',gc('buffered','--workers')),
 ('W67 sparse bounded admission direct',gc('direct','--pressure')),
 ('W67 sparse bounded admission buffered',gc('buffered','--pressure')),
 ('W75/W85 RPC integrity, ambiguous PUT/restart/accounting', ['python3','tests/live/fragment_integrity.py']),
 ('W75 parallel GET payload and metadata anchor', ['python3','tests/test_read_verify.py']),
 ('W65 pooled/partial-reader signal shutdown', ['python3','tests/live/server_shutdown.py']),
 ('W82 reachable stale leader partitions', ['python3','tests/live/raft_read_freshness.py']),
 ('W76 lost committed namespace replies direct',gc('direct','--opid')),
 ('W76 lost committed namespace replies buffered',gc('buffered','--opid')),
 ('W84 namespace bounds/deep/spread direct',gc('direct','--namespace-boundary')),
 ('W84 namespace bounds/deep/spread buffered',gc('buffered','--namespace-boundary')),
 ('W85 hint eviction/concurrency production helper', ['python3','tests/test_put_hint.py']),
 ('W27 phantom ownership and ambiguous REPORT helper', ['python3','tests/test_report_orphan.py']),
 ('W23/W89 strict pressure measurement schema', ['python3','tests/test_w23_samples.py']),
 ('Full Linux unit and architecture regressions', ['make','test']),
 ('Combined direct POSIX/GC/namespace/worker gate',gc('direct','--fold-audit','--memo','--rename-cache','--allocation','--integrity','--workers','--opid','--namespace-boundary','--posix')),
 ('Combined buffered POSIX/GC/namespace/worker gate',gc('buffered','--fold-audit','--memo','--rename-cache','--allocation','--integrity','--workers','--opid','--namespace-boundary','--posix')),
]
for number in range(args.start,args.end+1):
    title,command=rounds[number-1]
    record={'round':number,'title':title,'command':command,'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
            'test_sha256':{arg:hashlib.sha256((source/arg).read_bytes()).hexdigest() for arg in command if (source/arg).is_file()},
            'runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'binary_sha256':{name:hashlib.sha256((source/name).read_bytes()).hexdigest() for name in ('efsd','efs-fuse','efs-mgmt')}}
    path=args.output/f'round{number:02}.json'
    assert not path.exists(),f'refusing to replace evidence {path}'
    start=time.monotonic()
    print(f'ROUND {number:02}: {title}',flush=True)
    with (args.output/f'round{number:02}.log').open('x') as log:
        proc=subprocess.Popen(command,cwd=source,stdout=log,stderr=subprocess.STDOUT)
        try:record['exit_code']=proc.wait(timeout=1200)
        except subprocess.TimeoutExpired:
            # Let Python fixtures execute their owned-process finally blocks.
            proc.send_signal(signal.SIGINT)
            try:proc.wait(timeout=90)
            except subprocess.TimeoutExpired:record['cleanup_incomplete_pid']=proc.pid
            record['exit_code']=124
    record['elapsed_seconds']=time.monotonic()-start
    path.write_text(json.dumps(record,indent=2)+'\n')
    print(f"ROUND {number:02} exit={record['exit_code']} seconds={record['elapsed_seconds']:.1f}",flush=True)
    if record['exit_code']:raise SystemExit(record['exit_code'])
