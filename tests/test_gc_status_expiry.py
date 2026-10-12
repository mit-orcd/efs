#!/usr/bin/env python3
"""Management status wire compatibility, including unknown expiry on old nodes."""
import socket
import struct
import subprocess
import sys
import threading
from pathlib import Path


def exact(s,n):
    data=b''
    while len(data)<n:
        part=s.recv(n-len(data))
        assert part,'short request'
        data+=part
    return data


def probe(binary,command,version,payload_size=None):
    base=struct.pack('=8I25Q',version,1,0,0,0,1,0,0,*([0]*25))
    io=struct.pack('=12Q',*([0]*12))
    expiry=struct.pack('=6I12Q2I',1,1,1,0,1,1,
                       17,16384,2,3,4,5,1000,2000,64,0,3000,0,1,0)
    payload=base+(io if version>=2 else b'')+(expiry if version>=3 else b'')
    if payload_size is not None:payload=payload[:payload_size]
    errors=[]
    with socket.socket() as listener:
        listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(10)
        def serve():
            try:
                with listener.accept()[0] as conn:
                    conn.settimeout(10)
                    n=struct.unpack('>I',exact(conn,4))[0]
                    assert exact(conn,n)==bytes((119,))
                    conn.sendall(struct.pack('>IB',len(payload)+1,120)+payload)
            except BaseException as error:errors.append(error)
        thread=threading.Thread(target=serve);thread.start()
        result=subprocess.run([str(binary),command,f'127.0.0.1:{listener.getsockname()[1]}'],capture_output=True,text=True,timeout=15)
        thread.join(timeout=15);assert not thread.is_alive() and not errors,errors
    return result


def main():
    binary=Path(sys.argv[1] if len(sys.argv)>1 else './efs-mgmt').resolve()
    for version in (1,2):
        result=probe(binary,'gc-status',version)
        assert result.returncode==0,result
        assert 'expired_files=' not in result.stdout,'old status invented expiry counters'
        result=probe(binary,'expiry-status',version)
        assert result.returncode!=0 and 'unavailable' in result.stderr,result
    result=probe(binary,'gc-status',3)
    assert result.returncode==0 and 'expired_files=17' in result.stdout and 'expired_size_unknown_files=2' in result.stdout,result
    result=probe(binary,'expiry-status',3)
    assert result.returncode==0 and '17 files' in result.stdout and 'at least 64' in result.stdout,result
    assert '2 seconds ago' in result.stdout and 'size_unknown=2' in result.stdout,result
    for version,size in ((3,328),(2,232),(1,231),(4,None)):
        assert probe(binary,'gc-status',version,size).returncode!=0,(version,size)
    print('GC status v1/v2 compatibility, v3 expiry fields and malformed replies PASS')


if __name__=='__main__':main()
