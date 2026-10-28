#!/usr/bin/env python3
"""Host-side helpers of the Stage 2 gates a13-node-addition, c5-protocol-live,
d1-packing, d3-efs-load and e4-scale (scripts/qualify.py runs them on a
mount host, the management host or a node host from the candidate tree):

    stage2_helpers.py load --dir D --stop FILE --seed S [--threads 4] [--kib 64]
                           [--max-s 3600] [--deadline-s 30]
    stage2_helpers.py wire --node HOST:PORT ... [--timeout-s 5]
    stage2_helpers.py refuse --src TREE --scratch DIR
    stage2_helpers.py c5-prepare --dir D
    stage2_helpers.py c5-mutate --dir D --expect eio|ok
    stage2_helpers.py smallfiles write --dir D --files N --seed S [--max-size 8192]
    stage2_helpers.py smallfiles rows --mgmt EFS_MGMT --node EP ...   (manifest on stdin)
    stage2_helpers.py smallfiles mutate --dir D --seed S               (manifest on stdin)
    stage2_helpers.py d3 --dir D --seed S [--peer DIR] [--load-s 300] ...
    stage2_helpers.py e4 write --dir D --seed S --small N --large N --large-mib M
    stage2_helpers.py e4 queries --dir D --stop FILE
    stage2_helpers.py e4 delete --dir D                                (manifest on stdin)

Every command prints one JSON document as its last output line and exits
0 when its own checks hold, 1 otherwise (2: usage). The manifests use the
format of `qualify.py _manifest verify` (dir, files[name, size, sha256,
mode, mtime_ns], errors), which reads them back exactly. Payloads are
deterministic from (seed, name), as in qualify.py.

Nothing here formats, stops or mounts anything: the driver does that. The
refusal probe starts binaries with removed settings against a scratch
store that must stay absent (the refusal comes before any store access)
and an endpoint (127.0.0.1:1) no daemon can bind.
"""
import argparse, concurrent.futures, errno, hashlib, json, os, random, re, socket, struct
import signal, subprocess, sys, tempfile, threading, time
from pathlib import Path

PROTOCOL = 5
MSG_HEARTBEAT, MSG_HEARTBEAT_ACK = 0, 1
MSG_RAFT_MKFS, MSG_RAFT_MKFS_REPLY = 95, 96
MSG_CLIENT_HELLO, MSG_CLIENT_HELLO_REPLY = 165, 166
ERR_PROTO, ERR_NOT_PRIMARY, ERR_UNSUPPORTED = -7, -15, -24
UNKNOWN_TAG = 254      # no EFS_MD_CMD/REQ value (include/efs/meta_cmd.h)
RESERVED_TAG = 46      # EFS_MD_CMD_RESERVED_MIN..MAX
GROUPS = (0, 2)


def now():
    return time.strftime('%Y-%m-%dT%H:%M:%S', time.gmtime()) + f'.{int(time.time() * 1000) % 1000:03d}Z'


def payload(seed, name, size):
    """Same bytes as qualify.py payload()."""
    return random.Random(f'{seed}/{name}').randbytes(size)


def emit(doc, ok):
    print(json.dumps(doc, sort_keys=True))
    return 0 if ok else 1


def write_exact(path, data, flags=os.O_WRONLY | os.O_CREAT | os.O_EXCL, sync=True):
    fd = os.open(path, flags, 0o644)
    try:
        view = memoryview(data)
        while view:
            view = view[os.write(fd, view):]
        if sync:
            os.fsync(fd)
    finally:
        os.close(fd)


def verify_payload(path, expected):
    """One immediate exact read; mismatch evidence is captured before unwinding.

    No retry or any-version acceptance: a later matching read cannot turn this
    operation into success. Keep hashes/lengths rather than bulk payloads.
    """
    observed = Path(path).read_bytes()
    if observed != expected:
        detail = dict(path=str(path), expected_len=len(expected), observed_len=len(observed),
                      expected_sha256=hashlib.sha256(expected).hexdigest(),
                      observed_sha256=hashlib.sha256(observed).hexdigest(),
                      first_mismatch=next((i for i, (a, b) in enumerate(zip(expected, observed))
                                           if a != b), min(len(expected), len(observed))))
        exc = OSError(errno.EIO, 'read-back mismatch')
        exc.payload_detail = detail
        raise exc


def entry(d, name, data):
    st = os.stat(d / name)
    return dict(name=name, size=len(data), sha256=hashlib.sha256(data).hexdigest(),
                mode=st.st_mode & 0o7777, mtime_ns=st.st_mtime_ns)


def pct(values, p):
    v = sorted(values)
    return round(v[min(len(v) - 1, int((len(v) - 1) * p))], 4) if v else 0


# ---- load: writers until a stop file appears (A13 joins under load) --------

