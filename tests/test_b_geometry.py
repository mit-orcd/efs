#!/usr/bin/env python3
"""Alpha 1.0 checklist B1-B5 on real daemons (loopback regression evidence,
not qualification: A4 needs distinct hosts).

Real efsd daemons on loopback (tests/k33_daemons.py), private stores under
TMPDIR, free ports (never 17xxx), discovery off, GC on, the packer on with
a 500 ms minimum age; efs-fuse mounts. scripts/b_checks.py, the driver the
parent session reruns on real hosts, runs against the cluster at every size
with every store readable (--store), so B1 also checks the persisted
fragment records against the chunk rows and pack objects.
1. Growth cluster, three members, `--quota 1G`: driver PASS through mounts
   via nodes 1, 2 and 3.
2. B2 on disk: one 3-chunk file; for its middle chunk the three fragment
   payloads on the stores are D1, D2 and P = D1 ^ D2 and every pair
   reconstructs the file's bytes. Through fresh mounts: a corrupt D1
   still reads exactly (decoded from D2 + P), D1 replaced by another
   chunk's fragment (identity mismatch) too, corrupt D1 and D2 is EIO
   (never fabricated bytes), restored it reads exactly again.
3. Nodes 4..8 join one at a time; at every size the driver passes again
   (three: all three; four: rotation; five to eight: every member every
   role; packed segments over the ring; status/df capacity = the 3-of-N
   bound, rising by 2Q/3 with each 1 GiB addition). Node 8 starts without
   --quota: its automatic quota is the startup headroom budget (20 %
   reserve, at least 1 GiB, 7/8 of the rest).
4. Full member (separate cluster): three members, node 3 with a 16 MiB
   quota. Writes fill it; then a write fails ENOSPC instead of being
   acknowledged on two fragments; status reports 0 free and no degraded
   chunks; df agrees. Node 4 (1 GiB) joins: usable capacity rises, free is
   above 0 again and new writes succeed on three members, none on the full
   node 3, with no degraded acknowledgement; every file reads back.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
EFS_B_PHASES=grow or =full runs one part (negative controls on a base
build); EFS_B_ARTIFACTS=DIR keeps the driver's JSON results there.
"""
import errno, json, os, re, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster as _Cluster, Mount, EFSD, MGMT, HOST, fuse_ok, mgmt as _mgmt, payload  # noqa: E402

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'scripts'))
import b_checks  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('B geometry: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-bgeo-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_MEMBER_EXIT')}
env.update(EFS_PACK_MIN_AGE_MS='500', EFS_PACK_INTERVAL_MS='200')
env.pop('EFS_GC_DISABLE', None)
CS = 128 * 1024
G = 1 << 30
artifacts = Path(os.environ['EFS_B_ARTIFACTS']) if os.environ.get('EFS_B_ARTIFACTS') else None
summary = {}
phases = set((os.environ.get('EFS_B_PHASES') or 'grow,full').split(','))


def mgmt(*args):
    return _mgmt(env, *args)


