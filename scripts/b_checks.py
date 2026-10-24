#!/usr/bin/env python3
"""Alpha 1.0 checklist B1-B5 against any running EFS cluster.

    scripts/b_checks.py --mount /mnt/efs --seed HOST:PORT [--mount /other] \
        [--store ID=PATH ...] [--out result.json]

Read-only apart from its own test directory (created under the first mount,
removed at the end unless --keep): it writes files there, reads them back,
and asks the cluster with `efs-mgmt status`, `raft-status` and
`raft-getchunks`. It never changes membership, quotas or configuration and
never touches files outside the test directory or any store.

  B1  every chunk row and packed segment of the test files names exactly
      D1/D2/P on three distinct current members; with --store, the fragment
      records persisted on those stores match the rows (kind, object,
      generation, chunk, role), each on the row's node and on no other.
  B2  boundary sizes (empty, 1 B, page, half-chunk and chunk edges, the last
      chunk of a file): exact bytes through every mount, sizes, chunk rows
      (ceil(size/chunk) unpacked rows, or one packed row of the file's length).
  B3  placement over the spread files: histogram node x role (D1/D2/P).
      Three members: every chunk on all three. Four: the two chunks of a file
      omit different members and every member is omitted somewhere. Five or
      more: every member holds every role.
  B4  packing: the small files are switched into segments (waits up to
      --pack-wait s); group voters and segment destinations are recorded,
      segment fragments must be on members, and with enough segments every
      member (not only the voters) holds segment fragments.
  B5  `efs-mgmt status` on every member: identical usable capacity equal to
      the 3-of-N bound over the members' capacities, free never above
      capacity or 2/3 of the room; df on every mount equals status; the
      physical cost of the large files is 1.5x their logical bytes (plus at
      most --overhead-tol), small-file cost before and after packing is
      reported. A full member and capacity after an addition are NOTRUN
      here (they change the cluster; tests/test_b_geometry.py covers them).

The result file is JSON: {"verdict": "PASS"|"FAIL", "items": {"B1": {...}},
"notrun": [...], ...}. Exit 0 PASS, 1 FAIL, 2 usage or cluster unreachable.
On a cluster with other writers the B5 cost figures include their bytes;
run it on a quiet cluster.
"""
import argparse, hashlib, json, os, re, socket, struct, subprocess, sys, time
from pathlib import Path

CHUNK = 128 * 1024
PACK_THRESHOLD = 16384
REC = 128
TAIL = 4096
MAGIC = b'EFSFRAG\0'


# ---- parsing (pure, unit-tested by tests/test_b_checks_driver.py) --------

def parse_status(text):
    """`efs-mgmt status` -> {'members': {id: {...}}, 'usable': {...}|None, 'state': str}."""
    members, cur = {}, None
    for line in text.splitlines():
        m = re.match(r'^\s+node (\d+): (\S+)\s+(up|DOWN)\b', line)
        if m:
            cur = int(m.group(1))
            members[cur] = {'ep': m.group(2), 'up': m.group(3) == 'up'}
            continue
        m = re.match(r'^\s+bytes: used=(\d+) quota=(\d+) capacity=(\d+) available=(\d+) valid=(\d)', line)
        if m and cur is not None:
            members[cur].update(used=int(m.group(1)), quota=int(m.group(2)),
                                capacity=int(m.group(3)), available=int(m.group(4)),
                                valid=m.group(5) == '1')
    usable = None
    m = re.search(r'^Usable bytes: used=(\d+) free=(\d+) capacity=(\d+)', text, re.M)
    if m:
        usable = {'used': int(m.group(1)), 'free': int(m.group(2)), 'capacity': int(m.group(3))}
    m = re.search(r'^Cluster state: (\S+)', text, re.M)
    return {'members': members, 'usable': usable, 'state': m.group(1) if m else None}


def parse_voters(text):
    """`efs-mgmt raft-status` -> {group: (hosted, [voter node ids])} (raft id = node id - 1)."""
    out = {}
    for g, hosted, mask in re.findall(r'group (\d+) hosted=(\d+) .*?voters=(0x[0-9a-f]+)', text):
        v = int(mask, 16)
        out[int(g)] = (int(hosted), [b + 1 for b in range(32) if v >> b & 1])
    return out


