"""W92 raw TCP query client for private-fixture acceptance.

Unpaged requests (version 1) carry only the .stats aggregate; finds, listings
and filters use version 4 pages. Versions 2 and 3 were removed and are refused.

ctypes follows the existing native-struct protocol ABI, including alignment.
A size gate prevents silently changing this wire contract.
"""
import ctypes as C
import socket
import os
import struct
import json
import time
class Request(C.Structure):
    _fields_=[('version',C.c_uint32),('operation',C.c_uint32),('root',C.c_uint64),
              ('uid',C.c_uint32),('gid',C.c_uint32),('ngroups',C.c_uint32),('match',C.c_uint32),
              ('max_output',C.c_uint32),('visit_budget',C.c_uint32),('groups',C.c_uint32*128),('term',C.c_char*256)]
class Filters(C.Structure):
    _fields_=[('flags',C.c_uint32),('kind',C.c_uint32),('min_size',C.c_uint64),
              ('max_size',C.c_uint64),('after_ns',C.c_uint64),('before_ns',C.c_uint64)]
def filtered_request(**fields):
    """A filtered find is a first page (version 4) carrying the filters."""
    fields.setdefault('operation',1);fields.setdefault('limit',10000)
    return page_request(**fields)
class PageRequest(C.Structure):
    _anonymous_ = ('base',)
    _fields_ = [('base', Request), ('filters', Filters), ('token', C.c_uint64),
                ('cursor', C.c_uint64), ('limit', C.c_uint32), ('nonce', C.c_uint32)]
def page_request(**fields):
    extras = {key: fields.pop(key) for key in ('token', 'cursor', 'limit', 'nonce') if key in fields}
    selected = {key: fields.pop(key) for key, _ in Filters._fields_ if key in fields}
    fields.setdefault('version', 4);fields.setdefault('operation', 1)
    fields.setdefault('match', 3);fields.setdefault('visit_budget', 100000)
    fields.setdefault('max_output', 1 << 20);extras.setdefault('limit', 100);extras.setdefault('nonce', 1)
    return PageRequest(base=request(**fields), filters=Filters(**selected), **extras)
def page_query(port, **fields):
    """One first page, then close its retained generation (limit 0)."""
    if fields.get('operation', 1) == 0:
        fields.setdefault('limit', 1)
    reply, text = query(port, page_request(**dict(fields)))
    if not reply.status and text:
        page = json.loads(text).get('page') or {}
        if int(page.get('token', '0')):
            close = {k: v for k, v in fields.items() if k not in ('limit', 'token', 'cursor')}
            query(port, page_request(**close, token=int(page['token']), cursor=int(page['cursor']), limit=0))
    return reply, text
def _page_name():
    import sys
    from pathlib import Path
    tools = str(Path(__file__).resolve().parents[2] / 'src/client/commands')
    if tools not in sys.path:
        sys.path.insert(0, tools)
    from command import page_name
    return page_name
def query_name(command, term=b'', match='contains', filters=None, limit=None, nonce=None):
    """A .efs/.query file name (v3. first page) as the efs command tool builds it.
    The page's server session stays retained until closed (close_query) or
    swept when idle; use it for descriptors a test deliberately holds."""
    limit = (1 if command == 'stats' else 10000) if limit is None else limit
    nonce = int.from_bytes(os.urandom(4), 'big') if nonce is None else nonce
    return _page_name()(command, term, match, filters, limit, 0, 0, nonce)
def close_query(directory, command, data, term=b'', match='contains', filters=None, nonce=0):
    """Release the session of a first page read from directory/.efs/.query."""
    try:
        page = json.loads(data).get('page') or {}
    except ValueError:
        return
    if int(page.get('token', '0')):
        name = _page_name()(command, term, match, filters, 0, int(page['token']), int(page['cursor']), nonce)
        try:
            with open(os.path.join(directory, '.efs/.query', name), 'rb') as stream:
                stream.read(1)
        except OSError:
            pass  # the idle lease also releases it
