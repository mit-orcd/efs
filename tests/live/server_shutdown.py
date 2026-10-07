#!/usr/bin/env python3
"""Owned Linux daemon teardown gate: pooled readers and partial TCP frames."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

source=Path(__file__).resolve().parents[2]
for direct in (False,True):
    for repeat in range(3):
        with tempfile.TemporaryDirectory(prefix='efs-shutdown-') as work:
            with socket.socket() as free:
                free.bind(('127.0.0.1',0));port=free.getsockname()[1]
            logpath=Path(work)/'daemon.log'
            sockets=[];stop=threading.Event()
            with logpath.open('w') as log:
                proc=subprocess.Popen([str(source/'efsd'),'--node-id','1','--addr','127.0.0.1','--port',str(port),'--storage',work,'--direct-io' if direct else '--no-direct-io'],env=dict(os.environ,EFS_MD_RAFT_N='3',EFS_TRANSPORT='tcp'),stdout=log,stderr=log)
                def churn():
                    while not stop.is_set():
                        try:
                            with socket.create_connection(('127.0.0.1',port),.1):pass
                        except OSError:pass
                worker=threading.Thread(target=churn)
                try:
                    deadline=time.monotonic()+10
                    while True:
                        assert proc.poll() is None,'daemon exited during startup'
                        try:
                            sockets.append(socket.create_connection(('127.0.0.1',port),.1));break
                        except OSError:
                            assert time.monotonic()<deadline,'startup timeout';time.sleep(.05)
                    for index in range(48):
                        sock=socket.create_connection(('127.0.0.1',port),1);sockets.append(sock)
                        if index%2:sock.sendall(b'\x00\x00') # incomplete length frame
                    worker.start();time.sleep(.15)
                    before=time.monotonic();proc.terminate();proc.wait(5)
                    elapsed=time.monotonic()-before
                    assert proc.returncode==0,proc.returncode
                    assert 'forcing teardown' not in logpath.read_text()
                    print(f'{direct=} {repeat=} idle/partial/churning TCP shutdown {elapsed:.3f}s PASS',flush=True)
                finally:
                    stop.set()
                    if worker.is_alive():worker.join(2)
                    for sock in sockets:sock.close()
                    if proc.poll() is None:proc.kill();proc.wait()
