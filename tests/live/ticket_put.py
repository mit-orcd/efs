"""K13 ticketed PUT for live harnesses that hand-build EFS_MSG_PUT_CHUNK.

Since K13 every data PUT names an ADMITTED ticket: storage stores a fragment
only after a fresh ReadIndex STATUS finds the exact ticket ADMITTED, and an
unticketed frame is refused (reply status 1). A harness therefore admits
through the production client ledger (tests/live/client_put_ticket.c,
modes `create` and `admit`) and builds the frame here:

  EFS_MSG_PUT_CHUNK (6): be32 export, be32 fragment index, the 216-byte
  ticket wire (EFS_PUT_UPLOAD_HEADER_LEN 224), then exactly fragment_len
  bytes. Reply EFS_MSG_PUT_CHUNK_REPLY (7): status, path, be64 boot verifier.

`Cluster` runs a disposable formed membership (tcp, raft-mkfs) for wire
tests that used to start one unformed daemon. Only processes it started
are signalled.
"""
import re, socket, struct, subprocess, time
from pathlib import Path

PUT_CHUNK, PUT_REPLY = 6, 7
TICKET_LEN, HEADER_LEN = 216, 224
OK, ERROR, BUSY, STALE = 0, 1, 3, 4

def build_helper(source, out, env=None):
    """Compiles client_put_ticket.c against libefs.a once; returns its path."""
    out = Path(out)
    if not out.exists():
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I' + str(Path(source) / 'include'),
                        str(Path(source) / 'tests/live/client_put_ticket.c'), str(Path(source) / 'libefs.a'),
                        '-pthread', '-lm', '-ldl', '-libverbs', '-o', str(out)], check=True, env=env)
    return out

class Ticket:
    def __init__(self, saved, line):
        data = Path(saved).read_bytes()
        m = re.search(r'ticket ino=(\d+) ci=(\d+) object=(\d+) nodes=(\d+),(\d+),(\d+) fragment_len=(\d+)', line)
        assert m, line
        self.ino, self.ci, self.object = map(int, m.group(1, 2, 3))
        self.nodes = [int(x) for x in m.group(4, 5, 6)]
        self.fragment_len = int(m.group(7))
        assert len(data) == TICKET_LEN + 3 * self.fragment_len, (saved, len(data))
        self.wire = data[:TICKET_LEN]
        n = self.fragment_len
        self.frags = [data[TICKET_LEN + i * n:TICKET_LEN + (i + 1) * n] for i in range(3)]

    def body(self, fi, data=None, wire=None):
        """PUT_CHUNK body for fragment fi; data/wire override for refusal checks."""
        return struct.pack('>II', 1, fi) + (self.wire if wire is None else wire) + \
            (self.frags[fi] if data is None else data)

def helper_run(helper, port, ino, mode, *args, members=3, env=None, timeout=90):
    run_env = dict(env or {}, EFS_PROBE_NODES=str(members))
    return subprocess.run([str(helper), str(port), str(ino), '/bin/false', mode, *map(str, args)],
                          check=True, capture_output=True, text=True, timeout=timeout, env=run_env).stdout

def create(helper, port, name, **kw):
    out = helper_run(helper, port, 0, 'create', name, **kw)
    return int(re.search(r'ino=(\d+)', out).group(1))

def unlink(helper, port, name, **kw):
    helper_run(helper, port, 0, 'unlink', name, **kw)

def admit(helper, port, ino, save, ci=0, fill=0x78, **kw):
    """(rc, Ticket or None). fill 0x78: fragments 0 and 1 are 'x' bytes."""
    out = helper_run(helper, port, ino, 'admit', ci, fill, save, **kw)
    rc = int(re.search(r'admit rc=(-?\d+)', out).group(1))
    return rc, (Ticket(save, out) if rc == 0 else None)

