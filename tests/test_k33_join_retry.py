#!/usr/bin/env python3
"""K33/W157 (A13): a join interrupted before the membership commits is
retried with the same store and command.

Real efsd daemons on loopback (tests/k33_daemons.py), private stores under
TMPDIR, free ports (never 17xxx), discovery off. The EFS_FAULT_MEMBER_EXIT
hooks end a daemon (status 86) before any member hears of it; the store's
identity then decides (src/server/member_store.c, member_store_open):
1. The seed ends with boot.bin written and identity.bin not
   (before-identity); started again with the same command it is node 1 of a
   new cluster, and two joiners form it.
2. Node 4 through node 2: a new root holding unknown data is refused; one
   holding only the O_DIRECT probe page of a start killed during the probe
   starts (the page is removed) and ends like the seed: its store holds
   boot.bin at counter 0 and no identity.bin, and every member still lists
   3 members.
   Started again with the same --join, it initializes again (a new token),
   joins as member 4 and group 2 moves to {2,3,4}.
3. Node 5 ends with its joining identity persisted, before its first HELLO
   (before-hello); retried, it is member 5.
4. Refusals stay: node 5 stopped with its identity.bin removed (boot
   counter above 0) is refused ("lost identity.bin"); a wiped store at its
   endpoint is refused by the cluster ("can never be a member"); the
   membership stays 5.
"""
import os, re, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, voters_ok  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
work = Path(tempfile.mkdtemp(prefix='efs-k33r-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_MEMBER_EXIT')}
env['EFS_FAULT_GC_DISABLE'] = '1'


def hook(point):
    return {'EFS_FAULT_MEMBER_EXIT': point}


def mdraft(c, i):
    return c.dir / f'n{i}' / 'mdraft'


t0 = time.time()
c = None
try:
    c = Cluster(work, 'retry', [1, 2, 3, 4, 5], env)

    # 1. The seed's initialization interrupted, then the same command.
    c.start(1, extra_env=hook('before-identity'))
    c.wait_log(1, r'EFS_FAULT_MEMBER_EXIT at before-identity')
    assert c.wait_exit(1) == 86
    md = mdraft(c, 1)
    assert (md / 'boot.bin').exists() and not (md / 'identity.bin').exists(), sorted(os.listdir(md))
    c.start(1)
    c.wait_log(1, r'node 1: store .* \(initialized now\), seed')
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_members([1, 2, 3], 3)

    # 2. Node 4's first start killed during the O_DIRECT probe (its page is
    # left in data/), then again before identity.bin: the cluster never
    # hears of it. Unknown data in a new root stays refused.
    (c.dir / 'n4' / 'data').mkdir(parents=True)
    (c.dir / 'n4' / 'data' / 'junk').write_bytes(b'x')
    c.start(4, join=2)
    assert c.wait_exit(4) == 1, c.last_run(4)[-2000:]
    assert 'has fragment data but no root marker' in c.last_run(4), c.last_run(4)[-2000:]
    (c.dir / 'n4' / 'data' / 'junk').unlink()
    (c.dir / 'n4' / 'data' / '.efs-direct-io-probe.999999').write_bytes(b'\0' * 4096)
    c.start(4, join=2, extra_env=hook('before-identity'))
    c.wait_log(4, r'EFS_FAULT_MEMBER_EXIT at before-identity')
    assert c.wait_exit(4) == 86
    md = mdraft(c, 4)
    assert (md / 'boot.bin').exists() and not (md / 'identity.bin').exists(), sorted(os.listdir(md))
    assert not (c.dir / 'n4' / 'data' / '.efs-direct-io-probe.999999').exists()
    time.sleep(1)
    c.wait_members([1, 2, 3], 3)
    ep4 = c.ep(4)
    assert not any(ep4 in c.log(i) for i in (1, 2, 3)), 'a member heard of node 4'
    c.start(4, join=2)
    c.wait_log(4, r'store .*: joining \(no node id yet\), store [0-9a-f]+, incarnation \d+ '
                  r'\(initialized now\)')
    c.wait_log(4, r'Joined cluster [0-9a-f]+ as node 4 .*committed member: 4 members')
    c.wait_log(4, r'raft-host: up raft_id=3 n=4 formed')
    c.wait_members([1, 2, 3, 4], 4)
    c.wait_voters([1, 2, 3, 4], lambda i, v: voters_ok(i, v, 4))

    # 3. Node 5 ends with its joining identity persisted, before any HELLO.
    c.start(5, join=3, extra_env=hook('before-hello'))
    c.wait_log(5, r'EFS_FAULT_MEMBER_EXIT at before-hello')
    assert c.wait_exit(5) == 86
    tok = re.findall(r'joining \(no node id yet\), store ([0-9a-f]+)', c.last_run(5))
    assert tok, c.last_run(5)[-2000:]
    c.wait_members([2, 3, 4], 4)
    c.start(5, join=3)
    c.wait_log(5, r'Joined cluster [0-9a-f]+ as node 5 .*committed member: 5 members')
    assert re.search(r'joining \(no node id yet\), store %s, ' % tok[0], c.last_run(5)), \
        'the retry must keep the persisted token'
    c.wait_log(5, r'raft-host: up raft_id=4 n=5 formed')
    c.wait_members([1, 2, 3, 4, 5], 5)

    # 4. The refusals that protect the cluster stay.
    c.stop(5)
    (mdraft(c, 5) / 'identity.bin').unlink()
    c.start(5, join=3)
    assert c.wait_exit(5) == 1, c.last_run(5)[-2000:]
    assert 'lost identity.bin' in c.last_run(5), c.last_run(5)[-2000:]
    shutil.rmtree(c.dir / 'n5')
    c.start(5, join=3)
    assert c.wait_exit(5) == 1, c.last_run(5)[-2000:]
    assert 'can never be a member' in c.last_run(5), c.last_run(5)[-2000:]
    tab = c.members(2)
    assert sorted(tab) == [1, 2, 3, 4, 5] and tab[5][1] == 'down', tab
finally:
    if c:
        c.stop_all()
    if os.environ.get('EFS_K33_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print('K33 join retry: a leftover O_DIRECT probe page is removed, unknown data refused; seed '
      'and node 4 interrupted before identity.bin (boot counter 0) start again with the same command (node 1, member 4); node 5 interrupted before its first '
      'HELLO retries as member 5; lost identity and a wiped store stay refused: '
      f'PASS ({time.time() - t0:.1f} s)')
