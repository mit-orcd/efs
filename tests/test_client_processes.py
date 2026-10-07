#!/usr/bin/env python3
"""Actual process discovery and Linux endpoint-owner retirement checks."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('clients',root/'scripts/client_processes.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory(prefix='efs-processes-') as directory:
    proc=Path(directory)
    p=proc/'123';p.mkdir();(p/'comm').write_text('efs-fuse\n')
    (p/'cmdline').write_bytes(b'./efs-fuse\0seed:17432\0fs\0/mnt/a/../efs\0-o\0allow_other\0')
    (p/'stat').write_text('123 (efs-fuse) '+' '.join(['S']+['0']*18+['99']))
    assert m.clients(proc)==[(123,'/mnt/efs','99')]
    (p/'cmdline').write_bytes(b'./efs-fuse\0--stop\0/mnt/efs\0')
    assert not m.clients(proc)
    (p/'cmdline').write_bytes(b'./efs-fuse\0seed\0')
    try: m.clients(proc)
    except RuntimeError: pass
    else: raise AssertionError('ambiguous client must not be silently skipped')
    # Deterministic exit between the first stat and cmdline reads.
    original_read = Path.read_bytes
    def exiting_cmdline(path):
        if path == p/'cmdline':
            (p/'stat').write_text('123 (efs-fuse) '+' '.join(['Z']+['0']*18+['99']))
            return b''
        return original_read(path)
    with patch.object(Path, 'read_bytes', exiting_cmdline):
        assert not m.clients(proc)
    (p/'comm').unlink();assert not m.clients(proc)
if sys.platform.startswith('linux'):
    # Isolated synthetic daemon; never touch an actual cluster/client mount.
    with tempfile.TemporaryDirectory(prefix='efs-process-owner-') as directory:
        mount=str(Path(directory)/'mount')
        value=14695981039346656037
        for byte in os.fsencode(mount): value=((value^byte)*1099511628211)&((1<<64)-1)
        endpoint=f'/tmp/efs-control-{value:016x}.sock'
        code='''import ctypes,os,socket,sys,time
ctypes.CDLL(None).prctl(15,b"efs-fuse",0,0,0)
s=socket.socket(socket.AF_UNIX);s.bind(sys.argv[2]);s.listen(4)
if len(sys.argv)>3:
 s.close();os.unlink(sys.argv[2])
print("READY",flush=True)
time.sleep(1 if len(sys.argv)>3 else 30)
'''
        for owned in (False,True,'closing'):
            bound=endpoint if owned else endpoint+'.other'
            process=subprocess.Popen([sys.executable,'-c',code,mount,bound,*(['closing'] if owned=='closing' else [])],stdout=subprocess.PIPE,text=True)
            try:
                assert process.stdout.readline().strip()=='READY'
                assert any(row[0]==process.pid and row[1]==mount for row in m.clients())
                if not owned:
                    try: m.retire(mount)
                    except RuntimeError: pass
                    else: raise AssertionError('unowned endpoint must not authorize retirement')
                    assert process.poll() is None
                else:
                    m.retire(mount)
                    assert process.wait(timeout=2)==(0 if owned=='closing' else -15)
            finally:
                if process.poll() is None: process.terminate();process.wait()
                try: os.unlink(bound)
                except FileNotFoundError: pass
print('client processes: detached discovery and ambiguous command refusal PASS')
