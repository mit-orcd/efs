#!/usr/bin/env python3
"""Independent read-only decoder: exact retained W96 key order, CRC and bytes."""
import hashlib,json,math,struct,sys,zlib
from pathlib import Path
base=Path(sys.argv[1]);results=[]
for metadata in sorted((base/'w96').glob('*-batch*-*.json')):
    row=json.loads(metadata.read_text());batch=row['batch'];records=row['records']
    name=metadata.stem
    root=base/'w96-data' if name.startswith('sata-') else Path('/home/efs/devel-nuc-w96-20261010')
    path=root/(name+'.wal');data=path.read_bytes();offset=0
    record_bytes=12+145*batch;period=math.ceil(8*1024*1024/record_bytes)
    assert row['resets']==records//period,(name,row)
    expected=list(range(row['resets']*period,records))+[records-1]
    for record in expected:
        crc,length=struct.unpack_from('<II',data,offset);offset+=8
        body=data[offset:offset+length];offset+=length
        assert len(body)==length and zlib.crc32(body)==crc,(name,record,'CRC')
        assert struct.unpack_from('<I',body)[0]==batch
        i=4
        for slot in range(batch):
            op,klen,vlen=struct.unpack_from('<BII',body,i);i+=9
            assert op==1 and klen==8 and vlen==128
            key=struct.unpack_from('<Q',body,i)[0];i+=8
            assert key==record*16+slot,(name,record,slot,key)
            assert body[i:i+128]==bytes((key+j)%256 for j in range(128));i+=128
        assert i==length
    assert offset==len(data),(name,'unexpected trailing bytes',offset,len(data))
    results.append(dict(cell=name,exact_records=len(expected),exact_items=len(expected)*batch,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),read_only=True))
assert len(results)==16
(base/'w96/exact-replay.json').write_text(json.dumps(results,indent=2))
print('W96 exact ordered retained replay:',len(results),'cells PASS')