def wait_mount(m, pattern, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if re.search(pattern, m.logp.read_text(errors='replace')):
            return
        time.sleep(0.1)
    raise AssertionError(f'mount {m.path}: {pattern!r} not seen\n' + m.logp.read_text()[-2000:])


def wait_formed(c, i, count, timeout=90):
    c.wait_log(i, rf'raft-host: (formed: {count} members|up raft_id=\d+ n={count} formed|'
               rf'membership revision \d+: {count} members)', timeout=timeout, run=False)


def wait_served(c, i, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if mgmt('raft-getattr', c.ep(i), '1')[0] == 0:
            return
        time.sleep(0.3)
    raise AssertionError(f'node {i} serves no metadata')


def driver(c, ids, mounts, tag, extra=()):
    out = c.dir / f'b-checks-{tag}.json'
    argv = ['--seed', c.ep(ids[0]), '--out', str(out), '--pack-wait', '90',
            '--expect-members', str(len(ids)), '--mgmt', str(MGMT)]
    for m in mounts:
        argv += ['--mount', str(m.path)]
    for i in ids:
        argv += ['--store', f'{c.node_id(i)}={c.dir / f"n{i}"}']
    rc = b_checks.main(argv + list(extra))
    res = json.loads(out.read_text())
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
        (artifacts / out.name).write_text(out.read_text())
    assert rc == 0 and res['verdict'] == 'PASS', json.dumps(
        {k: v for k, v in res['items'].items() if v['verdict'] != 'PASS'}, indent=1)[:6000]
    for k in ('B1', 'B2', 'B3', 'B5'):
        assert res['items'][k]['verdict'] == 'PASS', (tag, k)
    # Every store was readable here: the inventory is complete.
    assert not [x for x in res['items']['B1']['notrun']], res['items']['B1']['notrun']
    assert res['items']['B1']['inventory_checked'] > 0
    summary[tag] = {'members': len(ids), 'seconds': res['seconds'],
                    'B3_histogram': res['items']['B3']['histogram'],
                    'B4': {k: res['items']['B4'].get(k) for k in
                           ('segments', 'destinations', 'group_voters', 'on_voters_share', 'histogram')},
                    'B5': {k: res['items']['B5'].get(k) for k in ('usable', 'large', 'small_packed', 'b2_cost')},
                    'checks': {k: v['checks'] for k, v in res['items'].items()}}
    return res


def status(c, i):
    rc, out = mgmt('status', c.ep(i))
    return b_checks.parse_status(out), out


def write(path, size, name=None):
    data = payload(name or path.name, size)
    with open(path, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def frag_paths(c, ids, ino, ci, gen):
    """Fragment files of one chunk on the stores: {fi: (node, path)}."""
    out = {}
    for i in ids:
        for r in b_checks.store_inventory(c.dir / f'n{i}'):
            if r['kind'] == 1 and r['object'] == ino and r['ci'] == ci and r['gen'] == gen:
                assert r['fi'] not in out, (r, out)
                out[r['fi']] = (i, Path(r['path']), r)
    return out


def fresh_read(c, node, path_rel, name):
    m = Mount(c, node, name)
    try:
        with open(m.path / path_rel, 'rb') as f:
            return f.read()
    finally:
        m.close()


def grow_phase():
    # ---- 1. Three members ----------------------------------------------
    c = _Cluster(work, 'grow', [1, 2, 3, 4, 5, 6, 7, 8], env)
    clusters.append(c)
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        wait_formed(c, i, 3)
    wait_served(c, 1)
    m1 = Mount(c, 1, 'mnt-1')
    mounts.append(m1)
    wait_mount(m1, r'membership view: revision 1, 3 members')
    extra = [Mount(c, 2, 'mnt-2'), Mount(c, 3, 'mnt-3')]
    mounts.extend(extra)
    driver(c, [1, 2, 3], [m1] + extra, 'n3')
    for m in extra:
        m.close()
        mounts.remove(m)

    # ---- 2. B2 on the stores: pairs, corruption, mismatch --------------
    name = 'b2-corrupt'
    size = 3 * CS + 17
    write(m1.path / name, size)
    data = payload(name, size)
    ino = os.stat(m1.path / name).st_ino
    rows = b_checks.Cluster(str(MGMT), c.ep(1)).chunks(ino, [c.ep(i) for i in (1, 2, 3)])
    row = next(r for r in rows if r['ci'] == 1)
    fr = frag_paths(c, [1, 2, 3], ino, 1, row['obj_gen'])
    assert sorted(fr) == [0, 1, 2], fr
    # Each role on the node the row names (logical node i has id i here).
    assert [c.node_id(fr[fi][0]) for fi in range(3)] == row['nodes'], (fr, row)
    pay = {fi: fr[fi][1].read_bytes()[:fr[fi][2]['plen']] for fi in range(3)}
    half = CS // 2
    chunk = data[CS:2 * CS]
    assert pay[0] == chunk[:half] and pay[1] == chunk[half:], 'D1/D2 are not the halves'
    assert pay[2] == bytes(x ^ y for x, y in zip(pay[0], pay[1])), 'P != D1 ^ D2'
    for a_, b_, miss in ((0, 1, 2), (0, 2, 1), (1, 2, 0)):
        f = dict(pay)
        f[miss] = bytes(x ^ y for x, y in zip(f[a_], f[b_]))
        assert f[0] + f[1] == chunk, f'pair {a_},{b_} does not reconstruct'
    # Last chunk: 17 bytes then zero padding in D1, D2 all zeros.
    last = next(r for r in rows if r['ci'] == 3)
    # A partial chunk is a span over a stub base: its own 2+1 object.
    assert b_checks.stub_base(last) and len(last['span']) == 1, last
    fl = frag_paths(c, [1, 2, 3], ino, 3, b_checks.row_objects(last)[0][0])
    lp = {fi: fl[fi][1].read_bytes()[:fl[fi][2]['plen']] for fi in range(3)}
    assert lp[0] == data[3 * CS:] + bytes(half - 17) and lp[1] == bytes(half) and lp[2] == lp[0], 'last chunk padding'
    rel = name
    orig = {fi: fr[fi][1].read_bytes() for fi in range(3)}

    def flip(fi, at=1000):
        b = bytearray(orig[fi])
        b[at] ^= 0x5a
        fr[fi][1].write_bytes(bytes(b))

    def restore():
        for fi in range(3):
            fr[fi][1].write_bytes(orig[fi])
    try:
        flip(0)
        assert fresh_read(c, 2, rel, 'mnt-c1') == data, 'corrupt D1: not decoded from D2 + P'
        # D1 replaced by chunk 2's D1 (same size, another identity).
        other = frag_paths(c, [1, 2, 3], ino, 2, next(r for r in rows if r['ci'] == 2)['obj_gen'])
        fr[0][1].write_bytes(other[0][1].read_bytes())
        assert fresh_read(c, 3, rel, 'mnt-c2') == data, 'mismatched D1 used or not refused'
        flip(0)
        flip(1)
        try:
            got = fresh_read(c, 2, rel, 'mnt-c3')
            raise AssertionError(f'two corrupt fragments returned {len(got)} bytes (fabricated)')
        except OSError as e:
            assert e.errno == errno.EIO, e
    finally:
        restore()
    assert fresh_read(c, 1, rel, 'mnt-c4') == data, 'restored chunk does not read'
    summary['b2_store'] = 'pairs exact; corrupt D1, mismatched D1 decoded from the others; two corrupt EIO'

    # ---- 3. Growth 4..8 --------------------------------------------------
    caps = {3: status(c, 1)[0]['usable']['capacity']}
    for n in range(4, 9):
        quota = None if n == 8 else '1G'
        c.start(n, join=2 if n % 2 == 0 else 3, quota=quota)
        wait_formed(c, n, n)
        for i in range(1, n):
            wait_formed(c, i, n)
        assert c.node_id(n) == n, c.node_id(n)
        wait_mount(m1, rf'membership view: revision \d+ -> \d+ from node \d+: added member {n} at '
                   rf'{HOST}:{c.ports[n]} \({n} members, no remount\)')
        mn = Mount(c, n, f'mnt-{n}')
        mounts.append(mn)
        try:
            res = driver(c, list(range(1, n + 1)), [m1, mn], f'n{n}')
        finally:
            mn.close()
            mounts.remove(mn)
        st, out = status(c, n)
        caps[n] = st['usable']['capacity']
        mem = st['members']
        if quota:
            assert mem[n]['quota'] == G, mem[n]
            want = b_checks.capacity_logical(b_checks.member_caps(mem[k])[0] for k in sorted(mem))
            assert caps[n] == want and caps[n] > caps[n - 1], (n, caps, want)
            # Equal 1 GiB quotas: +2 GiB/3 per addition (floor of the bound).
            assert abs(caps[n] - caps[n - 1] - 2 * G // 3) <= 2, (n, caps)
        else:
            # Automatic quota: the startup budget on the store's filesystem
            # (20 % reserve, at least 1 GiB, 7/8 of the rest).
            fs = os.statvfs(c.dir / f'n{n}')
            total = fs.f_blocks * fs.f_frsize
            reserve = max(total // 5, G)
            ceiling = total - reserve
            ceiling -= ceiling // 8 + (ceiling % 8 != 0)
            q = mem[n]['quota']
            assert 0 < q <= ceiling, (q, ceiling)
            avail_now = fs.f_bavail * fs.f_frsize
            assert q >= min(ceiling, (avail_now - reserve) * 7 // 8) - (256 << 20), (q, avail_now, reserve)
            summary['auto_quota'] = {'quota': q, 'fs_total': total, 'ceiling': ceiling}
    summary['capacity_by_members'] = caps
    m1.close()
    mounts.remove(m1)
    c.stop_all()



def full_phase():
    # ---- 4. A full member, ENOSPC, capacity after an addition ----------
    f = _Cluster(work, 'full', [1, 2, 3, 4], env)
    clusters.append(f)
    f.start(1)
    f.start(2, join=1)
    f.start(3, join=1, quota='16M')
    for i in (1, 2, 3):
        wait_formed(f, i, 3)
    wait_served(f, 1)
    fm = Mount(f, 1, 'mnt-f')
    mounts.append(fm)
    wait_mount(fm, r'membership view: revision 1, 3 members')
    st, _ = status(f, 1)
    assert st['usable']['capacity'] == 2 * (16 << 20), st['usable']
    written, k, enospc = {}, 0, None
    while k < 200:
        p = fm.path / f'fill-{k}'
        try:
            write(p, 1024 * 1024)
            written[p.name] = 1024 * 1024
        except OSError as e:
            enospc = e
            break
        k += 1
    assert enospc is not None, f'{k} MiB written on a 16 MiB member without ENOSPC'
    assert enospc.errno == errno.ENOSPC, enospc
    # The same write again is ENOSPC too (never a two-fragment success).
    try:
        write(fm.path / 'again', 1024 * 1024)
        raise AssertionError('write after ENOSPC succeeded with a full member at three')
    except OSError as e:
        assert e.errno == errno.ENOSPC, e
    time.sleep(6)
    st, out = status(f, 2)
    assert st['usable']['free'] == 0, out
    assert re.search(r'^Degraded chunks: 0 ', out, re.M), out
    flog = fm.logp.read_text(errors='replace')
    assert 'degraded for rebuild' not in flog, 'a full member caused a degraded acknowledgement'
    df = b_checks.statfs(fm.path)
    assert df['free'] == 0 and df['total'] == st['usable']['capacity'] // 512 * 512, (df, st['usable'])
    summary['full_n3'] = {'files_before_enospc': len(written), 'node3_used': st['members'][3]['used'],
                          'usable': st['usable'], 'df': df}

    f.start(4, join=2)
    for i in (1, 2, 3, 4):
        wait_formed(f, i, 4)
    wait_mount(fm, r'added member 4 at .*\(4 members, no remount\)')
    time.sleep(6)
    st, out = status(f, 1)
    assert st['usable']['capacity'] > 2 * (16 << 20) and st['usable']['free'] > 0, out
    df = b_checks.statfs(fm.path)
    assert df['total'] == st['usable']['capacity'] // 512 * 512, (df, st['usable'])
    after = []
    for k in range(8):
        p = fm.path / f'after-{k}'
        write(p, 1024 * 1024)
        written[p.name] = 1024 * 1024
        after.append(p.name)
    bc = b_checks.Cluster(str(MGMT), f.ep(1))
    eps = [f.ep(i) for i in (1, 2, 3, 4)]
    for name in after:
        for r in bc.chunks(os.stat(fm.path / name).st_ino, eps):
            assert b_checks.stripe_ok(r['nodes'], {1, 2, 3, 4}), r
            assert 3 not in r['nodes'], f'{name}: chunk {r["ci"]} placed on the full node 3: {r["nodes"]}'
    flog = fm.logp.read_text(errors='replace')
    assert 'degraded for rebuild' not in flog, 'degraded acknowledgement with a full member at four'
    st, out = status(f, 3)
    assert re.search(r'^Degraded chunks: 0 ', out, re.M), out
    for name, size in written.items():
        assert (fm.path / name).read_bytes() == payload(name, size), name
    summary['full_n4'] = {'usable': st['usable'], 'files_after': len(after),
                          'rerouted': len(re.findall(r'placing again on', flog))}
    fm.close()
    mounts.remove(fm)
    f.stop_all()


t0 = time.time()
clusters, mounts = [], []
try:
    if 'grow' in phases:
        grow_phase()
    if 'full' in phases:
        full_phase()
    print(json.dumps(summary, indent=1, sort_keys=True, default=str))
    if artifacts:
        (artifacts / 'test_b_geometry-summary.json').write_text(json.dumps(summary, indent=1, sort_keys=True, default=str))
    print(f'B geometry: B1-B5 on real daemons (3..8 members, full member, ENOSPC, addition) PASS '
          f'in {time.time() - t0:.0f} s')
except BaseException:
    for cl in clusters:
        for i in sorted(cl.ports):
            if (cl.dir / f'n{i}.log').exists():
                print(f'--- {cl.dir.name} node {i} log tail', file=sys.stderr)
                print(cl.log(i)[-2500:], file=sys.stderr)
    for m in mounts:
        print(f'--- mount {m.path} log tail', file=sys.stderr)
        print(m.logp.read_text(errors='replace')[-2500:], file=sys.stderr)
    print(f'work tree kept: {work}', file=sys.stderr)
    raise
finally:
    for m in mounts:
        m.close()
    for cl in clusters:
        cl.stop_all()
shutil.rmtree(work, ignore_errors=True)