def h_load(a):
    d = Path(a.dir)
    d.mkdir(parents=True, exist_ok=False)
    lock = threading.Lock()
    files, errors, slow, ops = [], [], [], [0]
    last_ok, max_gap = [time.monotonic()], [0.0]
    end = time.monotonic() + a.max_s

    def writer(w):
        sub = d / f'w{w}'
        sub.mkdir()
        i = 0
        while time.monotonic() < end and not os.path.exists(a.stop):
            name = f'w{w}/f{i:06d}'
            data = payload(a.seed, name, a.kib * 1024 + (i % 7) * 313)
            t0 = time.monotonic()
            try:
                write_exact(d / name, data)
                verify_payload(d / name, data)
                e = entry(d, name, data)
                t1 = time.monotonic()
                with lock:
                    files.append(e)
                    max_gap[0] = max(max_gap[0], t1 - last_ok[0])
                    last_ok[0] = t1
                    if t1 - t0 > a.deadline_s:
                        slow.append(dict(name=name, seconds=round(t1 - t0, 3), at=now()))
            except OSError as exc:
                with lock:
                    errors.append(dict(name=name, errno=exc.errno, error=str(exc), at=now(),
                                       **getattr(exc, 'payload_detail', {})))
            with lock:
                ops[0] += 1
            i += 1

    threads = [threading.Thread(target=writer, args=(w,)) for w in range(a.threads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    max_gap[0] = max(max_gap[0], time.monotonic() - last_ok[0])
    doc = dict(dir=str(d), ops=ops[0], acknowledged=len(files), errors=errors[:50], nerrors=len(errors),
               over_deadline=slow[:50], nover=len(slow), deadline_s=a.deadline_s,
               max_gap_s=round(max_gap[0], 3), threads=a.threads, kib=a.kib,
               files=sorted(files, key=lambda f: f['name']),
               failed=sorted(e['name'] for e in errors))
    return emit(doc, not errors and not slow)


# ---- wire: declaration gate and fail-closed tags (W133, W140) -------------

def frame(t, body=b''):
    return struct.pack('>IB', 1 + len(body), t) + body


def recv_frame(s):
    """(type, body), 'closed' (EOF, no reply) or 'timeout'."""
    try:
        head = b''
        while len(head) < 5:
            chunk = s.recv(5 - len(head))
            if not chunk:
                return 'closed'
            head += chunk
        n, t = struct.unpack('>IB', head)
        body = b''
        while len(body) < n - 1:
            chunk = s.recv(n - 1 - len(body))
            if not chunk:
                return 'closed'
            body += chunk
        return t, body
    except socket.timeout:
        return 'timeout'
    except ConnectionResetError:
        return 'closed'


def hello(version, magic=b'EFSP'):
    return magic + struct.pack('<IQ', version, 0)


def connect(ep, timeout):
    host, port = ep.rsplit(':', 1)
    s = socket.create_connection((host, int(port)), timeout=timeout)
    s.settimeout(timeout)
    return s


def declared(ep, timeout):
    s = connect(ep, timeout)
    s.sendall(frame(MSG_CLIENT_HELLO, hello(PROTOCOL)))
    r = recv_frame(s)
    if not isinstance(r, tuple) or r[0] != MSG_CLIENT_HELLO_REPLY or struct.unpack('<iI', r[1][:8])[0] != 0:
        s.close()
        raise OSError(errno.EPROTO, f'declaration of protocol {PROTOCOL} not accepted: {r!r}')
    return s


def submit(ep, body, timeout):
    """One RAFT_MKFS submit on a declared connection: (rc, leader_hint) or the outcome string."""
    s = declared(ep, timeout)
    try:
        s.sendall(frame(MSG_RAFT_MKFS, body))
        r = recv_frame(s)
    finally:
        s.close()
    if not isinstance(r, tuple) or r[0] != MSG_RAFT_MKFS_REPLY or len(r[1]) < 8:
        return r if isinstance(r, str) else f'reply type {r[0]}'
    return struct.unpack('<ii', r[1][:8])


def classify(name, got, want):
    return dict(check=name, ok=got == want, got=repr(got), want=repr(want))


def wire_node(ep, nodes, timeout):
    """The declaration gate on one member, then the tag gate through it."""
    out = []
    s = connect(ep, timeout)
    s.sendall(frame(MSG_HEARTBEAT))
    r = recv_frame(s)
    out.append(classify('undeclared-heartbeat-served', r[0] if isinstance(r, tuple) else r, MSG_HEARTBEAT_ACK))
    s.sendall(frame(MSG_RAFT_MKFS, bytes([0, 1])))
    out.append(classify('undeclared-request-closed-without-reply', recv_frame(s), 'closed'))
    s.close()
    s = connect(ep, timeout)
    s.sendall(frame(MSG_CLIENT_HELLO, hello(PROTOCOL - 1)))
    r = recv_frame(s)
    got = (r[0], *struct.unpack('<iI', r[1][:8])) if isinstance(r, tuple) and len(r[1]) >= 8 else r
    out.append(classify('older-declaration-answered-proto', got, (MSG_CLIENT_HELLO_REPLY, ERR_PROTO, PROTOCOL)))
    out.append(classify('older-declaration-then-closed', recv_frame(s), 'closed'))
    s.close()
    s = connect(ep, timeout)
    s.sendall(frame(MSG_CLIENT_HELLO, hello(PROTOCOL, b'XXXX')))
    r = recv_frame(s)
    got = struct.unpack('<i', r[1][:4])[0] if isinstance(r, tuple) else r
    out.append(classify('bad-magic-declaration-refused', got, ERR_PROTO))
    s.close()
    s = connect(ep, timeout)
    s.sendall(frame(MSG_CLIENT_HELLO, hello(PROTOCOL)))
    r = recv_frame(s)
    got = struct.unpack('<iI', r[1][:8]) if isinstance(r, tuple) and len(r[1]) >= 8 else r
    out.append(classify('current-declaration-accepted', got, (0, PROTOCOL)))
    s.sendall(frame(MSG_HEARTBEAT))
    r = recv_frame(s)
    out.append(classify('declared-heartbeat-served', r[0] if isinstance(r, tuple) else r, MSG_HEARTBEAT_ACK))
    s.close()
    for group in GROUPS:
        for name, tag in (('unknown', UNKNOWN_TAG), ('reserved', RESERVED_TAG)):
            # A follower names the leader (NOT_PRIMARY): follow the hint, bounded.
            target, hops, got = ep, [], None
            for _ in range(len(nodes) + 1):
                got = submit(target, bytes([group, tag]), timeout)
                hops.append(target)
                if isinstance(got, tuple) and got[0] == ERR_NOT_PRIMARY and 0 <= got[1] < len(nodes):
                    target = nodes[got[1]]
                    continue
                break
            rc = got[0] if isinstance(got, tuple) else got
            out.append(dict(check=f'{name}-tag-{tag}-group-{group}-refused', ok=rc == ERR_PROTO,
                            got=repr(got), want=repr(ERR_PROTO), via=hops))
    return out


def h_wire(a):
    checks, ok = [], True
    for ep in a.node:
        try:
            for c in wire_node(ep, a.node, a.timeout_s):
                checks.append(dict(c, node=ep))
        except OSError as exc:
            checks.append(dict(check='connect', node=ep, ok=False, got=str(exc)))
    ok = bool(checks) and all(c['ok'] for c in checks)
    return emit(dict(checks=checks, passed=sum(c['ok'] for c in checks), total=len(checks)), ok)


# ---- refuse: removed settings stop a launch (A6/A7/A10) --------------------

REFUSALS = (
    ('efsd', {'EFS_COMMANDS': '1'}, ['127.0.0.1:1', '{store}'], 'EFS_COMMANDS was removed'),
    ('efsd', {'EFS_MD_RAFT_N': '3'}, ['127.0.0.1:1', '{store}'], 'EFS_MD_RAFT_N was removed'),
    ('efsd', {'EFS_READ_VERIFY': '0'}, ['127.0.0.1:1', '{store}'], 'EFS_READ_VERIFY was removed'),
    ('efsd', {'EFS_METADATA_BALANCE': '1'}, ['127.0.0.1:1', '{store}'], 'EFS_METADATA_BALANCE was removed'),
    ('efsd', {'EFS_UNMOUNT_REVOKE': ''}, ['127.0.0.1:1', '{store}'], 'EFS_UNMOUNT_REVOKE was removed'),
    ('efsd', {}, ['127.0.0.1:1', '{store}', '--no-direct-io'], '--no-direct-io was removed'),
    ('efsd', {}, ['127.0.0.1:1', '{store}', '--direct-io'], '--direct-io was removed'),
    ('efsd', {}, ['127.0.0.1:1', '{store}', '--no-persist'], '--no-persist was removed'),
    ('efsd', {}, ['--node-id', '1', '127.0.0.1:1', '{store}'], '--node-id was removed'),
    ('efs-fuse', {'EFS_FUSE_EXPORT': '1'}, ['127.0.0.1:1', '{mount}', '-f'], 'EFS_FUSE_EXPORT was removed'),
    ('efs-fuse', {'EFS_UNMOUNT_REVOKE': '0'}, ['127.0.0.1:1', '{mount}', '-f'], 'EFS_UNMOUNT_REVOKE was removed'),
    ('efs-fuse', {'EFS_EXPIRY': '1'}, ['127.0.0.1:1', '{mount}', '-f'], 'EFS_EXPIRY was removed'),
)


def h_refuse(a):
    scratch = Path(a.scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    mount = scratch / 'never-mounted'
    mount.mkdir(exist_ok=True)
    checks = []
    for i, (prog, env, args, want) in enumerate(REFUSALS):
        store = scratch / f'store-{i}'
        argv = [str(Path(a.src).resolve() / prog)] + [x.format(store=store, mount=mount) for x in args]
        e = {k: v for k, v in os.environ.items() if not k.startswith('EFS_') or k == 'EFS_TRANSPORT'}
        e.update(env)
        t0 = time.monotonic()
        try:
            p = subprocess.run(argv, capture_output=True, text=True, timeout=30, env=e, stdin=subprocess.DEVNULL)
            rc, text = p.returncode, p.stdout + p.stderr
        except subprocess.TimeoutExpired as exc:
            rc, text = 124, f'still running after 30 s: {exc}'
        mounted = os.path.ismount(mount)
        ok = rc != 0 and want in text and not store.exists() and not mounted
        checks.append(dict(check=f'{prog} {" ".join(f"{k}={v}" for k, v in env.items())} {" ".join(args)}'.strip(),
                           ok=ok, rc=rc, named=want in text, store_untouched=not store.exists(),
                           seconds=round(time.monotonic() - t0, 3),
                           line=next((l for l in text.splitlines() if 'removed' in l), text.strip()[-200:])[:300]))
    return emit(dict(checks=checks, passed=sum(c['ok'] for c in checks), total=len(checks)),
                all(c['ok'] for c in checks))


# ---- c5: session fence and unmount revoke fixtures (K5/W134, W72) ----------

def h_c5_prepare(a):
    d = Path(a.dir)
    d.mkdir(parents=True, exist_ok=False)
    for name, text in (('f', b'before\n'), ('g', b'keep\n'), ('h', b'idle\n')):
        write_exact(d / name, text)
    return emit(dict(dir=str(d), files=sorted(os.listdir(d))), True)


MUTATIONS = (
    ('create', lambda d: os.close(os.open(d / 'new', os.O_CREAT | os.O_WRONLY, 0o644))),
    ('mkdir', lambda d: os.mkdir(d / 'dir')),
    ('rename', lambda d: os.rename(d / 'g', d / 'g2')),
    ('unlink', lambda d: os.unlink(d / 'h')),
    ('link', lambda d: os.link(d / 'f', d / 'f3')),
    ('chmod', lambda d: os.chmod(d / 'f', 0o600)),
    ('utime', lambda d: os.utime(d / 'f', (1, 1))),
    ('truncate', lambda d: os.truncate(d / 'f', 1)),
    ('setxattr', lambda d: os.setxattr(d / 'f', 'user.c5', b'x')),
)


def write_fsync(d):
    fd = os.open(d / 'f', os.O_WRONLY)
    try:
        os.write(fd, b'after-fence')
        os.fsync(fd)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass


def h_c5_mutate(a):
    """--expect eio: every mutation on a fenced mount fails EIO, metadata
    reads still answer. --expect ok: a peer mount mutates normally (in its
    own subdirectory) and sees the pre-fence state."""
    d = Path(a.dir)
    checks = []
    if a.expect == 'eio':
        for name, fn in MUTATIONS + (('write-fsync', write_fsync),):
            try:
                fn(d)
                checks.append(dict(check=f'fenced-{name}', ok=False, got='succeeded'))
            except OSError as exc:
                checks.append(dict(check=f'fenced-{name}', ok=exc.errno == errno.EIO,
                                   got=errno.errorcode.get(exc.errno, exc.errno)))
        try:
            names = sorted(os.listdir(d))
            size = os.stat(d / 'g').st_size
            checks.append(dict(check='fenced-metadata-reads', ok=names == ['f', 'g', 'h'] and size == 5,
                               got=f'names={names} size(g)={size}'))
        except OSError as exc:
            checks.append(dict(check='fenced-metadata-reads', ok=False, got=str(exc)))
    else:
        try:
            p = d / 'peer'
            p.mkdir()
            write_exact(p / 'x', b'peer-works\n')
            os.rename(p / 'x', p / 'y')
            os.chmod(p / 'y', 0o600)
            os.setxattr(p / 'y', 'user.c5', b'p')
            assert (p / 'y').read_bytes() == b'peer-works\n'
            assert (d / 'f').read_bytes() == b'before\n', (d / 'f').read_bytes()
            assert sorted(os.listdir(d)) == ['f', 'g', 'h', 'peer'], sorted(os.listdir(d))
            os.unlink(p / 'y')
            os.rmdir(p)
            checks.append(dict(check='peer-mutates-and-sees-pre-fence-state', ok=True,
                               got='create/fsync/rename/chmod/setxattr/unlink/rmdir'))
        except (OSError, AssertionError) as exc:
            checks.append(dict(check='peer-mutates-and-sees-pre-fence-state', ok=False, got=repr(exc)))
    return emit(dict(checks=checks), all(c['ok'] for c in checks))


# ---- smallfiles: D1 packing fixture, its rows and the D2 mutations ---------

def h_small_write(a):
    d = Path(a.dir)
    d.mkdir(parents=True, exist_ok=False)
    rng = random.Random(f'{a.seed}/sizes')
    files, errors = [], []
    for i in range(a.files):
        sub = f'd{i % 8}'
        (d / sub).mkdir(exist_ok=True)
        name = f'{sub}/s{i:05d}'
        size = 1 + rng.randrange(a.max_size)
        data = payload(a.seed, name, size)
        try:
            write_exact(d / name, data)
            files.append(dict(entry(d, name, data), ino=os.stat(d / name).st_ino))
        except OSError as exc:
            errors.append(dict(name=name, errno=exc.errno, error=str(exc)))
    for sub in sorted(p for p in d.iterdir() if p.is_dir()):
        fd = os.open(sub, os.O_RDONLY)
        os.fsync(fd)
        os.close(fd)
    return emit(dict(dir=str(d), seed=a.seed, files=files, errors=errors), not errors)


def parse_getchunks(text):
    """`efs-mgmt raft-getchunks` -> (status, rows); the shape b_checks.py parses."""
    m = re.search(r'raft-getchunks status=(\d+) primary=\d+ ino=\d+ count=(\d+)', text)
    if not m:
        return None, []
    rows = []
    for line in text.splitlines():
        r = re.match(r'^\s+ci=(\d+) nodes=(\d+),(\d+),(\d+) base_gen=(\d+) obj_gen=(\d+) ck0=\w+ spans=(\d+)', line)
        if r:
            rows.append(dict(ci=int(r.group(1)), nodes=[int(r.group(k)) for k in (2, 3, 4)],
                             obj_gen=int(r.group(6)), spans=[], packed=None))
            continue
        sp = re.match(r'^\s+span off=\d+ len=\d+ gen=(\d+) seq=\d+ nodes=(\d+),(\d+),(\d+)', line)
        if sp and rows:
            rows[-1]['spans'].append((int(sp.group(1)), tuple(int(sp.group(k)) for k in (2, 3, 4))))
            continue
        p = re.match(r'^\s+packed segment=(\d+) gen=(\d+) group=(\d+) offset=(\d+) len=(\d+)', line)
        if p and rows:
            rows[-1]['packed'] = dict(zip(('segment', 'gen', 'group', 'offset', 'len'), map(int, p.groups())))
    return int(m.group(1)), rows


def stored_objects(rows):
    """The stored 2+1 objects one file's rows reference: a packed row its
    segment, an unpacked row its base object (unless a stub), and every
    span (an overwrite overlays spans on a packed base until it is repacked)."""
    objs = set()
    for r in rows:
        if r['packed']:
            objs.add(('segment', r['packed']['segment'], r['packed']['gen']))
        elif r['obj_gen']:
            objs.add(('chunk', r['obj_gen'], tuple(r['nodes'])))
        for gen, nodes in r['spans']:
            objs.add(('span', gen, nodes))
    return objs


def file_rows(mgmt, nodes, ino):
    last = ''
    for ep in nodes:
        p = subprocess.run([mgmt, 'raft-getchunks', ep, str(ino), '0'], capture_output=True, text=True,
                           timeout=60, env=dict(os.environ, EFS_MGMT_CHUNKS='1000'))
        st, rows = parse_getchunks(p.stdout + p.stderr)
        if p.returncode == 0 and st == 0:
            return rows
        last = (p.stdout + p.stderr)[-300:]
    raise RuntimeError(f'raft-getchunks {ino}: no member answered: {last}')


def h_small_rows(a):
    """Which manifest files are packed, and the stored objects they need
    (inventory: one segment holds many files)."""
    doc = json.loads(sys.stdin.read())
    packed, clipped, unpacked, objects, segments, bad = 0, 0, [], set(), set(), []
    for f in doc['files']:
        try:
            rows = file_rows(a.mgmt, a.node, f['ino'])
        except RuntimeError as exc:
            bad.append(str(exc))
            continue
        objects |= stored_objects(rows)
        if f['size'] == 0 and not rows:
            packed += 1  # nothing stored
        elif rows and all(r['packed'] and not r['spans'] for r in rows):
            packed += 1
            for r in rows:
                segments.add((r['packed']['segment'], r['packed']['gen']))
                # A logical truncate (D25) clips reads at the file size; a
                # reference shorter than the size would lose bytes.
                if r['packed']['len'] < f['size']:
                    bad.append(f'{f["name"]}: packed len {r["packed"]["len"]} < size {f["size"]}')
                elif r['packed']['len'] > f['size']:
                    clipped += 1
        else:
            unpacked.append(f['name'])
    out = dict(files=len(doc['files']), packed=packed, clipped=clipped, unpacked=len(unpacked), unpacked_names=unpacked[:20],
               stored_objects=len(objects), segments=len(segments), bad=bad[:20], nbad=len(bad))
    return emit(out, not bad)


def h_small_mutate(a):
    """D2 on packed files: overwrite one in eight, truncate one in eight (to
    0 or to half), unlink one in eight; the rest stay. Prints the new
    manifest of what must read back (the unlinked names are gone)."""
    doc = json.loads(sys.stdin.read())
    d = Path(doc['dir'])
    keep, done, errors = [], dict(overwritten=0, truncated=0, unlinked=0, retained=0), []
    for i, f in enumerate(doc['files']):
        name = f['name']
        try:
            if i % 8 == 1:
                data = payload(doc['seed'], name + '#2', f['size'] + 17)
                write_exact(d / name, data, flags=os.O_WRONLY | os.O_TRUNC)
                keep.append(dict(entry(d, name, data), ino=f['ino']))
                done['overwritten'] += 1
            elif i % 8 == 3:
                size = 0 if i % 16 == 3 else f['size'] // 2
                os.truncate(d / name, size)
                data = payload(doc['seed'], name, f['size'])[:size]
                keep.append(dict(entry(d, name, data), ino=f['ino']))
                done['truncated'] += 1
            elif i % 8 == 5:
                os.unlink(d / name)
                done['unlinked'] += 1
            else:
                keep.append(f)
                done['retained'] += 1
        except OSError as exc:
            errors.append(dict(name=name, errno=exc.errno, error=str(exc)))
            keep.append(f)  # whatever happened, the verify says what is there
    os.sync()
    out = dict(dir=str(d), seed=doc['seed'], files=keep, errors=errors, mutations=done)
    return emit(out, not errors)


# ---- d3: .efs commands and concurrent queries under ordinary I/O ----------

def efs_cmd(where, cmd, *args, timeout=60):
    """Launch queries without forking the address space owning FUSE reads.

    W121/K10: fork during another thread's direct-I/O read can detach its
    destination page on affected kernels. Use posix_spawn explicitly, rather
    than relying on subprocess's platform/argument-dependent fast path. Query
    scripts locate their directory via __file__, not the launcher's cwd.
    Temporary output files avoid pipe backpressure and retain the same timeout,
    return-code and exact-answer checks. No read or query retry is added here.
    """
    return spawn_capture([sys.executable, str(Path(where).absolute() / '.efs' / cmd), *args], timeout)


def spawn_capture(argv, timeout):
    t0 = time.monotonic()
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        actions = [(os.POSIX_SPAWN_OPEN, 0, os.devnull, os.O_RDONLY, 0),
                   (os.POSIX_SPAWN_DUP2, out.fileno(), 1),
                   (os.POSIX_SPAWN_DUP2, err.fileno(), 2)]
        pid = os.posix_spawn(sys.executable, argv, os.environ, file_actions=actions,
                             setsigdef=(signal.SIGPIPE,))
        reaped = False
        try:
            while True:
                done, status = os.waitpid(pid, os.WNOHANG)
                if done:
                    reaped = True
                    rc = os.waitstatus_to_exitcode(status)
                    break
                left = timeout - (time.monotonic() - t0)
                if left <= 0:
                    os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)
                    reaped = True
                    return 124, b'', 'deadline', time.monotonic() - t0
                time.sleep(min(.01, left))
        finally:
            if not reaped:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.waitpid(pid, 0)
        out.seek(0)
        err.seek(0)
        return rc, out.read(), err.read().decode(errors='replace'), time.monotonic() - t0


def wait_indexed(where, timeout):
    """`.efs/stats --json` until exit 0; exit 75 (not indexed yet) is the
    only transient answer. Returns (seconds, doc) or raises."""
    t0 = time.monotonic()
    while True:
        rc, out, err, _ = efs_cmd(where, 'stats', '--json', timeout=30)
        if rc == 0:
            return time.monotonic() - t0, json.loads(out)
        if rc != 75:
            raise RuntimeError(f'.efs/stats exit {rc}: {err.strip()[-200:]}')
        if time.monotonic() - t0 > timeout:
            raise RuntimeError(f'{where} not indexed within {timeout:.0f} s')
        time.sleep(1)


def h_d3(a):
    base = Path(a.dir)
    static = base / 'static'
    static.mkdir(parents=True, exist_ok=False)
    want, nbytes = set(), 0
    for i in range(a.static_files):
        sub = static / f's{i % 4}'
        sub.mkdir(exist_ok=True)
        name = f's{i % 4}/q{i:05d}' + ('.txt' if i % 3 == 0 else '.bin')
        data = payload(a.seed, 'd3/' + name, 1 + (i * 37) % 5000)
        write_exact(static / name, data)
        nbytes += len(data)
        want.add(name)
    created = time.monotonic()
    peer = Path(a.peer) / static.relative_to(a.mount) if a.peer else static
    out = dict(static_files=len(want), static_bytes=nbytes, writers=a.writers, queriers=a.queriers,
               load_s=a.load_s, io_deadline_s=a.io_deadline_s, query_deadline_s=a.query_deadline_s,
               query_path='peer mount' if a.peer else 'same mount')
    try:
        out['index_wait_s'], first = wait_indexed(peer, a.index_wait_s)
    except RuntimeError as exc:
        out['error'] = str(exc)
        return emit(out, False)
    out['refresh_ms'] = first['snapshot'].get('refresh_ms')
    txt = {n for n in want if n.endswith('.txt')}
    # Freshness: a fixture created before the cut is listed exactly once the
    # catalog covers it; poll (bounded) before the load starts.
    settle = max(30.0, 2 * (out['refresh_ms'] or 60000) / 1000 + 60)
    t0 = time.monotonic()
    while True:
        rc, so, err, _ = efs_cmd(peer, 'stats', '--json')
        doc = json.loads(so) if rc == 0 else {}
        if doc.get('stats', {}).get('tree_files') == len(want):
            break
        if time.monotonic() - t0 > settle:
            out['error'] = f'static fixture never published: {doc.get("stats")} rc={rc} {err[-200:]}'
            return emit(out, False)
        time.sleep(1)
    out['fixture_published_s'] = round(time.monotonic() - t0, 3)
    # Each member keeps its own catalog: one that has not refreshed since
    # the fixture was created answers "not indexed yet" (exit 75). That is
    # tolerated (counted) for one settle window after the fixture; later
    # it is an error (initial catalog creation must finish, D3/D4).
    ready_by = created + settle
    stop = time.monotonic() + a.load_s
    lock = threading.Lock()
    res = dict(io_ops=0, io_errors=[], io_over=[], io_lat=[], q_ops=0, q_errors=[], q_over=[], q_lat=[],
               q_busy=0, q_busy_max_s=0.0, q_not_indexed=0, generations=set(), files=[])

    def writer(w):
        d = base / 'load' / f'w{w}'
        d.mkdir(parents=True)
        i = 0
        while time.monotonic() < stop:
            name = f'f{i:05d}'
            data = payload(a.seed, f'd3/load/w{w}/{name}', 4096 + (i % 13) * 4096)
            t = time.monotonic()
            try:
                write_exact(d / name, data)
                verify_payload(d / name, data)
                os.stat(d)
                os.listdir(d)
                if i % 5 == 4:  # ordinary namespace churn too
                    os.rename(d / name, d / (name + '.r'))
                    name += '.r'
                dt = time.monotonic() - t
                with lock:
                    res['io_ops'] += 1
                    res['io_lat'].append(dt)
                    res['files'].append((f'load/w{w}/{name}', len(data), hashlib.sha256(data).hexdigest()))
                    if dt > a.io_deadline_s:
                        res['io_over'].append(dict(op=name, seconds=round(dt, 3)))
            except OSError as exc:
                with lock:
                    res['io_errors'].append(dict(op=name, writer=w, path=str(d / name),
                                                 errno=exc.errno, error=str(exc), at=now(),
                                                 **{k: v for k, v in getattr(exc, 'payload_detail', {}).items() if k != 'path'}))
            i += 1

    def check_query(kind):
        if kind == 'find':
            rc, so, err, dt = efs_cmd(peer, 'find', '.txt', '--match', 'suffix', '--json', '--limit', '10000',
                                      timeout=a.query_deadline_s + 30)
            if rc == 0:
                doc = json.loads(so)
                got = {bytes.fromhex(e['path_hex']).decode() for e in doc['entries']}
                return rc, dt, (doc['complete'] and got == txt) or f'find: {len(got)} names, {len(got ^ txt)} differ', doc
        elif kind == 'ls':
            rc, so, err, dt = efs_cmd(peer / 's1', 'ls', '--json', '--limit', '10000', timeout=a.query_deadline_s + 30)
            if rc == 0:
                doc = json.loads(so)
                got = {bytes.fromhex(e['path_hex']).decode() for e in doc['entries']}
                exp = {n.split('/', 1)[1] for n in want if n.startswith('s1/')}
                return rc, dt, (doc['complete'] and got == exp) or f'ls: {len(got)} names, {len(got ^ exp)} differ', doc
        elif kind == 'stats':
            rc, so, err, dt = efs_cmd(peer, 'stats', '--json', timeout=a.query_deadline_s + 30)
            if rc == 0:
                doc = json.loads(so)
                st = doc['stats']
                ok = st['tree_files'] == len(want) and st['tree_bytes'] == nbytes and st['tree_dirs'] == 4
                return rc, dt, ok or f'stats: {st}', doc
        else:  # bounded refusal: an invalid request is refused at once
            rc, so, err, dt = efs_cmd(peer, 'find', 'x' * 200, timeout=a.query_deadline_s)
            return rc, dt, (rc == 2 and 'exceeds' in err) or f'invalid find rc={rc} {err[-120:]}', None
        if rc == 1 and 'resource busy' in err:
            return rc, dt, 'busy', None
        if rc == 75 and time.monotonic() < ready_by:
            return rc, dt, 'not-indexed', None
        return rc, dt, f'{kind} exit {rc}: {err.strip()[-200:]}', None

    def querier(q):
        # EBUSY while mutations publish is a bounded refusal, never a
        # partial answer: it is retried, and the same kind must answer
        # exactly within the query deadline of the first refusal.
        kinds = ('find', 'ls', 'stats', 'invalid')
        i, refused = q, {}
        while time.monotonic() < stop:
            kind = kinds[i % len(kinds)]
            rc, dt, verdict, doc = check_query(kind)
            t = time.monotonic()
            with lock:
                res['q_ops'] += 1
                res['q_lat'].append(dt)
                if dt > a.query_deadline_s:
                    res['q_over'].append(dict(kind=kind, seconds=round(dt, 3)))
                if verdict == 'busy':
                    res['q_busy'] += 1
                    first = refused.setdefault(kind, t - dt)
                    if t - first > a.query_deadline_s:
                        res['q_errors'].append(dict(kind=kind, rc=rc, error=f'refused BUSY for {t - first:.1f} s',
                                                    at=now()))
                        refused.pop(kind)
                    else:
                        continue  # retry the same kind at once
                elif verdict == 'not-indexed':
                    res['q_not_indexed'] += 1
                    continue
                elif verdict is True and kind in refused:
                    res['q_busy_max_s'] = max(res['q_busy_max_s'], t - refused.pop(kind))
                if verdict is not True:
                    res['q_errors'].append(dict(kind=kind, rc=rc, error=str(verdict)[:300], at=now()))
                if doc:
                    res['generations'].add(doc['snapshot']['generation'])
            i += 1

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.writers + a.queriers) as pool:
        tasks = [pool.submit(writer, w) for w in range(a.writers)] + \
                [pool.submit(querier, q) for q in range(a.queriers)]
        for t in tasks:
            t.result()
    # Freshness under load: the load tree converges to the exact count.
    expected = len(res['files'])
    t0 = time.monotonic()
    load = Path(a.peer) / (base / 'load').relative_to(a.mount) if a.peer else base / 'load'
    converged, last, converge_s = False, None, None
    while time.monotonic() - t0 < settle:
        rc, so, err, _ = efs_cmd(load, 'stats', '--json')
        if rc == 0:
            last = json.loads(so)['stats']
            if last['tree_files'] == expected and last['tree_bytes'] == sum(s for _, s, _ in res['files']):
                converged, converge_s = True, round(time.monotonic() - t0, 3)
                break
        time.sleep(1)
    bad = []
    for name, size, digest in res['files']:
        try:
            if hashlib.sha256((base / name).read_bytes()).hexdigest() != digest:
                bad.append(name)
        except OSError as exc:
            bad.append(f'{name}: {exc}')
    out.update(io_ops=res['io_ops'], io_errors=res['io_errors'][:20], nio_errors=len(res['io_errors']),
               io_over_deadline=res['io_over'][:20], io_p50_s=pct(res['io_lat'], .5),
               io_p99_s=pct(res['io_lat'], .99), io_max_s=round(max(res['io_lat'] or [0]), 3),
               query_ops=res['q_ops'], query_errors=res['q_errors'][:20], nquery_errors=len(res['q_errors']),
               query_over_deadline=res['q_over'][:20], query_busy=res['q_busy'],
               query_busy_max_s=round(res['q_busy_max_s'], 3), query_not_indexed=res['q_not_indexed'],
               query_p50_s=pct(res['q_lat'], .5),
               query_p99_s=pct(res['q_lat'], .99), query_max_s=round(max(res['q_lat'] or [0]), 3),
               generations=len(res['generations']), load_files=expected, load_converged=converged,
               load_converge_s=converge_s, load_settle_s=settle, load_stats=last,
               payloads_verified=expected - len(bad), bad_payloads=bad[:20])
    ok = (not res['io_errors'] and not res['io_over'] and not res['q_errors'] and not res['q_over']
          and res['q_ops'] > 0 and res['io_ops'] > 0 and converged and not bad and len(res['generations']) > 1)
    if len(res['generations']) <= 1:
        out['error'] = 'no new catalog generation was published under load (refresh starved)'
    return emit(out, ok)


# ---- e4: declared scale fixtures -------------------------------------------

def h_e4_write(a):
    d = Path(a.dir)
    d.mkdir(parents=True, exist_ok=False)
    sizes = [1, 100, 1000, 4095, 4096, 4097, 8000, 16384]
    work = [(f's{i % 64:02d}/s{i:07d}', sizes[i % len(sizes)]) for i in range(a.small)]
    work += [(f'large/l{i:04d}', a.large_mib * (1 << 20) + i * 4097) for i in range(a.large)]
    for sub in {n.split('/')[0] for n, _ in work}:
        (d / sub).mkdir()
    lock = threading.Lock()
    files, errors = [], []
    t0 = time.monotonic()

    def one(item):
        name, size = item
        data = payload(a.seed, name, size)
        try:
            write_exact(d / name, data)
            e = entry(d, name, data)
            with lock:
                files.append(e)
        except OSError as exc:
            with lock:
                errors.append(dict(name=name, errno=exc.errno, error=str(exc)))

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.threads) as pool:
        list(pool.map(one, work, chunksize=16))
    for sub in sorted(p for p in d.iterdir() if p.is_dir()):
        fd = os.open(sub, os.O_RDONLY)
        os.fsync(fd)
        os.close(fd)
    seconds = time.monotonic() - t0
    small = [f for f in files if not f['name'].startswith('large/')]
    large = [f for f in files if f['name'].startswith('large/')]
    out = dict(dir=str(d), seed=a.seed, files=sorted(files, key=lambda f: f['name']), errors=errors[:50],
               nerrors=len(errors), target_small=a.small, target_large=a.large, large_mib=a.large_mib,
               completed_small=len(small), completed_large=len(large),
               bytes=sum(f['size'] for f in files), seconds=round(seconds, 3), threads=a.threads,
               small_per_s=round(len(small) / seconds, 1) if seconds else 0)
    return emit(out, not errors)


