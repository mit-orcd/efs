"""Read committed command identities from production CRC-framed Raft logs.

Diagnostic only: a torn uncommitted append tail is ignored; corruption is fatal.
TRUNC removes obsolete indexes so superseded proposals are never counted.
"""
import struct
import zlib
from pathlib import Path

def commands(path, commits):
    data=Path(path).read_bytes()
    if len(data)<8 or struct.unpack_from('>II',data)!=(0x52464c31,1):
        raise ValueError('invalid Raft log header')
    off=8;entries={}
    while off+8<=len(data):
        crc,length=struct.unpack_from('>II',data,off)
        if not 0<length<=16*1024*1024:raise ValueError('invalid record length')
        if off+8+length>len(data):break
        payload=data[off+8:off+8+length]
        if zlib.crc32(payload)!=crc:raise ValueError('Raft record checksum mismatch')
        if len(payload)<5:raise ValueError('short Raft record')
        kind=payload[0];group=struct.unpack_from('>I',payload,1)[0]
        if kind==2:
            if len(payload)<25:raise ValueError('short entry')
            index,term,size=struct.unpack_from('>QQI',payload,5)
            if len(payload)!=25+size:raise ValueError('invalid command size')
            entries[group,index]=(term,payload[25:])
        elif kind==3:
            index=struct.unpack_from('>Q',payload,5)[0]
            entries={key:value for key,value in entries.items() if key[0]!=group or key[1]<index}
        elif kind not in (1,4,5):raise ValueError('unknown record kind')
        off+=8+length
    return {key:value for key,value in entries.items() if key[1]<=commits.get(key[0],0)}
