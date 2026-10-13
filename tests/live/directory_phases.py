#!/usr/bin/env python3
"""Observe existing POSIX deep-directory phase datagrams; diagnostic only."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('mount')
parser.add_argument('--jobs',type=int,default=1)
args=parser.parse_args()
if not 1<=args.jobs<=4:parser.error('--jobs must be 1..4')
source=Path(__file__).resolve().parents[1]/'posix/posix_suite.py'
rows=[]
with tempfile.TemporaryDirectory(prefix='efs-directory-phases-') as directory:
    sock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
    target=str(Path(directory)/'phases.sock')
    sock.bind(target);sock.settimeout(0.1)
    stopped=threading.Event()
    def receive():
        while not stopped.is_set():
            try:payload=sock.recv(2048).decode()
            except socket.timeout:continue
            rows.append(dict(item.split('=',1) for item in payload.split()))
    thread=threading.Thread(target=receive);thread.start()
    try:
        env=dict(os.environ,EFS_POSIX_PHASE_SOCKET=target,EFS_POSIX_PHASE_CLIENT='mac-directory-diagnostic')
        result=subprocess.run(['python3',str(source),args.mount,'--filter','dir_deep_nesting',
                               '--jobs',str(args.jobs),'--timeout-s','20'],env=env)
    finally:
        stopped.set();thread.join();sock.close()
    workers={}
    for row in rows:
        pid=row['worker'];previous=workers.get(pid)
        if previous:
            row['since_previous_ms']=(int(row['mono_ns'])-int(previous['mono_ns']))/1e6
        workers[pid]=row
    print(json.dumps({'diagnostic':True,'jobs':args.jobs,'timeout_s':20,
                      'suite_returncode':result.returncode,'phases':rows},indent=2))
    raise SystemExit(result.returncode)