def h_e4_queries(a):
    """`.efs/stats --json` on the fixture in a loop until the stop file
    appears (query load during the cold reads); counts and latencies."""
    where = Path(a.dir)
    lat, errors, answers = [], [], 0
    end = time.monotonic() + a.max_s
    while time.monotonic() < end and not os.path.exists(a.stop):
        rc, so, err, dt = efs_cmd(where, 'stats', '--json', timeout=a.deadline_s + 30)
        lat.append(dt)
        if rc == 0:
            answers += 1
        elif rc != 75:
            errors.append(dict(rc=rc, error=err.strip()[-200:], at=now()))
        time.sleep(0.2)
    over = [x for x in lat if x > a.deadline_s]
    out = dict(queries=len(lat), answered=answers, errors=errors[:20], nerrors=len(errors),
               p50_s=pct(lat, .5), p99_s=pct(lat, .99), max_s=round(max(lat or [0]), 3),
               over_deadline=len(over), deadline_s=a.deadline_s)
    return emit(out, not errors and not over and answers > 0)


def h_e4_delete(a):
    """Unlink every large file and every second small file; the new
    manifest names what stays."""
    doc = json.loads(sys.stdin.read())
    d = Path(doc['dir'])
    keep, deleted, nbytes, errors = [], 0, 0, []
    for i, f in enumerate(doc['files']):
        if f['name'].startswith('large/') or i % 2:
            try:
                os.unlink(d / f['name'])
                deleted += 1
                nbytes += f['size']
            except OSError as exc:
                errors.append(dict(name=f['name'], errno=exc.errno, error=str(exc)))
                keep.append(f)
        else:
            keep.append(f)
    os.sync()
    return emit(dict(dir=str(d), seed=doc.get('seed'), files=keep, errors=errors, deleted=deleted,
                     deleted_bytes=nbytes), not errors)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    lo = sub.add_parser('load')
    lo.add_argument('--dir', required=True)
    lo.add_argument('--stop', required=True)
    lo.add_argument('--seed', required=True)
    lo.add_argument('--threads', type=int, default=4)
    lo.add_argument('--kib', type=int, default=64)
    lo.add_argument('--max-s', type=float, default=3600)
    lo.add_argument('--deadline-s', type=float, default=30)
    w = sub.add_parser('wire')
    w.add_argument('--node', action='append', required=True)
    w.add_argument('--timeout-s', type=float, default=5)
    r = sub.add_parser('refuse')
    r.add_argument('--src', required=True)
    r.add_argument('--scratch', required=True)
    cp = sub.add_parser('c5-prepare')
    cp.add_argument('--dir', required=True)
    cm = sub.add_parser('c5-mutate')
    cm.add_argument('--dir', required=True)
    cm.add_argument('--expect', choices=('eio', 'ok'), required=True)
    sm = sub.add_parser('smallfiles')
    sm.add_argument('op', choices=('write', 'rows', 'mutate'))
    sm.add_argument('--dir')
    sm.add_argument('--files', type=int, default=400)
    sm.add_argument('--seed', default='')
    sm.add_argument('--max-size', type=int, default=8192)
    sm.add_argument('--mgmt')
    sm.add_argument('--node', action='append', default=[])
    d3 = sub.add_parser('d3')
    d3.add_argument('--dir', required=True)
    d3.add_argument('--mount', required=True)
    d3.add_argument('--peer', help='second mount of the same cluster on this host (queries go there)')
    d3.add_argument('--seed', required=True)
    d3.add_argument('--static-files', type=int, default=200)
    d3.add_argument('--writers', type=int, default=8)
    d3.add_argument('--queriers', type=int, default=4)
    d3.add_argument('--load-s', type=float, default=300)
    d3.add_argument('--io-deadline-s', type=float, default=10)
    d3.add_argument('--query-deadline-s', type=float, default=30)
    d3.add_argument('--index-wait-s', type=float, default=600)
    e4 = sub.add_parser('e4')
    e4.add_argument('op', choices=('write', 'queries', 'delete'))
    e4.add_argument('--dir')
    e4.add_argument('--seed', default='')
    e4.add_argument('--small', type=int, default=0)
    e4.add_argument('--large', type=int, default=0)
    e4.add_argument('--large-mib', type=int, default=64)
    e4.add_argument('--threads', type=int, default=16)
    e4.add_argument('--stop')
    e4.add_argument('--max-s', type=float, default=7200)
    e4.add_argument('--deadline-s', type=float, default=30)
    a = p.parse_args(argv)
    if a.cmd == 'load':
        return h_load(a)
    if a.cmd == 'wire':
        return h_wire(a)
    if a.cmd == 'refuse':
        return h_refuse(a)
    if a.cmd == 'c5-prepare':
        return h_c5_prepare(a)
    if a.cmd == 'c5-mutate':
        return h_c5_mutate(a)
    if a.cmd == 'smallfiles':
        return {'write': h_small_write, 'rows': h_small_rows, 'mutate': h_small_mutate}[a.op](a)
    if a.cmd == 'd3':
        return h_d3(a)
    return {'write': h_e4_write, 'queries': h_e4_queries, 'delete': h_e4_delete}[a.op](a)


if __name__ == '__main__':
    sys.exit(main())
