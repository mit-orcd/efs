"""After capture, verify command tags against the same retained WAL inode."""
import json,os
from pathlib import Path
base=Path('/data1/efs/durability-tracepoints-20261009')
identity=json.loads((base/'identity.json').read_text())
headers=json.loads((base/'wal-headers.json').read_text())
for n,s in identity['servers'].items():
    fd=os.open(s['wal'],os.O_RDONLY)
    try:
        assert os.fstat(fd).st_ino==s['inode'], 'WAL rotated: refuse command attribution'
        for off,old in list(headers[n].items()):
            data=os.pread(fd,35,int(off))
            assert data[:13].hex()==old, 'record header changed'
            # Do not read across a short record's boundary.
            length=int.from_bytes(data[4:8],'big')+8
            headers[n][off]=data[:min(length,35)].hex()
    finally:os.close(fd)
(base/'wal-headers.json').write_text(json.dumps(headers))
