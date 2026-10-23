#!/usr/bin/env python3
"""K13 private three-daemon gate: the ticketed PUT/REPORT path end to end.

Starts three efsd on 127.0.0.1 (EFS_MD_RAFT_N=3, EFS_TRANSPORT=tcp) with
stores under --work, one FUSE mount, and checks:
  * default PUT/flush: written files read back exactly (warm and cold),
    fsync succeeds, unflushed deletes and overwrites settle, and a clean
    unmount reports zero outstanding tickets;
  * tests/live/client_put_ticket.c on real daemons, same- and cross-group:
    lost admission/PUT/REPORT replies, collision, wrong bytes, revocation and
    cold replay of an admitted ticket;
  * W88: folded-span retry after trailer eviction through the host REPORT
    path, before and after a cold restart, with exact bytes read through
    the mount;
  * leader death mid-PUT: SIGKILL of a group leader during a streaming
    write, restart, exact bytes warm and after a cold restart.
No pub-reject line may appear in any daemon log. Only processes this script
started are signalled. Raw logs stay in --work (outside Git).
"""
import argparse, hashlib, os, random, re, signal, socket, subprocess, sys, threading, time
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--source', type=Path, required=True)
ap.add_argument('--work', type=Path, required=True)
ap.add_argument('--port', type=int, default=18700)
ap.add_argument('--stream-mib', type=int, default=256)
a = ap.parse_args()
src, work = a.source.resolve(), a.work.resolve()
assert 18700 <= a.port and a.port + 2 <= 18799, 'ports must stay in 18700-18799'
work.mkdir(parents=True, exist_ok=True)
mnt = work / 'mnt'; mnt.mkdir(exist_ok=True)
roots = [work / f'n{i+1}' for i in range(3)]
env = dict(os.environ, EFS_MD_RAFT_N='3', EFS_TRANSPORT='tcp', TMPDIR=str(work / 'tmp'))
(work / 'tmp').mkdir(exist_ok=True)
servers = {}
client = None
results = []
logs = []

def log(msg):
    print(msg, flush=True); results.append(msg)

def run(cmd, **kw):
    kw.setdefault('env', env)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)

def port_free(p):
    with socket.socket() as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try: s.bind(('127.0.0.1', p)); return True
        except OSError: return False

def wait(pred, label, limit):
    end = time.time() + limit
    while time.time() < end:
        if pred(): return
        time.sleep(.25)
    raise AssertionError(label + ' timed out')

def start_node(i):
    roots[i].mkdir(exist_ok=True)
    f = open(work / f'n{i+1}.log', 'ab'); logs.append(f)
    cmd = [src / 'efsd', '--node-id', str(i+1), '--addr', '127.0.0.1', '--port', str(a.port+i),
           '--storage', roots[i], '--quota', '4G', '--no-direct-io']
    if i: cmd += ['--join', f'127.0.0.1:{a.port}']
    servers[i] = subprocess.Popen([str(c) for c in cmd], env=env, stdout=f, stderr=f)
    (work / 'pids').open('a').write(f'efsd n{i+1} {servers[i].pid}\n')
    def listening():
        assert servers[i].poll() is None, f'efsd n{i+1} exited'
        try:
            with socket.create_connection(('127.0.0.1', a.port+i), timeout=.2): return True
        except OSError: return False
    wait(listening, f'n{i+1} listening', 30)

def start_all():
    for i in range(3): start_node(i)
    time.sleep(3)

def stop_all(crash):
    for p in servers.values():
        if p.poll() is None: p.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
    for p in servers.values():
        try: p.wait(timeout=30)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
    servers.clear()

