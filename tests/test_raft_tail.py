#!/usr/bin/env python3
"""Verify attribution filters uncommitted entries and obsolete truncated tails."""
import struct
import sys
import tempfile
import zlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'live'))
from raft_tail import commands

def record(kind,group,payload):
    body=struct.pack('>BI',kind,group)+payload
    return struct.pack('>II',zlib.crc32(body),len(body))+body

def entry(group,index,term,command):
    return record(2,group,struct.pack('>QQI',index,term,len(command))+command)
with tempfile.TemporaryDirectory(prefix='efs-tail-test-') as d:
    p=Path(d)/'raft.log';header=struct.pack('>II',0x52464c31,1)
    p.write_bytes(header+entry(0,1,1,b'A')+entry(0,2,1,b'B')+entry(2,1,2,b'C'))
    assert commands(p,{0:1,2:1})=={(0,1):(1,b'A'),(2,1):(2,b'C')}
    with p.open('ab') as out:
        out.write(record(3,0,struct.pack('>Q',2)))
        out.write(entry(0,2,3,b'D'))
        out.write(entry(0,3,3,b'E')[:-2])
    assert commands(p,{0:2,2:1})=={(0,1):(1,b'A'),(0,2):(3,b'D'),(2,1):(2,b'C')}
    body=bytearray(p.read_bytes());body[18]^=1;p.write_bytes(body)
    try:commands(p,{0:2})
    except ValueError:pass
    else:raise AssertionError('bad CRC was accepted')
print('Raft tail: CRC, committed bounds, groups, replacement and torn append PASS')
