#!/usr/bin/env python3
"""efs-mgmt reads only the current GC status reply (version 4, exact length)."""
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


def probe(binary,command,version,payload_size=None,healthy=1,uncertain=0):
    base=struct.pack('=8I25Q',version,1,0,0,0,1,0,0,*([0]*25))
    io=struct.pack('=12Q',*([0]*12))
    expiry=struct.pack('=6I12Q2I',1,1,healthy,uncertain,1,1,
                       17,16384,2,3,4,5,1000,2000,64,0,3000,0,1,0)
    reclaim=struct.pack('=12Q2IQ',*([0]*12),0,0,0)
    payload=base+io+expiry+reclaim
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
    result=probe(binary,'gc-status',4)
    assert result.returncode==0 and 'expired_files=17' in result.stdout and 'expired_size_unknown_files=2' in result.stdout,result
    assert 'reclaim_state=ok' in result.stdout and 'gc_io=dir_sync' in result.stdout,result
    result=probe(binary,'expiry-status',4)
    assert result.returncode==0 and '17 files' in result.stdout and 'at least 64' in result.stdout,result
    assert '2 seconds ago' in result.stdout and 'size_unknown=2' in result.stdout,result
    result=probe(binary,'expiry-status',4,healthy=0)
    assert result.returncode==0 and 'waiting: clock health sample' in result.stdout and 'operator reset required' not in result.stdout,result
    result=probe(binary,'expiry-status',4,healthy=0,uncertain=1)
    assert result.returncode==0 and 'paused: clock uncertain' in result.stdout and 'operator reset required' in result.stdout,result
    # Older layouts (versions 1-3, shorter replies) and wrong lengths are refused.
    for version,size in ((1,231),(2,328),(3,456),(4,456),(4,567),(3,None),(5,None)):
        result=probe(binary,'gc-status',version,size)
        assert result.returncode!=0 and 'unavailable' in result.stderr,(version,size,result)
    print('GC status: current v4 reply with expiry and reclaim fields; older layouts and malformed replies refused PASS')


if __name__=='__main__':main()