def unticketed_body(ino, ci=0, fi=0, gen=987654321, length=65536):
    """The pre-K13 native-endian PUT header (export, ino, ci, fi, digest,
    length, generation, hint): storage must refuse it."""
    head = struct.pack('=IxxxxQII32sIxxxxQIxxxx', 1, ino, ci, fi,
                       bytes.fromhex('70ff942c316810ac5ffe7081fc049dba30713646b1d6d7272cd5948ad5579804'),
                       length, gen, 0)
    return head + b'x' * length

def free_base(count):
    """An ephemeral base port with `count` consecutive free ports."""
    for _ in range(100):
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0)); base = s.getsockname()[1]
        if base + count >= 65536: continue
        ok = True
        for p in range(base, base + count):
            with socket.socket() as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try: s.bind(('127.0.0.1', p))
                except OSError: ok = False; break
        if ok: return base
    raise AssertionError('no free consecutive ports')

class Cluster:
    """`members` efsd on 127.0.0.1:base.. under work/n<i>, formed with raft-mkfs.
    storage(i) -> list of storage roots for member i; extra -> efsd flags."""
    def __init__(self, source, work, members=3, base=None, extra=(), storage=None, env=None):
        self.source, self.work, self.members = Path(source), Path(work), members
        self.base = base or free_base(members)
        self.extra = list(extra)
        self.storage = storage or (lambda i: [self.work / f'n{i+1}'])
        # W153: the member count comes from formation; EFS_MD_RAFT_N stops efsd.
        self.env = {k: v for k, v in dict(env or {}, EFS_TRANSPORT='tcp').items() if k != 'EFS_MD_RAFT_N'}
        self.procs, self.logs, self.joined = {}, [], set()

    def roots(self, i):
        return self.storage(i)

    def start(self, i):
        # K33 CLI: the seed and every restart as `efsd ADDR:PORT STORE`; the
        # first start of every other member joins the seed.
        for root in self.roots(i): Path(root).mkdir(parents=True, exist_ok=True)
        ep, store = f'127.0.0.1:{self.base + i}', ','.join(str(r) for r in self.roots(i))
        if i and i not in self.joined:
            cmd = [str(self.source / 'efsd'), '--join', f'127.0.0.1:{self.base}', '--listen', ep, store]
            self.joined.add(i)
        else:
            cmd = [str(self.source / 'efsd'), ep, store]
        cmd += ['--discovery-port', '0'] + self.extra
        log = open(self.work / f'n{i+1}.log', 'ab'); self.logs.append(log)
        self.procs[i] = subprocess.Popen(cmd, env=self.env, stdout=log, stderr=log)
        for _ in range(300):
            assert self.procs[i].poll() is None, f'efsd n{i+1} exited'
            try:
                with socket.create_connection(('127.0.0.1', self.base + i), .1): return
            except OSError: time.sleep(.1)
        raise AssertionError(f'efsd n{i+1} startup timeout')

    def form(self):
        for i in range(self.members): self.start(i)
        # W153: formation commits the members joined at that moment, so wait
        # until the seed lists every member up before raft-mkfs.
        deadline = time.monotonic() + 60
        while True:
            out = subprocess.run([str(self.source / 'efs-mgmt'), 'status', f'127.0.0.1:{self.base}'],
                                 env=self.env, capture_output=True, text=True, timeout=10).stdout
            if out.count('  up ') >= self.members: break
            assert time.monotonic() < deadline, ('members did not join', out[-400:])
            time.sleep(.2)
        subprocess.run([str(self.source / 'efs-mgmt'), 'raft-mkfs', f'127.0.0.1:{self.base}'],
                       check=True, env=self.env, stdout=subprocess.DEVNULL, timeout=120)

    def stop(self, i, timeout=20):
        p = self.procs.pop(i, None)
        if p is None or p.poll() is not None: return p and p.returncode
        p.terminate()
        try: return p.wait(timeout)
        except subprocess.TimeoutExpired: p.kill(); return p.wait()

    def close(self):
        for i in list(self.procs): self.stop(i)
        for log in self.logs: log.close()