def parse_getchunks(text):
    """`efs-mgmt raft-getchunks` (EFS_MGMT_CHUNKS large) -> (status, [row])."""
    m = re.search(r'raft-getchunks status=(\d+) primary=\d+ ino=\d+ count=(\d+)', text)
    if not m:
        return None, []
    rows = []
    for line in text.splitlines():
        r = re.match(r'^\s+ci=(\d+) nodes=(\d+),(\d+),(\d+) base_gen=(\d+) obj_gen=(\d+) '
                     r'ck0=([0-9a-f]{8}) spans=(\d+)', line)
        if r:
            rows.append({'ci': int(r.group(1)), 'nodes': [int(r.group(k)) for k in (2, 3, 4)],
                         'base_gen': int(r.group(5)), 'obj_gen': int(r.group(6)),
                         'ck0': r.group(7), 'spans': int(r.group(8)), 'packed': None,
                         'span': []})
            continue
        sp = re.match(r'^\s+span off=(\d+) len=(\d+) gen=(\d+) seq=(\d+) nodes=(\d+),(\d+),(\d+)', line)
        if sp and rows:
            rows[-1]['span'].append({'off': int(sp.group(1)), 'len': int(sp.group(2)),
                                     'gen': int(sp.group(3)), 'seq': int(sp.group(4)),
                                     'nodes': [int(sp.group(k)) for k in (5, 6, 7)]})
            continue
        p = re.match(r'^\s+packed segment=(\d+) gen=(\d+) group=(\d+) offset=(\d+) len=(\d+)', line)
        if p and rows:
            rows[-1]['packed'] = dict(zip(('segment', 'gen', 'group', 'offset', 'len'),
                                          (int(x) for x in p.groups())))
        elif re.match(r'^\s+packed reference malformed', line) and rows:
            rows[-1]['packed'] = {'malformed': True}
    return int(m.group(1)), rows


def parse_record(blob):
    """Identity of one fragment record (frag_record.h, format 2), or None."""
    if len(blob) < REC or blob[:8] != MAGIC:
        return None
    ver, kind, nbytes, space, fi, obj, gen, ci, plen, prof = struct.unpack('>HHIIIQQIII', blob[8:52])
    if ver != 2 or nbytes != REC:
        return None
    return {'kind': kind, 'space': space, 'fi': fi, 'object': obj, 'gen': gen, 'ci': ci,
            'plen': plen, 'profile': prof}


def read_record(path):
    """Record of a fragment file: buffered (payload || 128 B record) or
    O_DIRECT (payload || 4 KiB tail page starting with the record)."""
    try:
        size = os.path.getsize(path)
        with open(path, 'rb') as f:
            for off in (size - REC, size - TAIL):
                if off < 0:
                    continue
                f.seek(off)
                r = parse_record(f.read(REC))
                if r and r['plen'] == off:
                    r['size'] = size
                    r['alloc'] = os.stat(path).st_blocks * 512
                    return r
    except OSError:
        return None
    return None


def store_inventory(root):
    """Every fragment record under one store's data/ tree (metadata excluded)."""
    out = []
    data = Path(root) / 'data'
    for dirpath, dirnames, files in os.walk(data):
        for name in files:
            if '.tmp' in name or name.endswith('.sum'):
                continue
            p = os.path.join(dirpath, name)
            r = read_record(p)
            if r:
                r['path'] = p
                out.append(r)
    return out


def capacity_logical(caps):
    """efs_capacity_logical: 2 x the largest M with sum(min(c, M)) >= 3M."""
    caps = list(caps)
    if len(caps) < 3 or sum(1 for c in caps if c) < 3:
        return 0
    lo, hi = 0, sum(caps)
    while lo < hi:
        mid = lo + (hi - lo + 1) // 2
        if sum(min(c, mid) for c in caps) >= 3 * mid:
            lo = mid
        else:
            hi = mid - 1
    return 2 * lo


def member_caps(m):
    """(capacity, room) of one status member the way efs-mgmt status models
    it: the member's own capacity/available when valid, else its quota."""
    if m.get('valid'):
        return m['capacity'], m['available']
    q, u = m.get('quota', 0), m.get('used', 0)
    return q, max(q - u, 0)


def stripe_ok(nodes, members):
    return len(nodes) == 3 and len(set(nodes)) == 3 and all(n in members for n in nodes)


def stub_base(r):
    """A chunk row without a base object: a partial chunk written as spans
    over zeros (nodes 0,0,0, generation 0, zero checksum)."""
    return r['nodes'] == [0, 0, 0] and r['obj_gen'] == 0 and r['ck0'] == '00000000'


