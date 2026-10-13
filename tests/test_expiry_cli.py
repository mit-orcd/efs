#!/usr/bin/env python3
"""Named expiry durations must serialize identically to the legacy policy form."""
import socket
import struct
import subprocess
import sys
import threading
from pathlib import Path
from test_gc_status_expiry import exact

binary=Path(sys.argv[1] if len(sys.argv)>1 else './efs-mgmt').resolve()
def request(args):
    received=[];errors=[]
    with socket.socket() as listener:
        listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(10)
        def serve():
            try:
                with listener.accept()[0] as conn:
                    conn.settimeout(10);n=struct.unpack('>I',exact(conn,4))[0]
                    received.append(exact(conn,n))
                    reply=struct.pack('=iiQQQ',0,-1,2,86400_000_000_000,604800_000_000_000)
                    conn.sendall(struct.pack('>IB',len(reply)+1,96)+reply)
            except BaseException as e:errors.append(e)
        thread=threading.Thread(target=serve);thread.start()
        result=subprocess.run([str(binary),'expiry-policy',f'127.0.0.1:{listener.getsockname()[1]}','set','1','1','0',*args],capture_output=True,text=True,timeout=15)
        thread.join(15);assert not thread.is_alive() and not errors and result.returncode==0,(result,errors)
    return received[0]
help_result=subprocess.run([str(binary),'expiry-policy','--help'],capture_output=True,text=True,timeout=5)
assert help_result.returncode==0 and all(text in help_result.stdout for text in
    ('--expire-after','--renewal-limit','ns, s, m, h or d','does not renew','future files','not a maximum lifetime'))
legacy=request(['86400000000000','604800000000000','3'])
assert request(['--expire-after','24h','--renewal-limit','7d','--flags','3'])==legacy
assert request(['--flags','3','--renewal-limit','10080m','--expire-after','86400s'])==legacy
assert request(['--expire-after','1ns','--flags','0'])==request(['1','1','0'])
for args in (['--expire-after','0h','--flags','3'],['--expire-after','1.5h','--flags','3'],
             ['--expire-after','24','--flags','3'],['--expire-after','1w','--flags','3'],
             ['--expire-after','18446744073709551615d','--flags','3'],
             ['--expire-after','7d','--renewal-limit','24h','--flags','3'],
             ['--expire-after','24h','--expire-after','1h','--flags','3'],
             ['--renewal-limit','7d','--flags','3'],['--expire-after','1h'],
             ['--expire-after','1h','--flags','4']):
    result=subprocess.run([str(binary),'expiry-policy','127.0.0.1:1','set','1','1','0',*args],capture_output=True,text=True,timeout=5)
    assert result.returncode!=0 and '--expire-after' in result.stderr,(args,result)
print('Expiry duration serialization, defaults, legacy compatibility and rejection PASS')
