"""W92 v1 raw TCP query client for private-fixture acceptance.

ctypes follows the existing native-struct protocol ABI, including alignment.
A size gate prevents silently changing this wire contract.
"""
import ctypes as C
import socket
import os
import struct
class Request(C.Structure):
    _fields_=[('version',C.c_uint32),('operation',C.c_uint32),('root',C.c_uint64),
              ('uid',C.c_uint32),('gid',C.c_uint32),('ngroups',C.c_uint32),('match',C.c_uint32),
              ('max_output',C.c_uint32),('visit_budget',C.c_uint32),('groups',C.c_uint32*128),('term',C.c_char*256)]
class Filters(C.Structure):
    _fields_=[('flags',C.c_uint32),('kind',C.c_uint32),('min_size',C.c_uint64),
              ('max_size',C.c_uint64),('after_ns',C.c_uint64),('before_ns',C.c_uint64)]
class FilterRequest(C.Structure):
    _anonymous_=('base',)
    _fields_=[('base',Request),('filters',Filters)]
assert C.sizeof(FilterRequest)==848,C.sizeof(FilterRequest)
def filtered_request(**fields):
    selected={key:fields.pop(key) for key,_ in Filters._fields_ if key in fields}
    fields.setdefault('version',3);fields.setdefault('operation',1)
    return FilterRequest(base=request(**fields),filters=Filters(**selected))
class PageRequest(C.Structure):
    _anonymous_ = ('base',)
    _fields_ = [('base', Request), ('filters', Filters), ('token', C.c_uint64),
                ('cursor', C.c_uint64), ('limit', C.c_uint32), ('nonce', C.c_uint32)]
def page_request(**fields):
    extras = {key: fields.pop(key) for key in ('token', 'cursor', 'limit', 'nonce') if key in fields}
    fields.setdefault('version', 4);fields.setdefault('operation', 1)
    fields.setdefault('match', 3);fields.setdefault('visit_budget', 100000)
    fields.setdefault('max_output', 1 << 20);extras.setdefault('limit', 100);extras.setdefault('nonce', 1)
    return PageRequest(base=request(**fields), **extras)
class Reply(C.Structure):
    _fields_=[('version',C.c_uint32),('status',C.c_int32),('refresh_error',C.c_int32),
              ('primary_id',C.c_uint16),('reserved',C.c_uint16),('text_len',C.c_uint32),
              ('uid',C.c_uint32),('gid',C.c_uint32),('checked_dirs',C.c_uint32),
              ('boot_id',C.c_uint64),('generation',C.c_uint64),('source_ns',C.c_uint64),('built_ns',C.c_uint64),
              ('applied',C.c_uint64*2),('terms',C.c_uint64*2),('mtime',C.c_uint64),('atime',C.c_uint64),('ctime',C.c_uint64)]
assert C.sizeof(Request)==808 and C.sizeof(Reply)==120,(C.sizeof(Request),C.sizeof(Reply))
def request(**fields):
    q=Request(version=1,root=1,uid=1000,gid=1000,max_output=2048,visit_budget=1000000)
    for k,v in fields.items():setattr(q,k,v)
    return q
def receive(s,n):
    data=bytearray()
    while len(data)<n:
        chunk=s.recv(n-len(data))
        if not chunk:raise EOFError('short query frame')
        data.extend(chunk)
    return bytes(data)
def query(port,q,raw=None):
    body=bytes(q) if raw is None else raw
    with socket.create_connection((os.environ.get('EFS_QUERY_TEST_HOST','127.0.0.1'),port),7) as s:
        s.sendall(struct.pack('!I',len(body)+1)+bytes([123])+body)
        n=struct.unpack('!I',receive(s,4))[0]
        assert 121<=n<=120+(16<<20)+1,n
        payload=receive(s,n)
    assert payload[0]==124,payload[0]
    r=Reply.from_buffer_copy(payload[1:121]);text=payload[121:]
    assert r.version==(q.version if raw is None and q.version in (1,2,3,4) else 1) and r.text_len==len(text),(r.version,r.text_len,len(text))
    assert not r.status or not text,'partial failure delivered bytes'
    return r,text

def features(port):
    body=b'w92-private'.ljust(256,b'\0')
    with socket.create_connection((os.environ.get('EFS_QUERY_TEST_HOST','127.0.0.1'),port),7) as s:
        s.sendall(struct.pack('!I',len(body)+1)+bytes([39])+body)
        n=struct.unpack('!I',receive(s,4))[0]
        payload=receive(s,n)
    assert len(payload)==9 and payload[0]==40,payload
    mask=struct.unpack('@I',payload[1:5])[0]
    assert payload[5]==0,payload
    return mask
