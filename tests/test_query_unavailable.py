#!/usr/bin/env python3
"""Unknown/legacy replies cannot turn unavailable query stats into fake totals."""
import ctypes as c
from pathlib import Path
import socket
import struct
import subprocess
import threading
root=Path(__file__).resolve().parents[1]
class User(c.Structure):
    _fields_=[('uid',c.c_uint32),('files',c.c_uint64),('bytes',c.c_uint64)]
# Obtain the actual legacy ABI size from the header-defined compile-time constant.
import re
header=(root/'include/efs/protocol.h').read_text()
count=int(re.search(r'#define EFS_MAX_QUERY_USERS\s+(\d+)',header).group(1))
class Legacy(c.Structure):
    _fields_=[('files',c.c_uint64),('bytes',c.c_uint64),('count',c.c_uint32),('users',User*count)]
for body,code in ((bytes([1]),2),(bytes(Legacy()),2),(bytes([255]),1)):
    with socket.socket() as listener:
        listener.bind(('127.0.0.1',0));listener.listen()
        port=listener.getsockname()[1]
        def serve():
            conn,_=listener.accept()
            with conn:
                request=b''
                while len(request)<5:
                    request+=conn.recv(5-len(request))
                assert request[-1]==25,request
                reply=bytes([26])+body
                conn.sendall(struct.pack('>I',len(reply))+reply)
        worker=threading.Thread(target=serve);worker.start()
        result=subprocess.run([str(root/'efs-query'),'--raw',f'127.0.0.1:{port}'],capture_output=True,text=True,timeout=5)
        worker.join(5)
        assert result.returncode==code and not result.stdout,(result.stdout,result.stderr,result.returncode)
print('query: unsupported and legacy replies never print fake totals; malformed status rejected PASS')