def row_objects(r):
    """(generation, nodes) of every stored object of one unpacked row: the
    base unless it is a stub, then every span. Each one is a 2+1 stripe."""
    out = [] if stub_base(r) else [(r['obj_gen'], r['nodes'])]
    return out + [(sp['gen'], sp['nodes']) for sp in r.get('span', [])]


def row_stripe(r):
    """The stripe that holds most of the row: the base, else the first span."""
    objs = row_objects(r)
    return objs[0][1] if objs else r['nodes']


def check_rows(name, size, rows, members, chunk):
    """B1/B2 row checks for one file; returns a list of failure strings."""
    bad = []
    for r in rows:
        if r['packed']:
            objs = [(r['obj_gen'], r['nodes'])]
        else:
            objs = row_objects(r)
            if len(r.get('span', [])) != r['spans']:
                bad.append(f'{name}: ci={r["ci"]} {r["spans"]} spans, {len(r.get("span", []))} listed')
            if not objs:
                bad.append(f'{name}: ci={r["ci"]} no base and no span (stripe {r["nodes"]})')
        for gen, nodes in objs:
            if not stripe_ok(nodes, members):
                bad.append(f'{name}: ci={r["ci"]} gen={gen} stripe {nodes} not D1/D2/P on 3 distinct members')
        if r['packed'] and r['packed'].get('malformed'):
            bad.append(f'{name}: malformed packed reference')
    packed = [r for r in rows if r['packed']]
    if size == 0:
        return bad
    if packed:
        if len(rows) != 1 or rows[0]['ci'] != 0 or packed[0]['packed'].get('len') != size:
            bad.append(f'{name}: packed rows {[(r["ci"], r["packed"]) for r in rows]} for {size} B')
        return bad
    want = list(range((size + chunk - 1) // chunk))
    got = sorted(r['ci'] for r in rows)
    if got != want:
        bad.append(f'{name}: chunk rows {got[:8]}... ({len(got)}) for {size} B, want {len(want)}')
    return bad


def inventory_check(rows_by_file, inv_by_node, export_hint=None):
    """B1 persisted inventory: each row's fragments are on the row's nodes."""
    bad, checked = [], 0
    index = {}
    for nid, recs in inv_by_node.items():
        for r in recs:
            if r['kind'] == 1:
                key = (1, r['object'], r['ci'], r['gen'], r['fi'])
            else:
                key = (2, r['object'], 0, r['gen'], r['fi'])
            index.setdefault(key, []).append(nid)
    have = set(inv_by_node)
    seen_segments = set()
    for (name, ino), rows in rows_by_file.items():
        for r in rows:
            if r['packed']:
                seg = (r['packed']['segment'], r['packed']['gen'])
                if seg in seen_segments:
                    continue
                seen_segments.add(seg)
                keys = [(2, seg[0], 0, seg[1], fi) for fi in range(3)]
                objs = [(keys, r['nodes'])]
            else:
                objs = [([(1, ino, r['ci'], gen, fi) for fi in range(3)], nodes)
                        for gen, nodes in row_objects(r)]
            for keys, nodes in objs:
                for fi, key in enumerate(keys):
                    want = nodes[fi]
                    where = index.get(key, [])
                    if want in have:
                        checked += 1
                        if want not in where:
                            bad.append(f'{name}: {"segment" if key[0] == 2 else "ci=%d gen=%d" % (r["ci"], key[3])} '
                                       f'role {fi} not on node {want} (found on {where})')
                    stray = [n for n in where if n != want]
                    if stray:
                        bad.append(f'{name}: role {fi} of {key} also on {stray}')
    return bad, checked


def file_costs(rows_by_file, inv_by_node):
    """Physical bytes per file from the persisted records: fragment payload
    (what quotas charge) and allocated blocks (records, tail pages and
    filesystem alignment included). A segment's cost is shared by the
    listed files in it in proportion to their packed length (files of
    other writers in the same segment are not known here)."""
    rec = {}
    for nid, recs in inv_by_node.items():
        for r in recs:
            key = (r['kind'], r['object'], r['ci'] if r['kind'] == 1 else 0, r['gen'], r['fi'])
            rec[key] = r
    seg_files = {}
    for (name, ino), rows in rows_by_file.items():
        for r in rows:
            if r['packed']:
                k = (r['packed']['segment'], r['packed']['gen'])
                seg_files[k] = seg_files.get(k, 0) + max(r['packed'].get('len', 0), 1)
    out = {}
    for (name, ino), rows in rows_by_file.items():
        pay = alloc = 0.0
        found = 0
        for r in rows:
            if r['packed']:
                k = (r['packed']['segment'], r['packed']['gen'])
                keys = [(2, k[0], 0, k[1], fi) for fi in range(3)]
                share = seg_files[k] / max(r['packed'].get('len', 0), 1)
            else:
                keys = [(1, ino, r['ci'], gen, fi) for gen, _ in row_objects(r) for fi in range(3)]
                share = 1
            for key in keys:
                if key in rec:
                    found += 1
                    pay += rec[key]['plen'] / share
                    alloc += rec[key]['alloc'] / share
        expected = sum(3 if r['packed'] else 3 * len(row_objects(r)) for r in rows)
        out[name] = {'fragments': found, 'expected': expected, 'payload': pay, 'allocated': alloc}
    return out


# ---- cluster access --------------------------------------------------------

class Cluster:
    def __init__(self, mgmt, seed):
        self.mgmt, self.seed = mgmt, seed

    def run(self, *args, env=None, timeout=60):
        e = dict(os.environ, **(env or {}))
        p = subprocess.run([self.mgmt, *args], capture_output=True, text=True, timeout=timeout, env=e)
        return p.returncode, p.stdout + p.stderr

    def status(self, ep=None):
        rc, out = self.run('status', ep or self.seed)
        st = parse_status(out)
        st['raw_rc'] = rc
        return st

    def voters(self, ep):
        rc, out = self.run('raft-status', ep)
        return parse_voters(out) if rc == 0 else {}

    def chunks(self, ino, eps):
        """All rows of one inode from the first member that answers status 0."""
        last = ''
        for ep in eps:
            rows, start, ok = [], 0, True
            while True:
                rc, out = self.run('raft-getchunks', ep, str(ino), str(start),
                                   env={'EFS_MGMT_CHUNKS': '1000'})
                st, got = parse_getchunks(out)
                if rc != 0 or st != 0:
                    ok, last = False, out
                    break
                rows += got
                if len(got) < 64:
                    break
                start = got[-1]['ci'] + 1
            if ok:
                return rows
        raise RuntimeError(f'raft-getchunks {ino}: no member answered: {last[-500:]}')

    def version(self, ep):
        rc, out = self.run('version', ep)
        m = re.search(r'^version \S+ (.*)$', out, re.M)
        return m.group(1).strip() if m else None


def payload(name, size):
    out, k = bytearray(), 0
    while len(out) < size:
        out += hashlib.sha256(f'b-checks:{name}:{k}'.encode()).digest()
        k += 1
    return bytes(out[:size])


def write_file(path, data):
    with open(path, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def read_back(path):
    with open(path, 'rb') as f:
        return f.read()


def statfs(mount):
    st = os.statvfs(mount)
    return {'total': st.f_blocks * st.f_frsize, 'free': st.f_bavail * st.f_frsize}


B2_SIZES = (0, 1, 2, 511, 4095, 4096, 4097, 16383, 16384, 16385, 65535, 65536, 65537,
            CHUNK - 1, CHUNK, CHUNK + 1, 2 * CHUNK + 17, 3 * CHUNK, 1024 * 1024 + 3)


class Item:
    def __init__(self, name):
        self.name, self.fail, self.info, self.notrun, self.checks = name, [], {}, [], 0

    def check(self, cond, msg):
        self.checks += 1
        if not cond:
            self.fail.append(msg)
        return cond

    def result(self):
        return {'verdict': 'FAIL' if self.fail else 'PASS', 'checks': self.checks,
                'failures': self.fail[:50], 'failure_count': len(self.fail),
                'notrun': self.notrun, **self.info}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--mount', action='append', required=True,
                    help='EFS mount; repeat for mounts through other members (first one writes)')
    ap.add_argument('--seed', required=True, help='HOST:PORT of any member')
    ap.add_argument('--mgmt', default=None, help='efs-mgmt binary (default: next to this repo, then PATH)')
    ap.add_argument('--store', action='append', default=[], metavar='ID=PATH',
                    help='a member store readable here (B1 persisted inventory)')
    ap.add_argument('--out', default='b-checks.json')
    ap.add_argument('--dir', default=None, help='test directory name under the first mount')
    ap.add_argument('--chunk-size', type=int, default=CHUNK)
    ap.add_argument('--spread-files', type=int, default=0, help='two-chunk files for B3 (default 12 x N, at least 48)')
    ap.add_argument('--small-files', type=int, default=0, help='small files for B4 (default 40 x N)')
    ap.add_argument('--pack-wait', type=float, default=300.0, help='seconds to wait for packing')
    ap.add_argument('--df-wait', type=float, default=6.0, help='quiet seconds before df vs status')
    ap.add_argument('--overhead-tol', type=float, default=0.02,
                    help='--strict-used: allowed extra used bytes over 1.5x for the large files')
    ap.add_argument('--strict-used', action='store_true',
                    help='require the status used delta of the large files to be 1.5x (quiet cluster only:'
                         ' GC, packing and other writers move used bytes)')
    ap.add_argument('--expect-members', type=int, default=0)
    ap.add_argument('--items', default='B1,B2,B3,B4,B5')
    ap.add_argument('--keep', action='store_true', help='keep the test directory')
    a = ap.parse_args(argv)

    root = Path(__file__).resolve().parents[1]
    mgmt = a.mgmt or (str(root / 'efs-mgmt') if (root / 'efs-mgmt').exists() else 'efs-mgmt')
    want = set(a.items.split(','))
    cs = a.chunk_size
    c = Cluster(mgmt, a.seed)
    t0 = time.time()
    result = {'tool': 'b_checks', 'format': 1, 'seed': a.seed, 'mounts': a.mount,
              'host': socket.gethostname(), 'started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'chunk_size': cs, 'items': {}, 'notrun': []}

    st0 = c.status()
    members = st0['members']
    if not members:
        print(f'b_checks: no members from {a.seed}', file=sys.stderr)
        return 2
    ids = sorted(members)
    eps = [members[i]['ep'] for i in ids]
    n = len(ids)
    result['members'] = {str(i): members[i] for i in ids}
    result['state'] = st0['state']
    result['versions'] = {str(i): c.version(members[i]['ep']) for i in ids}
    voters = {}
    for i in ids:
        for g, (hosted, v) in c.voters(members[i]['ep']).items():
            if hosted:
                voters.setdefault(g, v)
    result['voters'] = {str(g): v for g, v in voters.items()}
    down = [i for i in ids if not members[i]['up']]
    if a.expect_members and n != a.expect_members:
        print(f'b_checks: {n} members, expected {a.expect_members}', file=sys.stderr)
        return 2
    if down:
        print(f'b_checks: members down {down}: checks need every member up', file=sys.stderr)
        return 2

    mount = Path(a.mount[0])
    tdir = mount / (a.dir or f'.b-checks-{socket.gethostname()}-{os.getpid()}-{int(t0)}')
    tdir.mkdir()
    rel = tdir.relative_to(mount)
    items = {k: Item(k) for k in ('B1', 'B2', 'B3', 'B4', 'B5')}
    rows_by_file = {}
    try:
        # B5 baseline: per-member used before anything is written.
        used0 = {i: members[i].get('used') for i in ids}

        # Large files only first: their physical cost is the 1.5x figure.
        large = {f'large-{k}': size for k, size in enumerate((4 * cs, 8 * cs, 16 * cs))}
        for name, size in large.items():
            write_file(tdir / name, payload(name, size))
        time.sleep(1.0)
        st1 = c.status()
        used1 = {i: st1['members'].get(i, {}).get('used') for i in ids}

        # B2 boundary files and B3 spread files.
        files = {}
        for size in B2_SIZES:
            name = f'b2-{size}'
            write_file(tdir / name, payload(name, size))
            files[name] = size
        nspread = a.spread_files or max(48, 12 * n)
        for k in range(nspread):
            name = f'spread-{k}'
            write_file(tdir / name, payload(name, 2 * cs))
            files[name] = 2 * cs
        files.update(large)
        # B4 small files (packable).
        nsmall = a.small_files or 40 * n
        small = {}
        for k in range(nsmall):
            name = f'small-{k}'
            size = 1 + (k * 4099) % PACK_THRESHOLD
            write_file(tdir / name, payload(name, size))
            small[name] = size
        files.update(small)
        time.sleep(1.0)
        st2 = c.status()
        used2 = {i: st2['members'].get(i, {}).get('used') for i in ids}
        inv = {}

        # B2: exact bytes through every mount.
        b2 = items['B2']
        for mnt in a.mount:
            base = Path(mnt) / rel
            for name, size in files.items():
                try:
                    got = read_back(base / name)
                except OSError as e:
                    b2.check(False, f'{mnt}: {name}: read {e}')
                    continue
                b2.check(len(got) == size and os.stat(base / name).st_size == size,
                         f'{mnt}: {name}: size {len(got)} want {size}')
                b2.check(got == payload(name, size), f'{mnt}: {name}: bytes differ')

        # Rows: B1 for every file, B2 row shape, B3 histogram over spread files.
        inos = {name: os.stat(tdir / name).st_ino for name in files}
        b1, b3 = items['B1'], items['B3']
        hist = {i: [0, 0, 0] for i in ids}
        omitted = {i: 0 for i in ids}
        for name, size in files.items():
            if name in small:
                continue
            rows = c.chunks(inos[name], eps)
            rows_by_file[(name, inos[name])] = rows
            for msg in check_rows(name, size, rows, members, cs):
                (b1 if 'stripe' in msg or 'malformed' in msg else b2).check(False, msg)
            b1.checks += len(rows)
            b2.checks += 1
            if name.startswith('spread-'):
                omit = []
                for r in rows:
                    stripe = row_stripe(r)
                    for role, nid in enumerate(stripe):
                        if nid in hist:
                            hist[nid][role] += 1
                    left = [i for i in ids if i not in stripe]
                    for i in left:
                        omitted[i] += 1
                    omit.append(tuple(left))
                    if n == 3:
                        b3.check(sorted(stripe) == ids, f'{name} ci={r["ci"]}: not on all three {stripe}')
                if n == 4 and len(omit) == 2:
                    b3.check(omit[0] != omit[1], f'{name}: both chunks omit {omit[0]} (no rotation)')
        b3.info['histogram'] = {str(i): {'D1': h[0], 'D2': h[1], 'P': h[2]} for i, h in hist.items()}
        b3.info['omitted'] = {str(i): v for i, v in omitted.items()}
        b3.info['members'] = n
        if n == 4:
            for i in ids:
                b3.check(omitted[i] > 0, f'4 members: node {i} never omitted')
        if n >= 5:
            for i in ids:
                for role in range(3):
                    b3.check(hist[i][role] > 0, f'{n} members: node {i} holds no {("D1", "D2", "P")[role]}')
        for i in ids:
            b3.check(sum(hist[i]) > 0, f'node {i} holds no fragment of the spread files')

        # B4: wait for packing, then rows of the small files.
        b4 = items['B4']
        if 'B4' in want:
            end = time.time() + a.pack_wait
            pending = dict(small)
            packed_rows = {}
            while pending and time.time() < end:
                for name in list(pending):
                    rows = c.chunks(inos[name], eps)
                    if rows and all(r['packed'] for r in rows):
                        packed_rows[name] = rows
                        pending.pop(name)
                if pending:
                    time.sleep(2.0)
            for name in pending:
                rows_by_file[(name, inos[name])] = c.chunks(inos[name], eps)
            b4.check(not pending, f'{len(pending)} of {len(small)} small files not packed after {a.pack_wait:.0f} s')
            segs = {}
            for name, rows in packed_rows.items():
                rows_by_file[(name, inos[name])] = rows
                for msg in check_rows(name, small[name], rows, members, cs):
                    (b1 if 'stripe' in msg or 'malformed' in msg else b2).check(False, msg)
                r = rows[0]
                key = (r['packed']['segment'], r['packed']['gen'])
                if key in segs:
                    b4.check(segs[key]['nodes'] == r['nodes'], f'segment {key}: rows disagree on nodes')
                segs.setdefault(key, {'nodes': r['nodes'], 'group': r['packed']['group'], 'files': 0})
                segs[key]['files'] += 1
            seg_hist = {i: [0, 0, 0] for i in ids}
            for s in segs.values():
                b4.check(stripe_ok(s['nodes'], members), f'segment stripe {s["nodes"]} not on 3 distinct members')
                for role, nid in enumerate(s['nodes']):
                    if nid in seg_hist:
                        seg_hist[nid][role] += 1
            on_voters = sum(1 for s in segs.values() for nid in s['nodes']
                            if nid in voters.get(s['group'], []))
            total = 3 * len(segs)
            b4.info.update(segments=len(segs), packed_files=len(packed_rows),
                           histogram={str(i): {'D1': h[0], 'D2': h[1], 'P': h[2]} for i, h in seg_hist.items()},
                           destinations=sorted({nid for s in segs.values() for nid in s['nodes']}),
                           eligible=ids, group_voters={str(g): v for g, v in voters.items()},
                           on_voters_share=round(on_voters / total, 3) if total else None,
                           usable_ring=capacity_logical(member_caps(members[i])[0] for i in ids),
                           usable_voters_only={str(g): capacity_logical(member_caps(members[i])[0]
                                                                        for i in v if i in members)
                                               for g, v in voters.items()})
            if len(segs) >= 4 * n:
                for i in ids:
                    b4.check(sum(seg_hist[i]) > 0, f'node {i} holds no segment fragment ({len(segs)} segments)')
            elif segs:
                b4.notrun.append(f'every-member spread: only {len(segs)} segments (< {4 * n})')
            if n > 3 and total and len(segs) >= 4 * n:
                b4.check(on_voters < total, 'every segment fragment is on a group voter')
            # Packed files read back exactly through every mount.
            for mnt in a.mount:
                base = Path(mnt) / rel
                for name in packed_rows:
                    try:
                        got = read_back(base / name)
                    except OSError as e:
                        b2.check(False, f'{mnt}: packed {name}: read {e}')
                        continue
                    b2.check(got == payload(name, small[name]), f'{mnt}: packed {name}: bytes differ')
        else:
            b4.notrun.append('not selected')

        # B1 persisted inventory. Rows again first: files the packer switched
        # meanwhile now name their segment, and reclaim may already have
        # removed the fragments their old rows named.
        if a.store:
            for key in list(rows_by_file):
                rows = c.chunks(key[1], eps)
                for msg in check_rows(key[0], files[key[0]], rows, members, cs):
                    (b1 if 'stripe' in msg or 'malformed' in msg else b2).check(False, msg)
                rows_by_file[key] = rows
        for spec in a.store:
            nid, path = spec.split('=', 1)
            inv[int(nid)] = store_inventory(path)
        if inv:
            bad, checked = inventory_check(rows_by_file, inv)
            for msg in bad:
                b1.check(False, msg)
            b1.checks += checked
            b1.info['inventory_checked'] = checked
            b1.info['inventory_stores'] = sorted(inv)
            if set(inv) != set(ids):
                b1.notrun.append(f'inventory on stores {sorted(set(ids) - set(inv))} (not readable here)')
        else:
            b1.notrun.append('persisted inventory (no --store given)')

        # B5: status on every member, df on every mount, costs.
        b5 = items['B5']
        time.sleep(a.df_wait)
        views = {i: c.status(members[i]['ep']) for i in ids}
        caps = None
        for i, v in views.items():
            b5.check(v['usable'] is not None, f'status on {i}: no usable line ({v["state"]})')
            if v['usable'] is None:
                continue
            u = v['usable']
            m = v['members']
            model = capacity_logical(member_caps(m[k])[0] for k in sorted(m))
            room = capacity_logical(member_caps(m[k])[1] for k in sorted(m))
            b5.check(u['capacity'] == model, f'status on {i}: capacity {u["capacity"]} != 3-of-N bound {model}')
            b5.check(u['free'] == min(room, model), f'status on {i}: free {u["free"]} != bound over room {room}')
            b5.check(u['free'] <= u['capacity'], f'status on {i}: free above capacity')
            b5.check(3 * u['capacity'] <= 2 * sum(member_caps(m[k])[0] for k in m),
                     f'status on {i}: capacity above 2/3 of the physical sum')
            if caps is None:
                caps = u
            else:
                b5.check(u['capacity'] == caps['capacity'], f'status on {i}: capacity differs between members')
        b5.info['usable'] = caps
        df = {}
        for mnt in a.mount:
            d = statfs(mnt)
            df[mnt] = d
            if caps:
                b5.check(d['total'] == caps['capacity'] // 512 * 512,
                         f'df {mnt}: total {d["total"]} != status capacity {caps["capacity"]}')
                # Both sample members within a few seconds; allow the bytes
                # written since (none in a quiet window) plus one chunk.
                b5.check(abs(d['free'] - caps['free']) <= 3 * cs,
                         f'df {mnt}: free {d["free"]} != status free {caps["free"]}')
        b5.info['df'] = df
        logical_large = sum(large.values())
        if all(used0[i] is not None and used1[i] is not None for i in ids):
            phys = sum(used1[i] - used0[i] for i in ids)
            ratio = phys / logical_large
            b5.info['large_used_delta'] = {'logical': logical_large, 'used_delta': phys, 'ratio': round(ratio, 4)}
            if a.strict_used:
                b5.check(phys >= 1.5 * logical_large, f'large files: used delta {phys} below 1.5x {logical_large}')
                b5.check(ratio <= 1.5 + a.overhead_tol,
                         f'large files: used delta {ratio:.3f}x above 1.5x + {a.overhead_tol}')
            later = {i: views[i]['members'].get(i, {}).get('used') for i in ids}
            b5.info['per_member_used'] = {str(i): {'start': used0[i], 'after_large': used1[i],
                                                   'after_all': used2[i], 'end': later[i]} for i in ids}
        else:
            b5.check(False, 'per-member used bytes missing from status (efs-mgmt without the bytes: line)')
        if inv and set(inv) == set(ids):
            costs = file_costs(rows_by_file, inv)
            lp = sum(costs[nm]['payload'] for nm in large)
            la = sum(costs[nm]['allocated'] for nm in large)
            b5.check(all(costs[nm]['fragments'] == costs[nm]['expected'] for nm in large),
                     'large files: fragment records missing for the cost')
            # Quotas charge fragment payload: exactly 1.5x for whole chunks.
            b5.check(lp == 1.5 * logical_large, f'large files: payload {lp} != 1.5 x {logical_large}')
            b5.check(la >= lp, f'large files: allocated {la} below payload {lp}')
            b5.info['large'] = {'logical': logical_large, 'payload': lp, 'allocated': la,
                                'payload_ratio': round(lp / logical_large, 4),
                                'allocated_ratio': round(la / logical_large, 4)}
            per_size = {}
            for nm, sz in files.items():
                if nm.startswith('b2-') and sz and nm in costs and costs[nm]['fragments']:
                    cst = costs[nm]
                    per_size[str(sz)] = {'payload': round(cst['payload']), 'allocated': round(cst['allocated']),
                                         'ratio': round(cst['payload'] / sz, 2),
                                         'packed': any(r['packed'] for r in rows_by_file[(nm, inos[nm])])}
                    # No false claim: a file never costs less than 1.5x.
                    b5.check(cst['payload'] >= 1.5 * sz - 1e-6 or per_size[str(sz)]['packed'],
                             f'{nm}: payload {cst["payload"]} below 1.5 x {sz}')
            b5.info['b2_cost'] = per_size
            sp = [costs[nm] for nm in small if nm in costs and costs[nm]['fragments']]
            if sp:
                ls = sum(small[nm] for nm in small if nm in costs and costs[nm]['fragments'])
                b5.info['small_packed'] = {'files': len(sp), 'logical': ls,
                                           'payload': round(sum(x['payload'] for x in sp)),
                                           'allocated': round(sum(x['allocated'] for x in sp)),
                                           'payload_ratio': round(sum(x['payload'] for x in sp) / ls, 3)}
        else:
            b5.notrun.append('cost from persisted records (needs --store for every member)')
        b5.notrun += ['full member (changes the cluster)', 'capacity after an addition (changes membership)',
                      'ENOSPC (changes the cluster)']
    finally:
        if not a.keep:
            for p in sorted(tdir.iterdir()):
                try:
                    p.unlink()
                except OSError:
                    pass
            try:
                tdir.rmdir()
            except OSError:
                pass

    for k, it in items.items():
        if k in want:
            result['items'][k] = it.result()
        else:
            result['notrun'].append(k)
    for k, r in result['items'].items():
        result['notrun'] += [f'{k}: {x}' for x in r['notrun']]
    result['verdict'] = 'FAIL' if any(r['verdict'] == 'FAIL' for r in result['items'].values()) else 'PASS'
    result['seconds'] = round(time.time() - t0, 1)
    Path(a.out).write_text(json.dumps(result, indent=1, sort_keys=True) + '\n')
    print(f'b_checks: {result["verdict"]} ({n} members, {result["seconds"]} s) -> {a.out}')
    for k, r in result['items'].items():
        print(f'  {k}: {r["verdict"]} checks={r["checks"]} failures={r["failure_count"]}'
              + (f' notrun={r["notrun"]}' if r['notrun'] else ''))
        for msg in r['failures'][:5]:
            print(f'    {msg}')
    return 0 if result['verdict'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