def read_query(directory, command, term=b'', match='contains', filters=None):
    """Read one first page through the mount, then close its session."""
    nonce = int.from_bytes(os.urandom(4), 'big')
    path = os.path.join(directory, '.efs/.query', query_name(command, term, match, filters, nonce=nonce))
    with open(path, 'rb') as stream:
        data = stream.read()
    close_query(directory, command, data, term, match, filters, nonce)
    return data
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
HELLO=struct.pack('>IB',17,165)+b'EFSP'+struct.pack('=IQ',5,0)  # CLIENT_HELLO, EFS_PROTOCOL_VERSION 5 (K13)
def declare(sock):
    sock.sendall(HELLO);n=struct.unpack('>I',receive(sock,4))[0];reply=receive(sock,n)
    assert n==9 and reply[0]==166 and struct.unpack('=iI',reply[1:])==(0,5),('declaration refused',reply)
def query(port,q,raw=None,*,host=None,timeout=7):
    body=bytes(q) if raw is None else raw
    with socket.create_connection((host or os.environ.get('EFS_QUERY_TEST_HOST','127.0.0.1'),port),timeout) as s:
        declare(s);s.sendall(struct.pack('!I',len(body)+1)+bytes([123])+body)
        n=struct.unpack('!I',receive(s,4))[0]
        assert 121<=n<=120+(16<<20)+1,n
        payload=receive(s,n)
    assert payload[0]==124,payload[0]
    r=Reply.from_buffer_copy(payload[1:121]);text=payload[121:]
    # Refusals of unknown or removed versions (2, 3) answer as version 1.
    assert r.version==(q.version if raw is None and q.version in (1,4) else 1) and r.text_len==len(text),(r.version,r.text_len,len(text))
    assert not r.status or not text,'partial failure delivered bytes'
    return r,text

def features(port):
    body=b'w92-private'.ljust(256,b'\0')
    with socket.create_connection((os.environ.get('EFS_QUERY_TEST_HOST','127.0.0.1'),port),7) as s:
        declare(s);s.sendall(struct.pack('!I',len(body)+1)+bytes([39])+body)
        n=struct.unpack('!I',receive(s,4))[0]
        payload=receive(s,n)
    assert len(payload)==9 and payload[0]==40,payload
    mask=struct.unpack('@I',payload[1:5])[0]
    assert payload[5]==0,payload
    return mask


def routed_query(port, q, *, deadline=None):
    """Follow node ownership in local-port or explicit multi-host fixtures.

    EFS_QUERY_TEST_NODES is JSON mapping node IDs to [host, port]. Retained
    page tokens are node-owned; sending them to the initial seed is invalid.
    Default consecutive ports preserve the four-node localhost test fixture.
    """
    supplied = os.environ.get('EFS_QUERY_TEST_NODES')
    nodes = json.loads(supplied) if supplied else None
    seed = (os.environ.get('EFS_QUERY_TEST_HOST', '127.0.0.1'), port)
    def endpoint(owner):
        if nodes is None:
            assert 1 <= owner <= 4, ('unknown query owner', owner)
            return seed[0], int(os.environ['EFS_QUERY_TEST_PORT']) + owner - 1
        assert str(owner) in nodes, ('unknown query owner', owner)
        host, target_port = nodes[str(owner)]
        assert isinstance(host, str) and host and isinstance(target_port, int)
        assert 1 <= target_port <= 65535, target_port
        return host, target_port
    def send(target):
        timeout = 7 if deadline is None else min(7, deadline - time.monotonic())
        if timeout <= 0:
            raise TimeoutError('query retry window expired')
        return query(target[1], q, host=target[0], timeout=timeout)
    token = getattr(q, 'token', 0)
    if token:
        host, target_port = endpoint(token >> 56)
        return send((host, target_port))
    seen = set()
    target = seed
    for _ in range(len(nodes) if nodes is not None else 4):
        assert target not in seen, ('query redirect loop', target, seen)
        seen.add(target)
        reply, text = send(target)
        if reply.status != -15:
            return reply, text
        target = endpoint(reply.primary_id)
    raise AssertionError('query redirect bound exceeded')


def accepted_query(port, q):
    """Positive page oracles allow BUSY inside the helper's existing 8s window.

    Negative budget/permission/status probes use routed_query directly. Hard
    statuses are returned unchanged; redirects and socket reads share one bound.
    """
    deadline = time.monotonic() + 8
    attempt = 0
    while True:
        reply, text = routed_query(port, q, deadline=deadline)
        if reply.status != -13:
            return reply, text
        assert not text, 'busy refusal delivered partial bytes'
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return reply, text
        attempt += 1
        time.sleep(min(.025 * attempt, .25, remaining))