def mount():
    global client
    f = open(work / 'fuse.log', 'ab'); logs.append(f)
    client = subprocess.Popen([str(src / 'efs-fuse'), f'127.0.0.1:{a.port}', str(mnt), '-f'], env=env, stdout=f, stderr=f)
    (work / 'pids').open('a').write(f'efs-fuse {client.pid}\n')
    def ready():
        assert client.poll() is None, 'efs-fuse exited'
        if subprocess.run(['mountpoint', '-q', str(mnt)]).returncode: return False
        return subprocess.run(['timeout', '2', 'stat', str(mnt)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    wait(ready, 'mount serving', 60)

def unmount():
    global client
    if client is None: return
    subprocess.run(['fusermount3', '-u', str(mnt)])
    try: client.wait(timeout=120)
    except subprocess.TimeoutExpired: client.kill(); client.wait(); subprocess.run(['fusermount3', '-uz', str(mnt)])
    client = None

def leaders():
    """{group: node index} from each node's own raft-status view."""
    out = {}
    for i, p in servers.items():
        if p.poll() is not None: continue
        r = subprocess.run([str(src / 'efs-mgmt'), 'raft-status', f'127.0.0.1:{a.port+i}'], env=env, capture_output=True, text=True, timeout=20)
        for m in re.finditer(r'group (\d+) hosted=1 role=(\w+) leader=(-?\d+)', r.stdout):
            if m.group(2).lower().startswith('lead'): out[int(m.group(1))] = i
    return out

def pattern(seed, n):
    rnd = random.Random(seed); return rnd.randbytes(n)

def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()

def write_file(name, chunks, fsync=True):
    p = mnt / name
    fd = os.open(p, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o644)
    try:
        for off, data in chunks: os.pwrite(fd, data, off)
        if fsync: os.fsync(fd)
    finally: os.close(fd)
    return p

def daemon_rejects():
    bad = []
    for i in range(3):
        t = (work / f'n{i+1}.log').read_text(errors='replace')
        bad += [l for l in t.splitlines() if 'pub-reject' in l or 'invalid REPORT rec' in l]
    return bad

expected = {}
helper = work / 'client-put-ticket'
try:
    for p in range(a.port, a.port + 3): assert port_free(p), f'port {p} busy'
    run(['cc', '-Wall', '-Wextra', '-Werror', '-I' + str(src / 'include'), src / 'tests/live/client_put_ticket.c',
         src / 'libefs.a', '-pthread', '-lm', '-ldl', '-libverbs', '-o', helper])
    start_all()
    run([src / 'efs-mgmt', 'raft-mkfs', f'127.0.0.1:{a.port}'], stdout=subprocess.DEVNULL)
    mount()
    log('three daemons (RAFT_N=3, tcp, buffered I/O) and one mount serving PASS')

    # Default PUT/flush through FUSE.
    cases = {
        'tiny': [(0, pattern(1, 1000))],
        'three-chunks': [(0, pattern(2, 300 * 1024))],
        'seq-8m': [(i << 20, pattern(100 + i, 1 << 20)) for i in range(8)],
        'spans': [(0, pattern(3, 1 << 20))] + [(off, pattern(200 + i, 4096)) for i, off in enumerate(random.Random(9).sample(range(0, 1 << 20, 4096), 64))],
        'rewrite': [(0, pattern(4, 512 * 1024)), (100 * 1024, pattern(5, 200 * 1024))],
        'zeros': [(0, bytes(2 << 20)), (131072 * 3 + 7, pattern(6, 9))],
    }
    for name, chunks in cases.items():
        p = write_file(name, chunks)
        buf = bytearray(max(off + len(d) for off, d in chunks))
        for off, d in chunks: buf[off:off + len(d)] = d
        expected[name] = hashlib.sha256(bytes(buf)).hexdigest()
        assert sha(p) == expected[name], ('warm bytes', name)
    # Unflushed delete and pre-REPORT overwrite: their tickets are abandoned.
    for k in range(20):
        p = write_file(f'scratch-{k}', [(0, pattern(300 + k, 256 * 1024))], fsync=False)
        os.unlink(p)
    p = write_file('overwritten', [(0, pattern(7, 1 << 20))], fsync=False)
    write_file('overwritten', [(0, pattern(8, 1 << 20))], fsync=True)
    expected['overwritten'] = hashlib.sha256(pattern(8, 1 << 20)).hexdigest()
    assert sha(p) == expected['overwritten']
    log(f'default ticketed PUT/flush: {len(cases)+1} files exact warm bytes with fsync, 20 unflushed deletes and a pre-REPORT overwrite PASS')

    # Ticket protocol on real daemons, same- and cross-group lanes.
    for cross in (0, 1):
        f = mnt / f'tickets-{cross}'; f.write_bytes(b'')
        out = run([helper, a.port, f.stat().st_ino, src / 'efs-mgmt', 'tickets', cross, work / f'ticket-{cross}.bin'],
                  capture_output=True, text=True, timeout=180).stdout
        log(out.strip())
    w88 = mnt / 'w88'; w88.write_bytes(b'')
    w88_proc = subprocess.Popen([str(helper), str(a.port), str(w88.stat().st_ino), str(src / 'efs-mgmt'), 'w88', str(work / 'w88.bin')],
                                env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    (work / 'pids').open('a').write(f'w88-helper {w88_proc.pid}\n')
    out = ''
    while 'w88 ready' not in out:
        line = w88_proc.stdout.readline()
        assert line, ('w88 helper exited', w88_proc.wait())
        out += line
    log(out.replace('w88 ready', '').strip())
    w88_ci = int(re.search(r'ci=(\d+)', out).group(1))
    w88_buf = bytearray((w88_ci + 1) * 131072)
    w88_buf[w88_ci * 131072:] = b'\x5a' * 131072; w88_buf[w88_ci * 131072 + 40] = 0xc3
    expected['w88'] = hashlib.sha256(bytes(w88_buf)).hexdigest()

    # Leader death mid-PUT. Async PUT acknowledges before durability: only
    # fsynced data is promised. Part A is fsynced before the kill and must
    # survive exactly (warm and cold). Part B streams across the SIGKILL and
    # its restart; every block must read back exact or fail with EIO (a lost
    # unsynced fragment), never as different bytes, and its fsync verdict is
    # recorded (W71 keeps a crashed root's COMMIT fenced).
    stream = mnt / 'stream'
    part_a = a.stream_mib // 4
    data_seed = 4242
    fd = os.open(stream, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    for i in range(part_a): os.pwrite(fd, pattern(data_seed + i, 1 << 20), i << 20)
    os.fsync(fd); os.close(fd)
    errors, b_fsync = [], []
    def writer():
        try:
            fd = os.open(stream, os.O_WRONLY)
            try:
                for i in range(part_a, a.stream_mib):
                    d = pattern(data_seed + i, 1 << 20); off = 0
                    while off < len(d): off += os.pwrite(fd, d[off:], (i << 20) + off)
                try: os.fsync(fd); b_fsync.append('ok')
                except OSError as e: b_fsync.append(f'errno {e.errno}')
            finally: os.close(fd)
        except Exception as e: errors.append(repr(e))
    t = threading.Thread(target=writer); t.start()
    time.sleep(1)
    lead = leaders()
    victim = max(set(lead.values()), key=lambda n: sum(1 for v in lead.values() if v == n))
    groups = sorted(g for g, n in lead.items() if n == victim)
    servers[victim].send_signal(signal.SIGKILL); servers[victim].wait()
    killed_at = time.time()
    time.sleep(4)
    start_node(victim)
    t.join(timeout=900)
    assert not t.is_alive(), 'stream writer hung'
    assert not errors, errors
    def check_stream(label):
        """Every block exact or EIO, never other bytes. A fsynced block may
        be EIO only while the restarted member is still in the client's
        node-down cooldown (a chunk stored on two of three); it must read
        exact once that member serves again."""
        exact, eio, late = 0, [], 0
        with open(stream, 'rb', buffering=0) as f:
            def block(i):
                got = os.pread(f.fileno(), 1 << 20, i << 20)
                assert got == pattern(data_seed + i, 1 << 20), (label, 'wrong bytes in block', i)
            for i in range(a.stream_mib):
                try: block(i); exact += 1
                except OSError as e:
                    assert e.errno == 5, (label, 'block', i, e); eio.append(i)
            synced = [i for i in eio if i < part_a or b_fsync == ['ok']]
            deadline = time.time() + 90
            while synced and time.time() < deadline:
                time.sleep(5)
                still = []
                for i in synced:
                    try: block(i); late += 1
                    except OSError as e:
                        assert e.errno == 5, (label, 'block', i, e); still.append(i)
                synced = still
            assert not synced, (label, 'fsynced blocks unreadable after the member returned', synced[:8])
        return exact, len(eio), late
    exact, eio, late = check_stream('warm')
    after = leaders()
    log(f'leader death mid-PUT: {part_a} MiB fsynced, then SIGKILL n{victim+1} (leader of groups {groups}) during the next {a.stream_mib-part_a} MiB, restart after 4 s; '
        f'part B fsync={b_fsync[0]}; warm read {exact} blocks exact at once, {eio} EIO of which {late} fsynced blocks exact once the member served again, 0 wrong; leaders now {after} PASS')

    unmount()
    fuse_text = (work / 'fuse.log').read_text(errors='replace')
    m = re.findall(r'put tickets outstanding at unmount: (\d+) \(settle rc=(-?\d+)\)', fuse_text)
    assert m, 'no ledger report at unmount'
    log(f'clean unmount ledger report: outstanding={m[-1][0]} settle_rc={m[-1][1]}')
    outstanding = int(m[-1][0])

    # Cold replay: crash every daemon, restart, remount.
    stop_all(crash=True)
    start_all()
    mount()
    for name, digest in expected.items():
        assert sha(mnt / name) == digest, ('cold bytes', name)
    cexact, ceio, clate = check_stream('cold')
    log(f'cold restart after crash of all daemons: {len(expected)} files exact (incl. W88 file); stream {cexact} blocks exact at once, {ceio} EIO ({clate} later exact), 0 wrong, every fsynced MiB exact PASS')
    for cross in (0, 1):
        f = mnt / f'tickets-{cross}'
        out = run([helper, a.port, f.stat().st_ino, src / 'efs-mgmt', 'cold', work / f'ticket-{cross}.bin'],
                  capture_output=True, text=True, timeout=180).stdout
        log(f'cross={cross}: ' + out.strip())
    rest, _ = w88_proc.communicate('go\n', timeout=180)
    assert w88_proc.returncode == 0, rest
    log(rest.strip())
    bad = daemon_rejects()
    assert not bad, bad[:5]
    log('no pub-reject / invalid REPORT record in any daemon log through the default workload, ticket probes, leader death and cold replay PASS')
    out = run([helper, a.port, w88.stat().st_ino, src / 'efs-mgmt', 'w88-foreign', work / 'w88.bin'], capture_output=True, text=True, timeout=180).stdout
    log(out.strip())
    assert sha(w88) == expected['w88'], 'W88 bytes after cold retry'
    probe = daemon_rejects()
    assert probe and all(f'ino={w88.stat().st_ino} ' in l for l in probe), probe[:5]
    log(f'the foreign-session replay was the only refused record ({len(probe)} log lines, reason=ticket) PASS')
    assert outstanding == 0, f'{outstanding} tickets left unsettled at clean unmount'
    log('all mount tickets settled (retired or abandoned) by clean unmount PASS')
    log('K13 private three-daemon ticket gate PASS')
finally:
    try:
        if 'w88_proc' in globals() and w88_proc.poll() is None: w88_proc.kill(); w88_proc.wait()
    except Exception: pass
    try: unmount()
    except Exception: pass
    stop_all(crash=False)
    for f in logs: f.close()
    (work / 'summary.txt').write_text('\n'.join(results) + '\n')
