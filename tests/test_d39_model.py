#!/usr/bin/env python3
"""D39 consolidated model: counterexample traces, gates and a randomized search.

Every test is named after the review section whose counterexample or gate it
runs (docs/status/d39-cluster-membership-proposal-20261022.md); the rules
are tests/d39_model.py and docs/status/d39-consolidated-model-20261023.md.
Negative controls (`neg_*`) run a trace under a superseded rule set and
require the failure that section's review predicted.

Usage: python3 tests/test_d39_model.py [-k substring] [--seeds N]
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import d39_model as M  # noqa: E402
from d39_model import (CONSOLIDATED, S9_CLIENTS, S9_LOCAL, S9_PAIR, S13, S15, S21, S29, S31, Cluster, ConfigError,  # noqa: E402
                       StartRefused, Tag, Violation)

TESTS = []
SEEDS = 30


def test(fn):
    TESTS.append(fn)
    return fn


# ---------------------------------------------------------------------------
# helpers


def formed(n=3, **kw):
    return Cluster(n=n, **kw).start_all().form()


def expect_violation(name, fn):
    names = name if isinstance(name, tuple) else (name,)
    try:
        fn()
    except Violation as v:
        assert v.name in names, 'expected violation %s, got %s' % (name, v)
        return v
    raise AssertionError('expected violation %s; none raised' % name)


def must(c, until, steps=60000, what=''):
    assert c.run(until=until, max_steps=steps), 'did not reach %s; %s' % (
        what or until, [n.status() for n in c.live()])


def never(c, until, steps=20000, what=''):
    assert not c.run(until=until, max_steps=steps), 'unexpectedly reached %s' % (what or until)


def tag_of(node, **over):
    t = node.tag()
    return Tag(over.get('member', t.member), over.get('inc', t.inc), over.get('counter', t.counter),
               over.get('digest', t.digest), over.get('pair', t.pair))


def conn_from(c, x, y):
    def find():
        return [cn for cn in c.conns.values() if cn.declarer == x and cn.admitter == y and cn.admitted
                and cn.state == 'ready' and not cn.dead and not cn.retired and cn.role == 'peer']
    must(c, find, 20000, 'an admitted connection %s -> %s' % (x, y))
    return find()[-1]


def raft(g, t, **fields):
    return {'g': g, 'm': dict(t=t, **fields)}


def rstate(node, g):
    r = node.raft[g]
    return (r.term, r.vote, r.last_index(), r.commit)


def hold_thread(c, mid, name):
    return c.hold(lambda t: t.node_id == mid and t.name == name)


def hold_frames_from(c, src, dst):
    return c.hold(lambda t: t.node_id == dst and t.name.startswith('admit')
                  and t.peek() is not None and t.peek().src == src)


def refusals(node, text):
    return [r for r in node.refused if text in str(r[2])]


def lock_do(node, fn):
    assert node.lock is None
    node.lock = 'test'
    try:
        return fn()
    finally:
        if node.alive:
            node.lock = None


def timeout(op):
    return lock_do(op.node, lambda: op.node.timeout(op))


def client_done(c, cl, steps=60000):
    must(c, lambda: cl.done, steps, 'client %s done' % cl.id)


def applied_once(c, opid, key):
    g = M.shard_group(key)
    counts = [sum(1 for _, o in n.raft[g].kv.effects if o == opid) for n in c.live() if g in n.raft]
    assert counts and all(k == 1 for k in counts), (opid, counts)


def leader_group_key(c, avoid):
    """A key whose group has a leader that is not `avoid`."""
    for key in (1, 2):
        g = M.shard_group(key)
        ld = c.leader_of(g)
        if ld is not None and ld != avoid:
            return key, g, ld
    raise AssertionError('no group led by a node other than %d' % avoid)


# ---------------------------------------------------------------------------
# C1 / §5 / §6: configuration file and canonical digest


@test
def s5_three_members_no_count():
    cfg = M.parse_config('1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17432\n')
    assert cfg.n == 3
    assert M.group_voters(0, 3) == M.group_voters(2, 3) == {1, 2, 3}
    c = formed(3)
    for n in c.live():
        assert all(set(r.voters) == {1, 2, 3} for r in n.raft.values())


@test
def s5_malformed_input_refused():
    bad = {
        '3junk': '1 10.0.0.1:1\n2 10.0.0.1:2\n3junk 10.0.0.1:3\n',
        'duplicate id': '1 10.0.0.1:1\n2 10.0.0.1:2\n2 10.0.0.1:3\n',
        'duplicate address': '1 10.0.0.1:1\n2 10.0.0.1:2\n3 10.0.0.1:2\n',
        'two members': '1 10.0.0.1:1\n2 10.0.0.1:2\n',
        'seventeen': ''.join('%d 10.0.0.1:%d\n' % (i, i) for i in range(1, 18)),
        'gap in ids': '1 10.0.0.1:1\n2 10.0.0.1:2\n4 10.0.0.1:3\n',
        'wildcard': '1 0.0.0.0:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
        'host name': '1 node1:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
        'leading zero': '1 10.0.0.01:1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
        'no port': '1 10.0.0.1\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
        'port zero': '1 10.0.0.1:0\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
        'extra field': '1 10.0.0.1:1 x\n2 10.0.0.1:2\n3 10.0.0.1:3\n',
    }
    for why, text in bad.items():
        try:
            M.parse_config(text)
        except ConfigError:
            continue
        raise AssertionError('%s accepted' % why)
    assert M.parse_config(''.join('%d 10.0.0.1:%d\n' % (i, i) for i in range(1, 17))).n == 16


@test
def s6_canonical_digest():
    a = M.parse_config('1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17432\n')
    b = M.parse_config('# comment\n3   10.0.0.2:17432  # third\n\n1 10.0.0.1:17432\n2\t10.0.0.1:17433\n')
    assert a == b and a.digest() == b.digest(), 'whitespace, comments and line order must not matter'
    c = M.parse_config('1 10.0.0.1:17432\n2 10.0.0.1:17433\n3 10.0.0.2:17433\n')
    assert c.digest() != a.digest()
    assert a.canonical().startswith(b'efs-cluster-config 1\n')
    # identical lists, independent clusters: equal digests, different pairs (the salt).
    c1, c2 = formed(3, seed=1), formed(3, seed=2)
    p1, p2 = c1.nodes[1].pair, c2.nodes[1].pair
    assert p1[1] == p2[1] and p1[0] != p2[0] and p1 != p2


@test
def s5_different_lists_cannot_form():
    conf4 = M.default_conf(4)
    c = Cluster(n=3, node_conf={3: conf4}).start_all()
    c.add_mgmt(1)
    never(c, lambda: c.nodes[1].pair is not None, 6000, 'formation with a differing list')
    msgs = [r for r in c.nodes[1].refused if 'digest' in str(r[2])]
    assert msgs and c.nodes[3].digest[:12] in msgs[0][2] and c.nodes[1].digest[:12] in msgs[0][2], msgs
    assert any('digest' in str(r[2]) for r in c.nodes[3].refused)
    mg = c.actors[0]
    assert any(isinstance(e, dict) and e.get('reason') in ('members absent', 'not leader') for e in mg.log)
    for m in (1, 2, 3):
        assert c.stores[m].formation_pair is None and c.stores[m].formation_state == 'unformed'


@test
def d40_mkfs_requires_every_member():
    c = Cluster(n=3)
    c.start(1), c.start(2)
    mg = c.add_mgmt(1)
    never(c, lambda: mg.done, 4000)
    absent = [e for e in mg.log if isinstance(e, dict) and e.get('reason') == 'members absent']
    assert absent and absent[-1]['absent'] == [3], mg.log[-3:]
    c.start(3)
    must(c, c.all_formed)


@test
def s5_absent_member_after_formation_serves_and_never_shrinks():
    c = formed(4)
    c.crash(4)
    cl = c.add_client(1, [('APP', 'a1', 1, 'x'), ('APP', 'a2', 2, 'y')])
    client_done(c, cl)
    st = c.nodes[1].status()
    assert 4 in st['absent'] and st['voters'] == {0: [1, 2, 3], 2: [2, 3, 4]}
    for n in c.live():
        for g, r in n.raft.items():
            assert r.voters == M.group_voters(g, 4)


@test
def s5_lose_each_node_in_turn():
    c = formed(3)
    for k in (1, 2, 3):
        c.crash(k)
        t = 1 if k != 1 else 2
        cl = c.add_client(t, [('APP', 'lose%d' % k, k, 'v')])
        client_done(c, cl)
        must(c, lambda: c.leader_of(0) is not None and c.leader_of(2) is not None)
        c.start(k)
        must(c, lambda: c.converged() and c.all_formed())
    for k in (1, 2, 3):
        applied_once(c, 'lose%d' % k, k)


# ---------------------------------------------------------------------------
# C2 / §18 §20 §22: store ownership and the durable boot counter


@test
def s22_first_formation_of_a_fresh_store():
    s = M.Store(1)
    assert M.start_process(s, 'p1') == 1 and s.initialized and s.owner == 'p1'
    assert M.boot_decode(s.boot) == 1


@test
def s22_damaged_counter_in_initialized_store_refused():
    for damage in ('short', ('EFSBOOT', 1, 5, 'bad'), ('XX', 1, 5, M._boot_sum(5)), None):
        s = M.Store(1)
        M.start_process(s, 'p1')
        s.owner = None
        s.boot = damage
        try:
            M.start_process(s, 'p2')
        except StartRefused:
            assert s.owner is None and s.boot == damage
            continue
        raise AssertionError('damaged counter %r accepted' % (damage,))


@test
def c2_uninitialized_store_with_counter_refused():
    s = M.Store(1)
    s.boot = M.boot_encode(3)
    try:
        M.start_process(s, 'p1')
    except StartRefused:
        return
    raise AssertionError('uninitialized store with a boot counter accepted')


@test
def s20_maximum_counter_refused():
    s = M.Store(1)
    M.start_process(s, 'p1')
    s.owner, s.boot = None, M.boot_encode(M.COUNTER_MAX - 1)
    assert M.start_process(s, 'p2') == M.COUNTER_MAX
    s.owner = None
    try:
        M.start_process(s, 'p3')
    except StartRefused as e:
        assert 'exhausted' in str(e) and M.boot_decode(s.boot) == M.COUNTER_MAX
        return
    raise AssertionError('exhausted counter accepted')


@test
def s18_persist_failure_fails_closed():
    s = M.Store(1)
    M.start_process(s, 'p1')
    s.owner, s.fail_fsync = None, True
    try:
        M.start_process(s, 'p2')
    except StartRefused:
        assert s.owner is None and M.boot_decode(s.boot) == 1
        return
    raise AssertionError('persist failure did not stop startup')


@test
def s18_single_owner_and_monotonic_incarnation():
    c = formed(3)
    old = c.nodes[2].inc
    try:
        M.start_process(c.stores[2], 'second-copy')
    except StartRefused as e:
        assert 'owned' in str(e)
    else:
        raise AssertionError('two live processes on one store')
    n2 = c.restart(2)
    assert n2.inc == old + 1
    s = M.Store(9)
    M.start_process(s, 'a', fresh=50)
    s.owner = None
    assert M.start_process(s, 'b', fresh=10) == 51   # max(fresh, saved + 1)


@test
def s18_old_live_process_cannot_reclaim_identity():
    """A delayed declaration from an older incarnation is stale (C2, C5 step 4)."""
    c = formed(3)
    n1 = c.nodes[1]
    old = tag_of(c.nodes[3])
    c.restart(3)
    must(c, lambda: n1.bindings[3].inc == c.nodes[3].inc)
    cn = conn_from(c, 3, 1)
    cn.fwd.append(M.Frame('DECL', old, dict(role='peer', member=3, endpoint=c.cfg.endpoint(3), digest=c.digest,
                                            inc=old.inc, counter=old.counter + 5, pair=old.pair), 3, 1))
    must(c, lambda: refusals(n1, 'stale incarnation'))
    assert n1.bindings[3].inc == c.nodes[3].inc


# ---------------------------------------------------------------------------
# C5 / §18 §19 §20 §21: the ordered declaration acceptance predicate R2''


def predicate_node(local_pair=None, binding=None):
    c = Cluster(n=3)
    n = c.start(1)
    n.pair = local_pair
    if binding is not None:
        n.bindings[2] = M.Binding(*binding)
    return c, n


def r2(n, inc, counter, pair, member=2, endpoint=None, digest=None):
    return n.declaration_predicate(member, endpoint or n.cfg.endpoint(member), digest or n.digest,
                                   inc, counter, pair)


P, Q = ('salt-P', 'd'), ('salt-Q', 'd')


@test
def s19_newer_incarnation_with_conflicting_pair_refused_at_step3():
    _, n = predicate_node(local_pair=P, binding=(10, 2, P))
    v, why = r2(n, 11, 0, Q)
    assert v == 'refuse' and 'conflict' in why
    _, n = predicate_node(local_pair=None, binding=(10, 2, P))
    v, why = r2(n, 11, 0, Q)
    assert v == 'refuse' and 'once bound' in why


@test
def s19_equal_counter_different_pair_refused_at_step4():
    _, n = predicate_node(local_pair=None, binding=(10, 2, None))
    v, why = r2(n, 10, 2, P)
    assert v == 'refuse' and 'equal counter' in why


@test
def s19_unknown_to_known_at_higher_counter_accepted():
    _, n = predicate_node(local_pair=None, binding=(10, 2, None))
    assert r2(n, 10, 3, P) == ('accept', None)


@test
def s19_identical_redeclaration_is_idempotent():
    c, n = predicate_node(local_pair=P, binding=(10, 2, P))
    assert r2(n, 10, 2, P) == ('idempotent', None)
    assert r2(n, 10, 1, P)[0] == 'refuse' and r2(n, 9, 7, P)[0] == 'refuse'


@test
def s20_restart_after_install_newer_incarnation_accepted():
    """A saw B unknown at incarnation 10; B installed P and restarted; B declares P at 11."""
    _, n = predicate_node(local_pair=None, binding=(10, 4, None))
    assert r2(n, 11, 0, P) == ('accept', None)
    _, n = predicate_node(local_pair=P, binding=(10, 4, None))
    assert r2(n, 11, 0, P) == ('accept', None)


@test
def s20_lagging_peer_is_not_a_downgrade():
    _, n = predicate_node(local_pair=P, binding=None)
    assert r2(n, 3, 0, None) == ('accept', None)          # formed local, unknown remote
    _, n = predicate_node(local_pair=None, binding=None)
    assert r2(n, 3, 0, P) == ('accept', None)             # formed remote, unknown local
    assert M.channel(None, P) == M.channel(P, None) == 'catchup'


@test
def s20_genuine_downgrade_refused():
    _, n = predicate_node(local_pair=P, binding=(10, 4, P))
    for inc, counter in ((10, 5), (11, 0), (12, 9)):
        v, why = r2(n, inc, counter, None)
        assert v == 'refuse' and 'once bound' in why


@test
def s12_unconfigured_member_or_endpoint_refused():
    _, n = predicate_node()
    assert r2(n, 1, 0, None, member=9)[0] == 'refuse'
    v, why = r2(n, 1, 0, None, endpoint='10.9.9.9:1')
    assert v == 'refuse' and 'configured record' in why
    v, why = r2(n, 1, 0, None, digest='0' * 64)
    assert v == 'refuse' and 'digest' in why


@test
def s16_reordered_declarations_cannot_lower_the_mark():
    c = formed(3)
    n1 = c.nodes[1]
    t = tag_of(c.nodes[2])
    cn = conn_from(c, 2, 1)

    def decl(counter):
        cn.fwd.append(M.Frame('DECL', t, dict(role='peer', member=2, endpoint=c.cfg.endpoint(2),
                                              digest=c.digest, inc=t.inc, counter=counter, pair=t.pair), 2, 1))
    decl(t.counter + 3)   # C
    decl(t.counter + 2)   # B, reordered after C
    decl(t.counter + 3)   # C again (duplicate)
    must(c, lambda: refusals(n1, 'stale counter'))
    c.quiesce()
    assert n1.bindings[2].counter == t.counter + 3


# ---------------------------------------------------------------------------
# C4 / §12 §13 §15: the connection matrix through the declaration entry points


def declare(c, frm, to):
    """Open one peer connection frm -> to through the pool entry point and run its declaration."""
    a, b = c.nodes[frm], c.nodes[to]
    cn = c.connect(a, c.cfg.endpoint(to), 'peer', intended=to)
    t = a.tag()
    cn.fwd.append(M.Frame('DECL', t, dict(role='peer', member=frm, endpoint=a.endpoint, digest=a.digest,
                                          inc=t.inc, counter=t.counter, pair=t.pair), frm, to))
    must(c, lambda: cn.state != 'declaring' or cn.dead, 4000)
    return cn


@test
def s13_matrix_cells_through_entry_points():
    c = Cluster(n=3).start_all()
    c.quiesce()
    n1, n2 = c.nodes[1], c.nodes[2]
    cn = declare(c, 1, 2)                                   # none -> none
    assert cn.state == 'ready' and cn.admitted
    lock_do(n2, lambda: n2.identity_event(P, 'test'))       # none -> P
    cn = declare(c, 1, 2)
    assert cn.state == 'ready', cn.refusal
    cn = declare(c, 2, 1)                                   # P -> none
    assert cn.state == 'ready', cn.refusal
    lock_do(n1, lambda: n1.identity_event(P, 'test'))       # P -> P
    cn = declare(c, 1, 2)
    assert cn.state == 'ready'
    n3 = c.nodes[3]
    lock_do(n3, lambda: n3.identity_event(Q, 'test'))       # Q -> P refused, both identities
    cn = declare(c, 3, 1)
    assert cn.state == 'refused' and 'salt-Q' in cn.refusal and 'salt-P' in cn.refusal
    assert c.stores[1].formation_pair == P and c.stores[3].formation_pair == Q


@test
def s13_formation_read_answers():
    c = Cluster(n=3).start_all()
    must(c, lambda: c.leader_of(0) is not None)
    ld = c.leader_of(0)
    mg = c.add_mgmt(ld, cmd='FORMATION_READ', args=dict(g=0))
    must(c, lambda: mg.done or len(mg.log) > 3)
    rep = [e for e in mg.log if isinstance(e, dict) and e.get('ok')]
    assert rep and rep[0]['pair'] is None, mg.log            # unknown before formation
    c.form()
    mg = c.add_mgmt(c.leader_of(0), cmd='FORMATION_READ', args=dict(g=0))
    must(c, lambda: mg.done)
    assert mg.result['pair'] == c.nodes[1].pair and mg.result['index'] > 0


@test
def s12_formed_leader_new_connection_to_lagging_voter():
    c = Cluster(n=3).start_all()
    hold = hold_thread(c, 3, 'pump')
    c.add_mgmt(1)
    must(c, lambda: c.nodes[1].complete and c.nodes[2].complete)
    ld = c.leader_of(0)
    assert c.nodes[3].pair is None
    cn = declare(c, ld, 3)                                  # P -> none: a formed leader to a lagging voter
    assert cn.state == 'ready' and cn.admitted
    c.release(hold)
    must(c, c.all_formed)


@test
def s12_absent_member_without_record_discovers_pair():
    c = Cluster(n=5).start_all()
    hold = c.hold(lambda t: t.node_id == 5 and t.name == 'formation')
    c.add_mgmt(1)
    must(c, lambda: all(c.nodes[m].complete for m in (1, 2, 3, 4)))
    pair = c.nodes[1].pair
    c.crash(5)                                              # absent before learning the pair
    c.release(hold)
    n5 = c.start(5)
    assert n5.pair is None and n5.state == 'unformed' and c.stores[5].formation_pair is None
    must(c, lambda: c.nodes[5].complete)
    assert c.nodes[5].pair == pair and c.stores[5].formation_pair == pair
    assert any(e[1] == 'identity' and e[2] == 5 and 'FORMATION_READ' in e[3] for e in c.monitor.events)


@test
def c13_replaced_store_is_a_downgrade_while_peers_remember():
    """Unresolved in the proposal (C13): a member whose store lost its record declares none;
    peers that bound it to the pair refuse that as a downgrade until they restart."""
    c = formed(5)
    c.crash(5)
    c.stores[5] = M.Store(5)
    c.start(5)
    c.ticks(12)
    assert not c.nodes[5].complete and c.stores[5].formation_pair is None
    assert any('may not redeclare None' in str(r[2]) for r in c.nodes[5].refused)


@test
def s12_reconnect_both_directions_during_transition():
    c = Cluster(n=4).start_all()
    c.add_mgmt(1)
    for _ in range(12):
        c.run(max_steps=150)
        for cn in list(c.conns.values()):
            if cn.role == 'peer' and not cn.dead and (cn.id % 3 == 0):
                c.close(cn)
    must(c, c.all_formed)


@test
def s11_self_asserted_identity_is_consistency_only():
    c = formed(3)
    n1 = c.nodes[1]
    must(c, lambda: n1.bindings[2].tup() == c.nodes[2].tag().tup())

    class Fake:
        id, alive = 'fake', True
    cn = c.connect(Fake, c.cfg.endpoint(1), 'peer', intended=1)
    cn.fwd.append(M.Frame('DECL', None, dict(role='peer', member=2, endpoint='10.0.0.99:1', digest=c.digest,
                                             inc=1, counter=0, pair=None), 'fake', 1))
    must(c, lambda: cn.rev)
    assert not cn.rev[0].body['ok'] and 'configured record' in cn.rev[0].body['reason']
    # A matching self-asserted endpoint passes the consistency check: impersonation is not
    # prevented (trusted network, C4). A stale tuple is refused; a fresher one would be accepted.
    cn = c.connect(Fake, c.cfg.endpoint(1), 'peer', intended=1)
    cn.fwd.append(M.Frame('DECL', None, dict(role='peer', member=2, endpoint=c.cfg.endpoint(2), digest=c.digest,
                                             inc=1, counter=0, pair=n1.pair), 'fake', 1))
    must(c, lambda: cn.rev)
    assert not cn.rev[0].body['ok'] and 'stale' in cn.rev[0].body['reason']
    cn = c.connect(Fake, c.cfg.endpoint(1), 'peer', intended=1)
    cn.fwd.append(M.Frame('DECL', None, dict(role='peer', member=2, endpoint=c.cfg.endpoint(2), digest=c.digest,
                                             inc=99, counter=0, pair=n1.pair), 'fake', 1))
    must(c, lambda: cn.rev)
    assert cn.rev[0].body['ok'] and n1.bindings[2].inc == 99


# ---------------------------------------------------------------------------
# C3 / C10 / §8 §10 §11 §12: formation across both groups


@test
def s8_independent_local_salts():
    c = Cluster(n=4).start_all()
    salts = {n.salt for n in c.live()}
    assert len(salts) == 4
    c.form()
    pair = c.nodes[1].pair
    assert pair[0] in salts and all(n.pair == pair for n in c.live())
    assert all(n.raft[g].kv.pair == pair for n in c.live() for g in n.hosted)


@test
def s8_concurrent_and_retried_mkfs_keep_first_pair():
    c = Cluster(n=3).start_all()
    a, b = c.add_mgmt(1), c.add_mgmt(2)
    must(c, lambda: c.all_formed() and a.done and b.done)
    pair = c.nodes[1].pair
    assert a.result['pair'] == b.result['pair'] == pair
    ld = c.leader_of(0)
    lock_do(c.nodes[ld], lambda: c.nodes[ld].raft[0].propose(('MKFS', ('other', c.digest))))
    r = c.add_mgmt(3)
    must(c, lambda: r.done and c.converged())
    assert r.result['pair'] == pair and all(n.pair == pair for n in c.live())
    assert all(n.raft[0].kv.pair == pair for n in c.live())


@test
def s8_restart_between_group0_commit_and_group2_install():
    c = Cluster(n=4).start_all()
    c.pause_salt = True
    c.add_mgmt(1)
    must(c, lambda: all(c.nodes[m].known[0] for m in (1, 2, 3)))
    assert not any(n.complete for n in c.live())
    for m in (1, 2, 3, 4):
        c.restart(m)
    c.quiesce()
    assert all(n.state == 'forming' for n in c.live() if n.id != 4) and not any(n.complete for n in c.live())
    c.pause_salt = False
    must(c, c.all_formed)


@test
def s8_preformation_connections_cross_into_formed():
    c = Cluster(n=3).start_all()
    c.quiesce()
    early = [cn for cn in c.conns.values() if cn.admitted and cn.role == 'peer']
    assert early and all(cn.bound is None for cn in early)
    seq = c.seq
    c.form()
    must(c, c.converged)
    # Connections admitted before formation carried control frames after the counter moved
    # (C7 (b) counter exception); none was closed by entering formed (C3).
    old_counter = [a for a in c.monitor.acquired if a[0] > seq and a[2] == 'control' and a[3] is not None
                   and a[3].counter < c.monitor.hwm[(a[1], a[3].member)].counter]
    assert old_counter, 'no older-counter control frame was exercised'
    assert not any(cn.dead for cn in early)


@test
def s8_mismatched_persisted_salt_with_matching_configuration():
    c = formed(3)
    other = formed(3, seed=99)
    q = other.nodes[3].pair
    assert q[1] == c.digest and q != c.nodes[1].pair
    c.crash(3)
    c.stores[3] = other.stores[3]
    other.stores[3].owner = None
    other.crash(3)
    before = (c.stores[3].formation_pair, c.stores[3].kv[0].pair)
    c.start(3)
    c.ticks(8)
    assert refusals(c.nodes[1], 'conflicts with local pair') or refusals(c.nodes[2], 'conflicts with local pair')
    msg = (refusals(c.nodes[1], 'conflicts') + refusals(c.nodes[2], 'conflicts'))[0][2]
    assert q[0] in msg and c.nodes[1].pair[0] in msg
    assert (c.stores[3].formation_pair, c.stores[3].kv[0].pair) == before
    assert c.nodes[1].pair == c.nodes[2].pair != q


@test
def s8_data_only_node_readiness_during_partial_formation():
    c = Cluster(n=5).start_all()
    c.pause_salt = True
    c.add_mgmt(1)
    must(c, lambda: c.nodes[5].known[0] is not None)
    c.ticks(6)
    n5 = c.nodes[5]
    assert n5.state == 'forming' and not n5.complete and n5.known[2] is None
    cl = c.add_client(5, [('APP', 'early', 1, 'x')])
    c.ticks(6)
    assert ('refused', 'not formed') in cl.log
    c.pause_salt = False
    must(c, c.all_formed)
    client_done(c, cl)


@test
def s8_declared_client_attempts_peer_traffic():
    c = formed(3)
    n1 = c.nodes[1]
    cl = c.add_client(1, [])
    cn = c.connect(cl, c.cfg.endpoint(1), 'client')
    cn.fwd.append(M.Frame('DECL', None, dict(role='client'), cl.id))
    must(c, lambda: cn.rev)
    assert cn.rev.popleft().body['ok']
    before = rstate(n1, 0)
    cn.fwd.append(M.Frame('CLI_REQ', None, dict(op='RAFT', ref=1, args={}), cl.id))
    cn.fwd.append(M.Frame('CLI_REQ', None, dict(op='FWD_REQ', ref=2, args={}), cl.id))
    must(c, lambda: len(cn.rev) >= 2)
    assert all('allowlist' in f.body['result'] for f in cn.rev)
    assert rstate(n1, 0)[0] == before[0]


@test
def s8_readiness_facts_are_separate_and_the_write_decides():
    c = formed(3)
    st = c.nodes[1].status()
    assert {'metadata_quorum', 'observed_health', 'last_write'} <= set(st)
    assert all(st['observed_health'].values()) and st['last_write'] is None
    c.io_policy[41] = 'fail'
    cl = c.add_client(1, [('PUT', 'w1', 41)])
    c.ticks(4)
    assert c.nodes[1].status()['observed_health'] == st['observed_health']   # health unchanged ...
    assert c.nodes[1].last_write == ('error', 41) and 'w1' not in cl.results  # ... the write failed
    c.io_policy[41] = 'ok'
    client_done(c, cl)
    assert c.nodes[1].last_write == ('ack', 41)


@test
def s10_lagging_voter_catches_up_without_service():
    c = Cluster(n=3).start_all()
    hold = hold_thread(c, 3, 'pump')
    c.add_mgmt(1)
    must(c, lambda: c.nodes[1].complete and c.nodes[2].complete)
    pair = c.nodes[1].pair
    c.restart(c.leader_of(0))     # a new leader starts with next[3] beyond node 3's log
    must(c, lambda: c.nodes[1].complete and c.nodes[2].complete and c.leader_of(0) and c.leader_of(2))
    cl = c.add_client(3, [('APP', 'lag', 1, 'x')])
    c.ticks(4)
    assert ('refused', 'not formed') in cl.log and not c.nodes[3].complete
    c.release(hold)
    must(c, c.all_formed)
    assert c.nodes[3].pair == pair and c.stores[3].formation_pair == pair
    client_done(c, cl)


@test
def neg_s10_lagging_voter_stranded_under_s9():
    """§9 formed admission: the first formed node refuses pair=none, so voters that apply
    later can no longer send their acknowledgements and never form."""
    c = Cluster(n=3, rules=S9_PAIR).start_all()
    hold = hold_thread(c, 3, 'pump')
    c.add_mgmt(1)
    must(c, lambda: any(n.complete for n in c.live()))
    c.release(hold)
    never(c, c.all_formed, 30000, 'formation of the lagging voters under §9')
    assert any(refusals(n, 'formed admitter requires the pair') for n in c.live())
    assert not c.nodes[3].complete


@test
def s10_group2_paused_node1_stays_forming():
    c = Cluster(n=4).start_all()
    c.pause_salt = True
    c.add_mgmt(1)
    must(c, lambda: c.nodes[1].known[0] is not None)
    c.ticks(6)
    n1 = c.nodes[1]
    assert n1.state == 'forming' and not n1.complete
    cl = c.add_client(1, [('APP', 'p1', 1, 'x')])
    c.ticks(6)
    assert ('refused', 'not formed') in cl.log
    c.pause_salt = False
    must(c, c.all_formed)
    client_done(c, cl)


@test
def neg_s10_node1_premature_service_under_s9():
    c = Cluster(n=4, rules=S9_LOCAL).start_all()
    c.pause_salt = True
    c.add_mgmt(1)
    must(c, lambda: c.nodes[1].known[0] is not None)
    c.add_client(1, [('APP', 'p1', 1, 'x')])
    expect_violation('premature_application', lambda: c.ticks(8))


@test
def s10_group2_paused_with_restarts_and_a_node_hosting_neither():
    c = Cluster(n=5).start_all()
    c.pause_salt = True
    c.add_mgmt(1)
    must(c, lambda: c.nodes[1].known[0] is not None and c.nodes[5].known[0] is not None)
    c.restart(1)
    c.restart(5)
    c.ticks(8)
    assert not c.nodes[1].complete and not c.nodes[5].complete
    assert c.nodes[1].state == 'forming' and c.nodes[5].state == 'forming'
    c.pause_salt = False
    must(c, c.all_formed)


@test
def s10_mgmt_works_unformed_clients_and_peer_rpcs_refused():
    c = Cluster(n=3).start_all()
    c.quiesce()
    st = c.add_mgmt(1, cmd='STATUS')
    must(c, lambda: st.done)
    assert st.result['status']['state'] == 'unformed'
    cl = c.add_client(2, [('APP', 'u1', 1, 'x')])
    c.ticks(4)
    assert ('refused', 'not formed') in cl.log
    c.form()
    client_done(c, cl)


@test
def neg_s10_mgmt_refused_before_formation_under_s9():
    c = Cluster(n=3, rules=S9_CLIENTS).start_all()
    mg = c.add_mgmt(1)
    never(c, lambda: c.nodes[1].pair is not None, 8000, 'formation without a mgmt role')
    assert ('refused', 'not formed') in mg.log


@test
def s12_cold_restart_between_mkfs_and_salt():
    c = Cluster(n=3).start_all()
    c.pause_salt = True
    c.add_mgmt(1)
    must(c, lambda: all(n.known[0] for n in c.live()))
    for m in (1, 2, 3):
        c.crash(m)
    for m in (1, 2, 3):
        c.start(m)
    c.add_client(1, [('APP', 'cold', 1, 'x')])
    c.ticks(10)
    assert c.leader_of(0) is not None and c.leader_of(2) is not None   # elections among forming voters
    assert not any(n.complete for n in c.live())
    c.pause_salt = False
    must(c, c.all_formed)
    for n in c.live():
        assert all(r.voters == M.group_voters(g, 3) for g, r in n.raft.items())


@test
def s12_election_with_freshest_logs_on_forming_replicas():
    c = Cluster(n=4).start_all()
    c.pause_salt = True
    c.add_mgmt(1)
    must(c, lambda: all(c.nodes[m].known[0] for m in (1, 2, 3)))
    old = c.leader_of(0)
    term = c.nodes[old].raft[0].term
    c.crash(old)
    must(c, lambda: c.leader_of(0) not in (None, old))
    new = c.nodes[c.leader_of(0)]
    assert new.raft[0].term > term and new.state == 'forming' and not new.complete
    c.start(old)
    c.pause_salt = False
    must(c, c.all_formed)


@test
def s12_mgmt_allowlist():
    c = formed(3)
    ch = c.add_mgmt(1, cmd='RAFT_CHANGE')
    uk = c.add_mgmt(1, cmd='FOO')
    must(c, lambda: ch.done and uk.done)
    assert 'not permitted' in ch.result['reason'] and 'not permitted' in uk.result['reason']
    ld = c.leader_of(0)
    sd = c.add_mgmt(ld, cmd='RAFT_STEP_DOWN', args=dict(g=0))
    must(c, lambda: sd.done)
    must(c, lambda: c.leader_of(0) not in (None, ld))


@test
def c10_formed_restart_verifies_before_service():
    c = formed(5)
    hold = c.hold(lambda t: t.node_id == 5 and t.name == 'formation')
    c.restart(5)
    n5 = c.nodes[5]
    assert n5.state == 'formed' and not n5.complete      # persisted claim, not yet verified
    cl = c.add_client(5, [('APP', 'r5', 1, 'x')])
    c.ticks(4)
    assert ('refused', 'not formed') in cl.log
    c.release(hold)
    must(c, lambda: c.nodes[5].complete)
    client_done(c, cl)


# ---------------------------------------------------------------------------
# C6 / C7 / §14 §16 §17: identity on existing connections, queued work, replies


def s14_setup(rules):
    c = Cluster(n=3, rules=rules).start_all()
    hold = hold_thread(c, 3, 'pump')
    c.add_mgmt(1)
    must(c, lambda: c.nodes[1].complete)
    n1, n3 = c.nodes[1], c.nodes[3]
    cn = conn_from(c, 3, 1)
    assert n1.bindings[3].pair is None and n1.pair is not None    # none -> P connection
    return c, n1, n3, cn, hold


def inject_conflicting_vote(c, n1, n3, cn):
    lock_do(n3, lambda: n3.identity_event(Q, 'conflicting install (test)'))
    term = n1.raft[0].term + 50
    cn.fwd.append(M.Frame('RAFT', tag_of(n3), raft(0, 'VOTE_REQ', term=term, lli=10 ** 6, llt=10 ** 6), 3, 1))
    return term


@test
def s14_none_to_P_connection_while_sender_installs_Q():
    c, n1, n3, cn, _ = s14_setup(CONSOLIDATED)
    term = inject_conflicting_vote(c, n1, n3, cn)
    c.ticks(3)
    assert n1.raft[0].term < term and refusals(n1, 'pair conflict')
    assert any('conflicts with local pair' in str(r[2]) for r in n3.refused)  # its redeclaration refused
    assert c.stores[1].formation_pair == n1.pair


@test
def neg_s14_static_connection_permissions_under_s13():
    c, n1, n3, cn, _ = s14_setup(S13)
    inject_conflicting_vote(c, n1, n3, cn)
    expect_violation('conflicting_identity', lambda: c.ticks(3))


def s16_setup(rules):
    c = Cluster(n=3, rules=rules).start_all()
    hold = hold_thread(c, 3, 'pump')
    c.add_mgmt(1)
    must(c, lambda: c.nodes[2].complete)
    n2, n3 = c.nodes[2], c.nodes[3]
    c1 = conn_from(c, 3, 2)                                  # C1: declared with pair none
    assert c1.bound is None or c1.bound[2] is None
    old = tag_of(n3)
    gate = c.hold(lambda t: t.name == 'admit%d' % c1.id)
    lock_do(n3, lambda: n3.identity_event(n2.pair, 'pair learned (test)'))   # C2 declares P
    c1.fwd.append(M.Frame('RAFT', old, raft(2, 'VOTE_REQ', term=n2.raft[2].term + 50, lli=10 ** 6,
                                            llt=10 ** 6), 3, 2))
    must(c, lambda: n2.bindings[3].pair == n2.pair or c.rules.binding == 'connection'
         and c.monitor.hwm[(2, 3)].pair == n2.pair)
    c.release(gate)
    return c, n2, old


@test
def s16_old_connection_none_frames_refused_after_P():
    """§14: receiver learns P after none->none; §16: C1 at none while C2 declares P."""
    c, n2, old = s16_setup(CONSOLIDATED)
    c.ticks(3)
    assert n2.raft[2].term < old.inc + 10 ** 3 and refusals(n2, 'downgrade')


@test
def neg_s16_per_connection_generation_under_s15():
    c, n2, old = s16_setup(S15)
    expect_violation('obsolete_authority', lambda: c.ticks(3))


def s16_inbox_setup(rules):
    """A frame passes dispatch while the receiver is unknown and waits in the queue."""
    c = Cluster(n=3, rules=rules).start_all()
    hold = hold_thread(c, 3, 'pump')
    c.add_mgmt(1)
    must(c, lambda: c.nodes[1].complete and any(it.kind == 'RAFT' and it.frame.tag.pair is not None
                                                 for it in c.nodes[3].inbox))
    n3 = c.nodes[3]
    lock_do(n3, lambda: n3.identity_event(Q, 'conflicting install (test)'))
    return c, n3, hold


@test
def s16_frame_paused_in_inbox_across_install_refused_at_effect():
    c, n3, hold = s16_inbox_setup(CONSOLIDATED)
    c.release(hold)
    c.ticks(2)
    assert refusals(n3, 'pair conflict')


@test
def neg_s16_check_at_dispatch_under_s15():
    c, n3, hold = s16_inbox_setup(S15)
    c.release(hold)
    expect_violation(('conflicting_identity', 'obsolete_authority'), lambda: c.ticks(2))


def fwd_frame(tag, ref=777000, key=1, opid='inj'):
    return M.Frame('FWD_REQ', tag, dict(ref=ref, args=dict(opid=opid, key=key, val='v', op='APP'), op='APP'),
                   tag.member, None)


@test
def s14_both_know_P_group2_paused_application_refused():
    c = Cluster(n=4).start_all()
    c.pause_salt = True
    c.add_mgmt(1)
    must(c, lambda: all(c.nodes[m].known[0] for m in (2, 3)))
    n2, n3 = c.nodes[2], c.nodes[3]
    must(c, lambda: n3.bindings.get(2) and n3.bindings[2].tup() == n2.tag().tup())
    assert M.channel(n3.bindings[2].pair, n3.pair) == 'formed'
    conn_from(c, 2, 3).fwd.append(fwd_frame(tag_of(n2)))
    must(c, lambda: refusals(n3, 'not formed'))
    must(c, lambda: c.nodes[4].known[0] is not None)        # formation progress continues
    assert not any(n.complete for n in c.live())


@test
def s14_configured_endpoint_answering_as_wrong_member():
    c = Cluster(n=3).start_all()
    c.quiesce()
    c.route[c.cfg.endpoint(3)] = 2
    n1 = c.nodes[1]
    cn = c.connect(n1, c.cfg.endpoint(3), 'peer', intended=3)
    t = n1.tag()
    cn.fwd.append(M.Frame('DECL', t, dict(role='peer', member=1, endpoint=n1.endpoint, digest=n1.digest,
                                          inc=t.inc, counter=t.counter, pair=t.pair), 1, 3))
    must(c, lambda: cn.state != 'declaring')
    assert cn.state == 'discarded' and 'reply from member 2' in cn.refusal


@test
def neg_s14_unvalidated_declaration_reply_under_s13():
    c = Cluster(n=3, rules=S13).start_all()
    c.quiesce()
    c.route[c.cfg.endpoint(3)] = 2
    n1 = c.nodes[1]
    cn = c.connect(n1, c.cfg.endpoint(3), 'peer', intended=3)
    t = n1.tag()
    cn.fwd.append(M.Frame('DECL', t, dict(role='peer', member=1, endpoint=n1.endpoint, digest=n1.digest,
                                          inc=t.inc, counter=t.counter, pair=t.pair), 1, 3))
    must(c, lambda: cn.state != 'declaring')
    assert cn.state == 'ready'
    n1.pool[3] = [cn]
    n1.send_peer(3, 'RAFT', raft(0, 'AE_REP', term=0, ok=False, match=0, hint=0, rid=0))
    expect_violation('misdirected', lambda: c.quiesce())


@test
def s14_stale_queued_raft_and_snapshot_refused_before_effects():
    c = formed(3)
    n1 = c.nodes[1]
    old = tag_of(c.nodes[2])
    cn = conn_from(c, 2, 1)
    hold = hold_thread(c, 1, 'pump')
    term = n1.raft[0].term + 50
    cn.fwd.append(M.Frame('RAFT', old, raft(0, 'VOTE_REQ', term=term, lli=10 ** 6, llt=10 ** 6), 2, 1))
    cn.fwd.append(M.Frame('RAFT', old, raft(0, 'SNAP_REQ', term=term, sid=('inj',), chunk=0, total=1,
                                            last_i=10 ** 6, last_t=1, voters=[1, 2, 3], commit=10 ** 6,
                                            state=None), 2, 1))
    must(c, lambda: sum(1 for it in n1.inbox if it.frame and it.frame.tag == old) == 2)
    c.restart(2)
    must(c, lambda: n1.bindings[2].inc == c.nodes[2].inc)
    c.release(hold)
    c.ticks(2)
    assert n1.raft[0].term < term and (n1.raft[0].stage is None or n1.raft[0].stage.sid != ('inj',))
    assert len(refusals(n1, 'stale incarnation')) >= 2


@test
def s17_higher_counter_frame_without_declaration():
    c = formed(3)
    ld = c.leader_of(0)
    x = next(m for m in (1, 2, 3) if m != ld)
    L, X = c.nodes[ld], c.nodes[x]
    must(c, lambda: L.bindings[x].tup() == X.tag().tup())
    cn = conn_from(c, x, ld)
    up = tag_of(X, counter=X.counter + 1)
    cn.fwd.append(fwd_frame(up, opid='hc1'))
    must(c, lambda: refusals(L, 'is not the current binding'))
    seq = c.seq
    cn.fwd.append(M.Frame('RAFT', up, raft(0, 'AE_REP', term=L.raft[0].term, ok=False, match=0, hint=0, rid=0),
                          x, ld))
    must(c, lambda: any(a[0] > seq and a[3] == up and a[2] == 'control' for a in c.monitor.acquired))
    cn.fwd.append(M.Frame('DECL', up, dict(role='peer', member=x, endpoint=X.endpoint, digest=X.digest,
                                           inc=up.inc, counter=up.counter, pair=up.pair), x, ld))
    cn.fwd.append(fwd_frame(up, opid='hc2'))
    must(c, lambda: any(a[3] == up and a[2] == 'app' for a in c.monitor.acquired))


@test
def s17_restarted_member_with_persisted_pair():
    c = formed(3)
    old = tag_of(c.nodes[2])
    pair = c.nodes[2].pair
    n2 = c.restart(2)
    assert n2.pair == pair and n2.inc == old.inc + 1
    must(c, lambda: all(c.nodes[m].bindings[2].inc == n2.inc for m in (1, 3)))
    cn = conn_from(c, 2, 1)
    cn.fwd.append(M.Frame('RAFT', old, raft(0, 'VOTE_REQ', term=999, lli=10 ** 6, llt=10 ** 6), 2, 1))
    must(c, lambda: refusals(c.nodes[1], 'below fence'))
    assert c.nodes[1].raft[0].term < 999
    must(c, c.converged)


# ---------------------------------------------------------------------------
# C8 / §18 §19 §20 §21 §22: operation lifecycle, timeouts and obligations


def token_for(c, mid, opid, owner='token'):
    n = c.nodes[mid]
    for op in n.ops.values():
        if op.opid == opid and op.owner == owner:
            return op
    return None


@test
def s18_commit_pause_apply_then_sender_incarnation_change():
    c = formed(3)
    key, g, ld = leader_group_key(c, avoid=3)
    hold = hold_thread(c, ld, 'apply%d' % g)
    cl = c.add_client(3, [('APP', 'cpa', key, 'x')])
    must(c, lambda: token_for(c, ld, 'cpa') and token_for(c, ld, 'cpa').accepted)
    c.restart(3)                                             # the forwarder's incarnation changes
    c.release(hold)
    client_done(c, cl)
    must(c, c.converged)
    applied_once(c, 'cpa', key)


@test
def s18_stale_reply_after_replier_restart_then_retry_resolves():
    c = formed(3)
    key, g, ld = leader_group_key(c, avoid=3)
    hold = c.hold(lambda t: t.node_id == 3 and t.name.startswith('reply') and t.peek() is not None
                  and t.peek().kind == 'FWD_REP')
    cl = c.add_client(3, [('APP', 'srr', key, 'x')])
    must(c, lambda: any(op.opid == 'srr' and op.terminal == 'published' for op in c.nodes[ld].ops.values()))
    c.restart(ld)
    must(c, lambda: c.nodes[3].bindings[ld].inc == c.nodes[ld].inc)
    c.release(hold)
    client_done(c, cl)
    assert refusals(c.nodes[3], 'stale incarnation')
    must(c, c.converged)
    applied_once(c, 'srr', key)


@test
def c8_forward_queued_across_leader_restart():
    """Model finding: a request record takes the responder's binding when it is sent; a
    refused reply leaves the operation uncertain, never pinned (found by the search)."""
    c = formed(3)
    key, g, ld = leader_group_key(c, avoid=3)
    hold = c.hold(lambda t: t.node_id == 3 and t.name == 'send%d' % ld)
    cl = c.add_client(3, [('APP', 'fq', key, 'x')])
    must(c, lambda: any(k == 'FWD_REQ' for k, _, _ in c.nodes[3].outq[ld]))
    c.restart(ld)
    must(c, lambda: c.nodes[3].bindings[ld].inc == c.nodes[ld].inc)
    c.release(hold)
    client_done(c, cl)
    must(c, lambda: c.converged() and not [op for op in c.nodes[3].ops.values() if op.owner == 'token'])
    applied_once(c, 'fq', key)


@test
def c8_waiter_leaves_between_admission_and_start():
    """Model finding: a proven pre-acceptance abort must stop the paused effect step, and the
    abort's reply must still name the request (found by the search)."""
    c = formed(3)
    key, g, ld = leader_group_key(c, avoid=3)
    hold = c.hold(lambda t: t.node_id == ld and t.name.startswith('admit') and t.stage == 'registered')
    cl = c.add_client(3, [('APP', 'wl', key, 'x')])
    must(c, lambda: token_for(c, ld, 'wl') is not None)
    op = token_for(c, ld, 'wl')
    assert op.state == 'admitted' and timeout(op) == 'aborted'
    c.release(hold)
    client_done(c, cl)
    assert op.idx is None                                     # the aborted operation never started
    must(c, c.converged)
    applied_once(c, 'wl', key)


@test
def s19_storage_write_paused_mid_io_across_transition():
    c = formed(3)
    c.io_hold.add(51)
    cl = c.add_client(2, [('PUT', 'w51', 51), ('PUBLISH', 'pub51', 51)])
    must(c, lambda: token_for(c, 2, 'w51') and token_for(c, 2, 'w51').state == 'put_io')
    c.restart(1)
    must(c, lambda: c.nodes[2].bindings[1].inc == c.nodes[1].inc)
    c.io_hold.discard(51)
    client_done(c, cl)
    assert c.stores[2].frags[51] == 'durable'
    must(c, c.converged)
    applied_once(c, 'pub51', 51)


def drain_trace(rules):
    c = formed(3, rules=rules)
    key, g, ld = leader_group_key(c, avoid=3)
    z = next(m for m in (1, 2, 3) if m not in (ld, 3))
    c.crash(z)                                               # the quorum now needs member 3
    hold = hold_thread(c, 3, 'pump')
    c.add_client(3, [('APP', 'drn', key, 'x')])
    must(c, lambda: token_for(c, ld, 'drn') and token_for(c, ld, 'drn').state == 'proposed')
    c.release(hold)
    c.restart(3)                                             # transition while the token waits on 3's ack
    return c, ld, g


@test
def s20_pause_before_quorum_ack_then_transition_both_finish():
    c, ld, g = drain_trace(CONSOLIDATED)
    L = c.nodes[ld]
    must(c, lambda: L.bindings[3].inc == c.nodes[3].inc and not token_for(c, ld, 'drn')
         and 'drn' in L.raft[g].kv.receipts)


@test
def neg_s22_drain_waits_for_traffic_it_refuses_under_s21():
    c, ld, g = drain_trace(S21)
    L = c.nodes[ld]
    never(c, lambda: not token_for(c, ld, 'drn'), 20000, 'drain completion under §21')
    assert 3 in L.pending and refusals(L, 'redeclare required')


@test
def s20_local_pair_install_during_inflight_formation_op():
    c = Cluster(n=3).start_all()
    mg = c.add_mgmt(1)
    must(c, lambda: mg.done and c.all_formed())
    ld = c.leader_of(0)
    assert c.nodes[ld].counter >= 1 and mg.result['pair'] == c.nodes[ld].pair


@test
def s20_timeout_before_acceptance_aborts():
    c = formed(3)
    hold = c.hold(lambda t: t.node_id == 2 and t.name.startswith('admit') and t.stage == 'io_begin')
    cl = c.add_client(2, [('PUT', 'tb', 61)])
    must(c, lambda: token_for(c, 2, 'tb') and token_for(c, 2, 'tb').state == 'put_staged')
    op = token_for(c, 2, 'tb')
    assert timeout(op) == 'aborted' and c.nodes[2].units() == 0 and op.terminal == 'aborted'
    c.release(hold)
    c.quiesce()
    assert c.stores[2].frags.get(61) is None or cl.results.get('tb') == 'ok'
    client_done(c, cl)


@test
def s20_timeout_after_acceptance_before_reply_transfers():
    c = formed(3)
    key, g, ld = leader_group_key(c, avoid=0)
    hold = hold_thread(c, ld, 'apply%d' % g)
    cl = c.add_client(ld, [('APP', 'ta', key, 'x')])
    must(c, lambda: token_for(c, ld, 'ta') and token_for(c, ld, 'ta').accepted)
    op = token_for(c, ld, 'ta')
    assert timeout(op) == 'transferred' and c.nodes[ld].units() == 0
    c.release(hold)
    client_done(c, cl)
    must(c, c.converged)
    applied_once(c, 'ta', key)


@test
def s20_io_error_with_uncertain_result():
    for outcome, fid in (('uncertain_durable', 63), ('uncertain_none', 65)):
        c = formed(3)
        c.io_policy[fid] = outcome
        cl = c.add_client(2, [('PUT', 'u%d' % fid, fid)])
        must(c, lambda: any(op.resolver == 'fragment' for op in c.nodes[2].ops.values()))
        c.io_policy[fid] = 'ok'
        cl.ops.append(('PUBLISH', 'pu%d' % fid, fid))
        client_done(c, cl)
        assert c.stores[2].frags[fid] == 'durable' and 'uncertain' in [r for _, r in cl.log]
        must(c, c.converged)
        applied_once(c, 'pu%d' % fid, fid)
        assert not [op for op in c.nodes[2].ops.values() if op.owner in ('token', 'obligation')]


@test
def s22_uncertain_io_completes_after_transition_unpublished():
    c = formed(3)
    c.io_hold.add(67)
    cl = c.add_client(2, [('PUT', 'w67', 67), ('PUBLISH', 'p67', 67)])
    must(c, lambda: token_for(c, 2, 'w67') and token_for(c, 2, 'w67').state == 'put_io')
    assert timeout(token_for(c, 2, 'w67')) == 'handed off'
    c.restart(1)
    must(c, lambda: c.nodes[2].bindings[1].inc == c.nodes[1].inc)
    c.io_hold.discard(67)
    must(c, lambda: not any(op.owner == 'obligation' for op in c.nodes[2].ops.values()))
    assert all(67 not in n.raft[0].kv.published for n in c.live() if 0 in n.raft) or 'w67' in cl.results
    client_done(c, cl)
    must(c, c.converged)
    applied_once(c, 'p67', 67)


@test
def s22_concurrent_retry_with_same_identity_publishes_once():
    c = formed(3)
    key, g, ld = leader_group_key(c, avoid=0)
    others = [m for m in (1, 2, 3) if m != ld]
    holds = [hold_thread(c, m, 'pump') for m in others]
    cl = c.add_client(ld, [('APP', 'cr', key, 'x')])
    must(c, lambda: token_for(c, ld, 'cr') and token_for(c, ld, 'cr').state == 'proposed')
    assert timeout(token_for(c, ld, 'cr')) == 'handed off'
    must(c, lambda: len([op for op in c.nodes[ld].ops.values() if op.opid == 'cr']) >= 2)
    for h in holds:
        c.release(h)
    client_done(c, cl)
    must(c, c.converged)
    r = c.nodes[ld].raft[g]
    proposals = [i for i in range(r.base + 1, r.last_index() + 1) if r.entry(i)[1][:2] == ('APP', 'cr')]
    assert len(proposals) >= 2 or r.base > 0
    applied_once(c, 'cr', key)


@test
def s22_uncommitted_entry_replaced_by_new_leader():
    c = formed(3)
    key, g, ld = leader_group_key(c, avoid=0)
    holds = [hold_frames_from(c, ld, m) for m in (1, 2, 3) if m != ld]
    cl = c.add_client(ld, [('APP', 'ur', key, 'x')])
    must(c, lambda: token_for(c, ld, 'ur') and token_for(c, ld, 'ur').state == 'proposed')
    idx = token_for(c, ld, 'ur').idx
    must(c, lambda: c.leader_of(g) not in (None, ld) and c.nodes[c.leader_of(g)].raft[g].commit >= idx)
    for h in holds:
        c.release(h)
    client_done(c, cl)
    must(c, c.converged)
    assert any(e[1] == 'handoff' and e[2] == ld for e in c.monitor.events)
    applied_once(c, 'ur', key)


@test
def s23_continuation_must_resolve_an_owned_operation():
    c = formed(3)
    n1, n2 = c.nodes[1], c.nodes[2]
    must(c, lambda: n1.bindings[2].tup() == n2.tag().tup())
    cn = conn_from(c, 1, 2)
    cn.rev.append(M.Frame('FWD_REP', tag_of(n2), dict(ref=424242, result='ok'), 2))
    must(c, lambda: refusals(n1, 'no owned operation'))
    op = n1.new_op('FR', 2, n1.bindings[2].tup(), {'FR_REP'}, 0, None, dict(g=0, sent=c.now), owner='control')
    cn.rev.append(M.Frame('FWD_REP', tag_of(n2), dict(ref=op.id, result='ok'), 2))
    must(c, lambda: refusals(n1, 'not authorized by the operation'))


# ---------------------------------------------------------------------------
# C6 / C8 / §24 §25: repeated transitions, high-water mark, immutable tokens, bounds


@test
def s24_abcd_outstanding_operations():
    c = formed(3, member_bound=4, total_bound=8)
    ld = c.leader_of(0)
    x = next(m for m in (1, 2, 3) if m != ld)
    L, X = c.nodes[ld], c.nodes[x]
    must(c, lambda: L.bindings[x].tup() == X.tag().tup())
    hold = hold_thread(c, ld, 'apply0')
    cn = conn_from(c, x, ld)
    base = X.counter
    tags = {k: tag_of(X, counter=base + i) for i, k in enumerate('ABCD')}

    def decl(k):
        t = tags[k]
        cn.fwd.append(M.Frame('DECL', t, dict(role='peer', member=x, endpoint=X.endpoint, digest=X.digest,
                                              inc=t.inc, counter=t.counter, pair=t.pair), x, ld))

    def fwd(k, opid):
        cn.fwd.append(fwd_frame(tags[k], ref=900000 + len(opid) * 100 + ord(opid[-1]), key=1, opid=opid))
    fwd('A', 'a1')
    decl('B'), fwd('B', 'b1')
    decl('C'), decl('B')                                     # reordered B after C: stale
    fwd('B', 'b2')                                           # retired tuple: refused
    fwd('C', 'c1')
    decl('D'), decl('D')                                     # duplicate: idempotent
    fwd('D', 'd1'), fwd('D', 'd2')                           # bound reached at d2
    c.quiesce()
    toks = {op.opid: op for op in L.ops.values() if op.owner == 'token' and op.remote == x}
    assert set(toks) == {'a1', 'b1', 'c1', 'd1'}, toks
    assert [toks[o].tup[1] for o in ('a1', 'b1', 'c1', 'd1')] == [base, base + 1, base + 2, base + 3]
    assert L.bindings[x].counter == base + 3 and c.monitor.hwm[(ld, x)].counter == base + 3
    assert refusals(L, 'stale counter') and refusals(L, 'is not the current binding') and refusals(L, 'BUSY')
    assert L.units(x) == 4
    c.release(hold)
    must(c, lambda: not any(op.owner == 'token' and op.remote == x for op in L.ops.values()))
    assert all(op.terminal == 'published' for op in toks.values())


@test
def s24_declaration_during_partial_formation_refuses_application():
    c = Cluster(n=4).start_all()
    c.pause_salt = True
    c.add_mgmt(1)
    must(c, lambda: all(c.nodes[m].known[0] for m in (1, 2, 3)))
    n2, n3 = c.nodes[2], c.nodes[3]
    must(c, lambda: n3.bindings[2].tup() == n2.tag().tup())   # the declaration was accepted
    conn_from(c, 2, 3).fwd.append(fwd_frame(tag_of(n2)))
    must(c, lambda: refusals(n3, 'not formed'))
    before = n3.raft[0].commit
    ld = c.leader_of(0)
    lock_do(c.nodes[ld], lambda: c.nodes[ld].raft[0].propose(('NOOP',)))
    must(c, lambda: n3.raft[0].commit > before)              # control traffic still commits


@test
def s24_supported_commands_under_intervening_state():
    c = formed(3)
    pair = c.nodes[1].pair
    for g, cmd in ((0, 'MKFS'), (2, 'SALT')):
        ld = c.leader_of(g)
        lock_do(c.nodes[ld], lambda: c.nodes[ld].raft[g].propose((cmd, ('intervening', c.digest))))
        lock_do(c.nodes[ld], lambda: c.nodes[ld].raft[g].propose((cmd, pair)))
    must(c, c.converged)
    assert all(n.raft[g].kv.pair == pair for n in c.live() for g in n.raft) and all(n.pair == pair for n in c.live())
    assert not any(n.stopped for n in c.nodes.values())
    ch = c.add_mgmt(1, cmd='RAFT_CHANGE', args=dict(voters=[1, 2]))
    must(c, lambda: ch.done)
    assert 'not permitted' in ch.result['reason']


# ---------------------------------------------------------------------------
# C9 / §20 §22 §24: snapshots


def lone_raft(rules=CONSOLIDATED, term=12, applied=900, log_terms=None):
    c = Cluster(n=3, rules=rules)
    n = M.Node(c, 1, 1, 'p')
    r = n.raft[0]
    r.term = term
    r.log = [(t, ('NOOP',)) for t in (log_terms or [9] * applied)]
    r.persisted = r.last_index()
    r.kv.applied = r.applied = r.commit = applied
    return c, n, r


def snap_msg(r, term=12, last_i=1000, last_t=9, commit=1000):
    st = M.KV()
    st.applied = last_i
    return dict(t='SNAP_REQ', term=term, sid=('L', term, last_i), chunk=0, total=1, last_i=last_i, last_t=last_t,
                voters=sorted(r.voters), commit=commit, state=st)


@test
def s22_snapshot_rpc_term_12_included_term_9_installs():
    c, n, r = lone_raft()
    m = snap_msg(r)
    r.on_SNAP_REQ(2, m, None)
    assert r.stage is not None and r.snapshot_verdict(m) is None
    r.install(m)
    assert (r.base, r.base_term, r.applied, r.commit, r.term) == (1000, 9, 1000, 1000, 12)


@test
def neg_s22_included_term_ordered_against_current_term_under_s21():
    c, n, r = lone_raft(rules=S21)
    r.on_SNAP_REQ(2, snap_msg(r), None)
    assert r.stage is None and refusals(n, 'snapshot term below current term')


@test
def s22_stale_rpc_term_refused_and_already_applied_no_import():
    c, n, r = lone_raft()
    r.on_SNAP_REQ(2, snap_msg(r, term=11, last_t=9), None)
    assert r.stage is None and r.term == 12
    c, n, r = lone_raft(applied=1100)
    assert r.snapshot_verdict(snap_msg(r)) == 'no-import'
    m = snap_msg(r, last_t=13)
    assert r.snapshot_verdict(m) == 'last-included term above RPC term'
    assert r.snapshot_verdict(snap_msg(r, commit=999)) == 'snapshot above attested commit'


@test
def s24_snapshot_suffix_retained_or_discarded():
    c, n, r = lone_raft(log_terms=[9] * 1000 + [12] * 5)
    r.install(snap_msg(r))
    assert (r.base, r.last_index(), r.term_at(1005)) == (1000, 1005, 12)
    c, n, r = lone_raft(log_terms=[9] * 999 + [10] + [12] * 5)
    r.install(snap_msg(r))
    assert (r.base, r.last_index()) == (1000, 1000)


def lagging_snapshot_cluster():
    c = formed(3)
    hold = hold_thread(c, 3, 'pump')
    cl = c.add_client(1, [('APP', 'sn%d' % i, i, 'v') for i in range(1, 9)])
    client_done(c, cl)
    must(c, lambda: all(c.nodes[m].raft[g].applied == c.nodes[m].raft[g].commit for m in (1, 2) for g in (0, 2)))
    for m in (1, 2):
        for g in (0, 2):
            c.nodes[m].raft[g].compact()
    n3 = c.nodes[3]                       # frames in flight to the lagging node are lost
    n3.inbox.clear()
    for cn in c.conns.values():
        if cn.admitter == 3:
            cn.fwd.clear()
    for m in (1, 2):
        c.nodes[m].outq[3].clear()
    return c, hold


@test
def s20_snapshot_ahead_of_receiver_installs():
    c, hold = lagging_snapshot_cluster()
    c.release(hold)
    must(c, c.converged)
    assert all(c.nodes[3].raft[g].base > 0 for g in (0, 2))
    for i in range(1, 9):
        assert c.nodes[3].raft[M.shard_group(i)].kv.receipts.get('sn%d' % i)


@test
def s20_transition_during_staged_installation():
    c, hold = lagging_snapshot_cluster()
    sh = c.hold(lambda t: t.node_id == 3 and t.name.startswith('snap') and t.stage == 'install_io')
    c.release(hold)
    must(c, lambda: any(t.node_id == 3 and t.name.startswith('snap') and t.stage == 'install_io'
                        for t in c.threads if t.alive))
    sender = next(r.stage.sid[0] for r in c.nodes[3].raft.values() if r.stage and r.stage.installing)
    c.restart(sender)
    must(c, lambda: c.nodes[3].bindings[sender].inc == c.nodes[sender].inc)
    c.release(sh)
    must(c, c.converged)
    assert all(c.nodes[3].raft[g].base > 0 for g in (0, 2))


# ---------------------------------------------------------------------------
# C11 / §26 §28: the Raft incarnation fence


def fence_pair(c):
    ld = c.leader_of(0)
    return ld, next(m for m in (1, 2, 3) if m != ld)


@test
def s26_older_incarnation_frame_after_newer_declaration():
    for direction in (0, 1):
        c = formed(3)
        ld, f = fence_pair(c)
        y, x = (ld, f) if direction == 0 else (f, ld)
        Y = c.nodes[y]
        old = tag_of(c.nodes[x])
        cn = conn_from(c, x, y)
        hold = hold_thread(c, y, 'pump')
        queued_term = Y.raft[0].term + 40
        cn.fwd.append(M.Frame('RAFT', old, raft(0, 'VOTE_REQ', term=queued_term, lli=10 ** 6, llt=10 ** 6), x, y))
        must(c, lambda: any(it.frame and it.frame.tag == old for it in Y.inbox))      # queued before
        assert Y.pred_control(old) is None                                           # pre: admissible
        c.restart(x)
        must(c, lambda: Y.bindings[x].inc == c.nodes[x].inc)                          # the ordering point
        assert Y.pred_control(old) == 'stale incarnation'                            # post: refused
        live = conn_from(c, x, y)
        live.fwd.append(M.Frame('RAFT', old, raft(0, 'VOTE_REQ', term=queued_term + 1, lli=10 ** 6,
                                                  llt=10 ** 6), x, y))               # normal receive
        must(c, lambda: refusals(Y, 'below fence'))
        c.release(hold)
        c.ticks(2)
        assert Y.raft[0].term < queued_term and refusals(Y, 'stale incarnation')


@test
def s26_s28_declaration_racing_queued_read_proof():
    c = formed(3)
    ld, f = fence_pair(c)
    L = c.nodes[ld]
    old = tag_of(c.nodes[f])
    holds = [hold_thread(c, ld, 'pump'), hold_thread(c, ld, 'early_reads')]
    r = L.raft[0]
    got = []
    assert lock_do(L, lambda: r.start_read(lambda rr: got.append(rr.commit)))
    rid = r.read_round
    rd = r.reads[rid]
    L.inbox.append(M.Item('RAFT', M.Frame('RAFT', old, raft(0, 'AE_REP', term=r.term, ok=True, match=0, rid=rid),
                                          f, ld), conn_from(c, f, ld)))
    assert L.pred_control(old) is None
    c.restart(f)
    must(c, lambda: L.bindings[f].inc == c.nodes[f].inc)
    c.release(holds[1])
    must(c, lambda: refusals(L, 'stale incarnation'))
    assert f not in rd['acks']
    c.release(holds[0])
    must(c, lambda: got)                                      # completes on fresh acknowledgements


@test
def s26_counter_only_transition_raft_continues_application_refused():
    for rules in (CONSOLIDATED, S31):
        c = formed(3, rules=rules)
        ld, x = fence_pair(c)
        L, X = c.nodes[ld], c.nodes[x]
        must(c, lambda: L.bindings[x].tup() == X.tag().tup())
        cn = conn_from(c, x, ld)
        old = tag_of(X)
        up = tag_of(X, counter=X.counter + 1)
        word = L.fence[x]
        cn.fwd.append(M.Frame('DECL', up, dict(role='peer', member=x, endpoint=X.endpoint, digest=X.digest,
                                               inc=up.inc, counter=up.counter, pair=up.pair), x, ld))
        must(c, lambda: L.bindings[x].counter == up.counter)
        assert L.fence[x] == word                              # the fence word did not move
        seq = c.seq
        cn.fwd.append(M.Frame('RAFT', old, raft(0, 'AE_REP', term=L.raft[0].term, ok=False, match=0, hint=0,
                                                rid=0), x, ld))
        cn.fwd.append(fwd_frame(old, opid='co'))
        must(c, lambda: refusals(L, 'is not the current binding'))
        c.quiesce()
        raft_ok = any(a[0] > seq and a[3] == old and a[2] == 'control' for a in c.monitor.acquired)
        if rules is CONSOLIDATED:
            assert raft_ok
        else:
            assert not raft_ok and refusals(L, 'counter differs')   # §31 exact equality fences control
        newer = tag_of(X, inc=X.inc + 1, counter=0)
        cn.fwd.append(M.Frame('DECL', newer, dict(role='peer', member=x, endpoint=X.endpoint, digest=X.digest,
                                                  inc=newer.inc, counter=0, pair=newer.pair), x, ld))
        must(c, lambda: L.bindings[x].inc == newer.inc)
        n_ref = len(L.refused)
        cn.fwd.append(M.Frame('RAFT', up, raft(0, 'AE_REP', term=L.raft[0].term, ok=False, match=0, hint=0,
                                               rid=0), x, ld))
        cn.fwd.append(fwd_frame(up, opid='co2'))
        must(c, lambda: len(L.refused) >= n_ref + 2)


@test
def s26_repeated_uncertain_handoffs_with_resolution_paused_are_bounded():
    c = formed(3, total_bound=3, member_bound=3)
    key, g, ld = leader_group_key(c, avoid=0)
    L = c.nodes[ld]
    L.resolution_paused.add('raft_entry')
    holds = [hold_thread(c, m, 'pump') for m in (1, 2, 3) if m != ld]
    cls = [c.add_client(ld, [('APP', 'bh%d' % i, key, 'x')]) for i in range(3)]
    for rnd in range(2):                                      # repeated handoffs, retries take over
        for i in range(3):
            must(c, lambda: token_for(c, ld, 'bh%d' % i) and token_for(c, ld, 'bh%d' % i).state == 'proposed')
            assert timeout(token_for(c, ld, 'bh%d' % i)) == 'handed off'
        assert L.units() == 3 and len([op for op in L.ops.values() if op.owner == 'obligation']) == 3
    quiet = c.hold(lambda t: t.owner in cls)                  # no retries while resolution is paused
    late = c.add_client(ld, [('APP', 'new', key, 'y')])
    must(c, lambda: (0, 'busy') in late.log)                  # new work refused at the bound
    for h in holds:
        c.release(h)
    must(c, lambda: all('bh%d' % i in L.raft[g].kv.receipts for i in range(3)))   # control continues
    c.ticks(3)
    assert L.units() == 3 and 'new' not in late.results       # obligations still held
    L.resolution_paused.clear()
    must(c, lambda: 'new' in late.results)                    # admissions recover
    c.release(quiet)
    for cl in cls + [late]:
        client_done(c, cl)
    must(c, lambda: L.units() == 0 and c.converged())
    for i in range(3):
        applied_once(c, 'bh%d' % i, key)


@test
def c8_retry_of_held_obligation_reuses_its_reservation():
    """Model finding (C13): at the bound, the retry that resolves an obligation is not new work."""
    c = formed(3, total_bound=1, member_bound=1)
    key, g, ld = leader_group_key(c, avoid=0)
    holds = [hold_thread(c, m, 'pump') for m in (1, 2, 3) if m != ld]
    cl = c.add_client(ld, [('APP', 'rr', key, 'x')])
    must(c, lambda: token_for(c, ld, 'rr') and token_for(c, ld, 'rr').state == 'proposed')
    assert timeout(token_for(c, ld, 'rr')) == 'handed off' and c.nodes[ld].units() == 1
    must(c, lambda: token_for(c, ld, 'rr') is not None)       # the retry was admitted at the bound
    assert not (0, 'busy') in cl.log and c.nodes[ld].units() == 1
    for h in holds:
        c.release(h)
    client_done(c, cl)
    must(c, c.converged)
    applied_once(c, 'rr', key)


@test
def s28_batch_receive_declaration_between_items():
    c = formed(3, batch=8)
    ld, x = fence_pair(c)
    L, X = c.nodes[ld], c.nodes[x]
    hold = hold_thread(c, ld, 'pump')
    c.quiesce()
    L.inbox.clear()
    old = tag_of(X)
    cn = conn_from(c, x, ld)
    f1 = M.Frame('RAFT', old, raft(0, 'AE_REP', term=L.raft[0].term, ok=False, match=0, hint=0, rid=0), x, ld)
    f2 = M.Frame('RAFT', old, raft(0, 'AE_REP', term=L.raft[0].term, ok=False, match=0, hint=0, rid=0), x, ld)
    L.inbox.extend([M.Item('RAFT', f1, cn), M.Item('RAFT', f2, cn)])
    pump = c.find(lambda t: t.node_id == ld and t.name == 'pump')
    c.release(hold)
    while not (pump.stage == 'registered'):
        c.step(pump)
    assert any(a[3] is old for a in c.monitor.acquired)
    hold = hold_thread(c, ld, 'pump')
    c.restart(x)
    must(c, lambda: L.bindings[x].inc == c.nodes[x].inc)
    c.release(hold)
    c.ticks(1)
    assert refusals(L, 'stale incarnation')
    acq = [a for a in c.monitor.acquired if a[3] is old]
    assert len(acq) == 1, acq                                 # item 1 completed, item 2 refused


@test
def s28_declaration_in_the_unlocked_persistence_interval():
    c = formed(3)
    y, cand, x = 2, 1, 3
    Y = c.nodes[y]
    hold = c.hold(lambda t: t.node_id == y and t.name == 'persist' and t.stage == 'persist_io')
    term = Y.raft[0].term + 1
    conn_from(c, cand, y).fwd.append(M.Frame('RAFT', tag_of(c.nodes[cand]),
                                             raft(0, 'VOTE_REQ', term=term, lli=10 ** 6, llt=10 ** 6), cand, y))
    must(c, lambda: Y.raft[0].vote == cand and any(t.node_id == y and t.name == 'persist'
                                                     and t.stage == 'persist_io' for t in c.threads if t.alive))
    assert Y.raft[0].disk.term < term
    assert not any(k == 'RAFT' and b['m']['t'] == 'VOTE_REP' and b['m'].get('granted')
                   for k, b, _ in Y.outq[cand])
    c.restart(x)
    must(c, lambda: Y.bindings[x].inc == c.nodes[x].inc)      # published during the unlocked interval
    c.release(hold)
    must(c, lambda: Y.raft[0].disk.vote == cand or Y.raft[0].disk.term > term)
    must(c, c.converged)


@test
def s28_exactly_full_all_tokens_handed_off_destination_refusal_mixed_sessions():
    c = formed(3, total_bound=4, member_bound=4)
    N = c.nodes[2]
    c.io_hold.update({81, 82})
    a = c.add_client(2, [('PUT', 'f81', 81)])
    b = c.add_client(2, [('PUT', 'f82', 82)])
    must(c, lambda: token_for(c, 2, 'f81') and token_for(c, 2, 'f82')
         and token_for(c, 2, 'f81').state == token_for(c, 2, 'f82').state == 'put_io')
    assert N.units() == 4
    third = c.add_client(2, [('APP', 'full', 1, 'x')])
    must(c, lambda: (0, 'busy') in third.log)
    N.refuse_handoff.add('fragment')
    op = token_for(c, 2, 'f81')
    assert timeout(op) == 'kept' and op.owner == 'token' and N.units() == 4    # ownership retained
    N.refuse_handoff.clear()
    assert timeout(op) == 'handed off' and timeout(token_for(c, 2, 'f82')) == 'handed off'
    assert N.units() == 4                                     # reservation transferred, no extra slot
    c.io_hold.clear()
    for cl in (a, b, third):
        client_done(c, cl)
    must(c, lambda: N.units() == 0)


# ---------------------------------------------------------------------------
# C7 / §30 §32 §33: check-and-register


def cr_setup(rules=CONSOLIDATED):
    c = formed(3, rules=rules)
    y, x = 1, 2
    Y, X = c.nodes[y], c.nodes[x]
    must(c, lambda: Y.bindings[x].tup() == X.tag().tup())
    hold = hold_thread(c, y, 'pump')
    c.quiesce()
    Y.inbox.clear()
    old = tag_of(X)
    term = max(Y.raft[0].term, X.raft[0].term) + 1
    f = M.Frame('RAFT', old, raft(0, 'VOTE_REQ', term=term, lli=10 ** 6, llt=10 ** 6), x, y)
    Y.inbox.append(M.Item('RAFT', f, conn_from(c, x, y)))
    pump = c.find(lambda t: t.node_id == y and t.name == 'pump')
    c.release(hold)
    return c, Y, x, f, term, pump


def publish_d(c, Y, x):
    hold = hold_thread(c, Y.id, 'pump')
    c.restart(x)
    must(c, lambda: Y.bindings[x].inc == c.nodes[x].inc)
    c.release(hold)


@test
def s30_frame_paused_before_registration_refused_after_declaration():
    c, Y, x, f, term, pump = cr_setup()
    c.step(pump)
    assert pump.stage == 'taken'
    publish_d(c, Y, x)
    while pump.stage != 'idle':
        c.step(pump)
    assert Y.raft[0].term < term and refusals(Y, 'stale incarnation')
    assert not any(a[3] is f.tag for a in c.monitor.acquired)


@test
def s30_frame_paused_after_registration_completes_only_its_effects():
    c, Y, x, f, term, pump = cr_setup()
    while pump.stage != 'registered':
        c.step(pump)
    publish_d(c, Y, x)
    c.step(pump)                                              # the registered effect runs after D
    assert Y.raft[0].term == term                             # its named effect: term update and vote
    later = M.Frame('RAFT', f.tag, raft(0, 'VOTE_REQ', term=term + 5, lli=10 ** 6, llt=10 ** 6), x, Y.id)
    Y.inbox.append(M.Item('RAFT', later, conn_from(c, x, Y.id)))
    must(c, lambda: refusals(Y, 'stale incarnation'))         # unrelated frames inherit nothing
    assert Y.raft[0].term < term + 5


@test
def neg_s30_atomic_load_then_effect_under_s29():
    c, Y, x, f, term, pump = cr_setup(S29)
    while pump.stage != 'checked':
        c.step(pump)
    publish_d(c, Y, x)                                        # D is not blocked: no lock was taken
    expect_violation('obsolete_authority', lambda: c.step(pump))


@test
def s32_declaration_cannot_publish_between_check_and_register():
    c, Y, x, f, term, pump = cr_setup()
    while pump.stage != 'checked':
        c.step(pump)
    assert Y.lock is pump
    hold = hold_thread(c, Y.id, 'pump')
    c.restart(x)
    must(c, lambda: any(t.node_id == Y.id and t.name.startswith('admit') and t.cond.kind == 'lock'
                        and t.stage == 'decl' for t in c.threads if t.alive))
    blocked = next(t for t in c.threads if t.alive and t.node_id == Y.id and t.name.startswith('admit')
                   and t.cond.kind == 'lock' and t.stage == 'decl')
    try:
        c.step(blocked)
    except RuntimeError:
        pass
    else:
        raise AssertionError('declaration published inside the check-and-register section')
    assert Y.bindings[x].inc == f.tag.inc
    c.release(hold)
    c.step(pump)                                              # register, release the lock
    assert Y.lock is None
    must(c, lambda: Y.bindings[x].inc == c.nodes[x].inc)
    c.ticks(1)
    assert Y.raft[0].term >= term                             # registered before D: completed


@test
def s32_registered_old_term_work_obeys_raft():
    c = formed(3)
    ld, x = fence_pair(c)
    X, L = c.nodes[x], c.nodes[ld]
    r = X.raft[0]
    must(c, lambda: r.commit > 0 and X.bindings[ld].tup() == L.tag().tup())
    before = (r.term, r.last_index(), r.commit, [r.entry(i) for i in range(r.base + 1, r.last_index() + 1)])
    old_term = r.term - 1 if r.term > 1 else 0
    cn = conn_from(c, ld, x)
    cn.fwd.append(M.Frame('RAFT', tag_of(L), raft(0, 'AE_REQ', term=old_term, prev_i=0, prev_t=0,
                                                  ents=[(old_term, ('NOOP',))] * 3, commit=0, rid=0), ld, x))
    cn.fwd.append(M.Frame('RAFT', tag_of(L), raft(0, 'VOTE_REQ', term=old_term, lli=10 ** 6, llt=10 ** 6), ld, x))
    c.ticks(1)
    after = (r.term, r.last_index(), r.commit, [r.entry(i) for i in range(r.base + 1, before[1] + 1)])
    assert after[0] == before[0] and after[2] >= before[2] and after[3][:len(before[3])] == before[3]
    assert r.vote in (None, ld) or r.vote == before[0]
    # an acknowledgement from an earlier term never completes a read proof
    lr = L.raft[0]
    lock_do(L, lambda: lr.start_read(lambda rr: None))
    rid = lr.read_round
    lr.note_read_ack(x, lr.term - 1, rid)
    assert rid not in lr.reads or x not in lr.reads[rid]['acks']


@test
def s32_vote_reply_waits_for_durable_hard_state():
    c = formed(3)
    Y = c.nodes[2]
    hold = hold_thread(c, 2, 'persist')
    term = Y.raft[0].term + 3
    conn_from(c, 1, 2).fwd.append(M.Frame('RAFT', tag_of(c.nodes[1]),
                                          raft(0, 'VOTE_REQ', term=term, lli=10 ** 6, llt=10 ** 6), 1, 2))
    must(c, lambda: Y.raft[0].vote == 1 and Y.raft[0].term == term)
    assert any(m['t'] == 'VOTE_REP' and m['granted'] for _, m in Y.raft[0].held)
    assert not any(b['m']['t'] == 'VOTE_REP' for k, b, _ in Y.outq[1] if k == 'RAFT')
    c.release(hold)
    must(c, lambda: Y.raft[0].disk.term >= term)
    must(c, c.converged)


# ---------------------------------------------------------------------------
# C14: randomized interleaving search


@test
def random_interleavings():
    t0 = time.time()
    tot = dict(runs=0, faults=0, ops=0, restarts=0, handoffs=0, stale=0, busy=0)
    for seed in range(SEEDS):
        for n in (3, 4, 5):
            r = M.random_search(seed, n)
            tot['runs'] += 1
            for k in ('faults', 'ops', 'restarts', 'handoffs', 'stale', 'busy'):
                tot[k] += r[k]
    print('    random search: %(runs)d schedules, %(faults)d injected faults (%(restarts)d restarts), '
          '%(ops)d client operations, %(handoffs)d obligation handoffs, %(stale)d stale refusals, '
          '%(busy)d BUSY admissions' % tot + ', %.1fs' % (time.time() - t0))


@test
def neg_random_search_detects_superseded_rules():
    """The same bounded search, run under superseded rule sets, must fail."""
    for rules, expect in ((S29, 'obsolete_authority'), (S15, 'obsolete_authority'),
                          (S13, 'obsolete_authority')):
        hit = None
        for seed in range(15):
            for n in (3, 4):
                try:
                    M.random_search(seed, n, rules=rules)
                except Violation as v:
                    hit = v.name
                except AssertionError:
                    hit = 'liveness'
                if hit:
                    break
            if hit:
                break
        assert hit == expect, '%s: search found %s, expected %s' % (rules.name, hit, expect)
        print('    %s: search found %s at seed %d n %d' % (rules.name, hit, seed, n))



# ---------------------------------------------------------------------------
# C16-C22: automatic formation and online node addition (K33/W157). The
# rules are docs/status/k33-membership-protocol-20261023.md M1-M9; every
# `explore` enumerates all interleavings of the scenario and settles every
# k-th reachable state to the converged membership.

A3 = [('10.0.0.1:18901', 1), ('10.0.0.2:18902', 2), ('10.0.0.3:18903', 3)]
N4, N4B, N5 = ('10.0.0.4:18904', 4), ('10.0.0.9:18909', 4), ('10.0.0.5:18905', 5)


def growth(actors, label, **kw):
    t = time.time()
    st = M.Growth(actors, **kw).explore()
    print('    %-34s %7d states %8d transitions, %6d settled, max %d members, %d joint, %.1fs'
          % (label, st['states'], st['transitions'], st['settled'], st['max_count'], st['joint'],
             time.time() - t))
    return st


@test
def growth_one_or_two_nodes_stay_unready():
    for n in (1, 2):
        st = growth(A3[:n], '%d node(s), 2 crashes' % n, crashes=2)
        assert st['formed'] == 0
    g, s = M.growth_run(A3[:2], steps=[('up', 0), ('up', 1), ('join', 1)])
    assert not any(g.formed(s, a) for a in range(2)) and s.g0 is None and not s.inflight


@test
def growth_automatic_formation_exhaustive():
    """M4: three joined members form without an operator step; every crash
    pair and every lost proposal still converges to one record."""
    st = growth(A3, 'formation, 2 crashes', crashes=2, settle_every=3)
    assert st['formed'] and st['max_count'] == 3
    g, s = M.growth_run(A3, steps=[('up', 0), ('up', 1), ('up', 2), ('join', 1), ('join', 2),
                                   ('propose-mkfs', 0), ('apply-mkfs', None)])
    s = g.settle(s)
    assert g.converged(s)[0] and s.g0.count == 3 and s.g0.revision == 1
    assert s.g2cfg == (frozenset({1, 2, 3}), None)


@test
def growth_automatic_ids_formation_exhaustive():
    """M4' (target design): the seed allocates ids, every candidate promises
    durably before the formation commits; competing restarts converge."""
    st = growth([('a', 0), ('b', 0), ('c', 0)], 'auto-id formation, 2 crashes', mode='auto',
                crashes=2, settle_every=3)
    assert st['max_count'] == 3


@test
def growth_add_fourth_node_exhaustive():
    """M5/M6: pending joiner, committed revision, SHARD2 {1,2,3} -> {2,3,4}
    with catch-up and joint consensus, restart after every durable step."""
    st = growth(A3 + [N4], 'add node 4, 2 crashes', crashes=2, settle_every=11, from_formed=3)
    assert st['max_count'] == 4 and st['joint']
    g = M.Growth(A3 + [N4], from_formed=3)
    s = g.settle(g.init)
    assert s.g0.count == 4 and s.g0.revision == 2 and s.g2cfg == (frozenset({2, 3, 4}), None)


@test
def growth_competing_and_retried_joins_exhaustive():
    """Two stores claim id 4 (manual ids); duplicate HELLOs, retried and lost
    proposals: exactly one identity commits, the other is refused."""
    st = growth(A3 + [N4, N4B], 'two claims of id 4, 1 crash', crashes=1, settle_every=7,
                from_formed=3)
    assert st['refused'] and st['max_count'] == 4


@test
def growth_concurrent_automatic_joins_exhaustive():
    """Target design: concurrent joiners get distinct ids by the committed
    revision order; a stale proposal conflicts and is retried."""
    st = growth(A3 + [('d', 0), ('e', 0)], 'two auto-id joiners', mode='auto', crashes=0,
                settle_every=7, from_formed=3)
    assert st['max_count'] == 5
    st = growth(A3 + [('d', 0)], 'auto-id joiner, 2 crashes, 1 wipe', mode='auto', crashes=2,
                wipes=1, settle_every=11, from_formed=3)


@test
def growth_fifth_node_adds_storage_only_exhaustive():
    st = growth(A3 + [N4, N5], 'add node 5 after 4, 1 crash', crashes=1, settle_every=7,
                from_formed=4)
    assert st['max_count'] == 5 and not st['joint']
    st = growth(A3 + [N4, N5], 'add 4 and 5 together', crashes=0, settle_every=7, from_formed=3)
    g = M.Growth(A3 + [N4, N5], from_formed=3)
    s = g.settle(g.init)
    assert s.g0.count == 5 and s.g2cfg == (frozenset({2, 3, 4}), None)
    assert 4 not in s.g2att  # actor 4 is node 5: hosts no metadata group


@test
def growth_store_replacement_refused_member_stays():
    """M2: a wiped store is a new token; it is refused, the committed member
    stays a member (unreachable), group 2 waits on it instead of dropping it."""
    st = growth(A3 + [N4], 'add node 4, 1 crash, 1 wipe', crashes=1, wipes=1, settle_every=7,
                from_formed=3)
    assert st['refused']
    g, s = M.growth_run(A3 + [N4], from_formed=3, crashes=1, wipes=1,
                        steps=[('up', 3), ('propose-add', None), ('apply-add', None), ('crash', 3),
                               ('wipe', 3)])
    s = g.settle(s)
    assert s.refused[3] and s.g0.count == 4 and s.g2cfg == (frozenset({1, 2, 3}), None)
    assert g.converged(s)[0]


@test
def growth_retained_placement_and_usable_capacity():
    """M7: chunks keep the fragment nodes recorded at publication; new writes
    on the enlarged view use the new member."""
    st = growth(A3 + [N4], 'add node 4 with 2 writes', crashes=0, writes=2, settle_every=7,
                from_formed=3)
    g = M.Growth(A3 + [N4], from_formed=3)
    s = g.settle(g.init)
    ids = [m[0] for m in s.g0.members]
    on4 = [ino for ino in range(1000, 1100) if 4 in M.place_wide(ids, ino)]
    assert len(on4) > 40, 'node 4 receives about 3/4 of new chunks: %d of 100' % len(on4)
    moved = [ino for ino in range(1000, 1100) if M.place_wide([1, 2, 3], ino) != M.place_wide(ids, ino)]
    assert moved, 'recomputing old chunks would move them; the recorded nodes are authoritative'


def neg_growth(rules, expect, actors, **kw):
    try:
        M.Growth(actors, rules=rules, settle_every=0, **kw).explore()
    except Violation as v:
        assert v.name in expect, '%s: got %s' % (rules.name, v)
        print('    %s: %s after %s' % (rules.name, v.name, ' '.join(v.trace())))
        return
    raise AssertionError('%s: no violation' % rules.name)


@test
def neg_growth_hello_table_authority():
    neg_growth(M.NEG_HELLO_TABLE, ('voter_without_identity', 'divergent_membership'),
               A3 + [N4, N4B], from_formed=3, crashes=0)


@test
def neg_growth_direct_voter_change():
    neg_growth(M.NEG_DIRECT, ('disjoint_quorums',), A3 + [N4], from_formed=3, crashes=0)


@test
def neg_growth_voter_without_catch_up():
    neg_growth(M.NEG_NO_CATCHUP, ('uncaught_voter',), A3 + [N4], from_formed=3, crashes=0)


@test
def neg_growth_replica_before_identity():
    neg_growth(M.NEG_EARLY_VOTE, ('voter_without_identity',), A3 + [N4], from_formed=3, crashes=1)


@test
def neg_growth_no_store_token():
    neg_growth(M.NEG_NO_TOKEN, ('store_not_member',), A3 + [N4], from_formed=3, crashes=1, wipes=1)


@test
def neg_growth_recomputed_placement():
    neg_growth(M.NEG_RECOMPUTE, ('read_misplaced',), A3 + [N4], from_formed=3, crashes=0, writes=1)


@test
def neg_growth_two_member_formation():
    neg_growth(M.NEG_FORM_TWO, ('formation_below_three',), A3[:2], crashes=0)


@test
def neg_growth_drop_unreachable_member():
    neg_growth(M.NEG_DROP, ('membership_shrank',), A3 + [N4], from_formed=3, crashes=1)


def main(argv):
    global SEEDS
    k = None
    if '-k' in argv:
        k = argv[argv.index('-k') + 1]
    if '--seeds' in argv:
        SEEDS = int(argv[argv.index('--seeds') + 1])
    passed = failed = 0
    t0 = time.time()
    for fn in TESTS:
        if k and k not in fn.__name__:
            continue
        t = time.time()
        try:
            fn()
        except Exception as e:  # noqa: BLE001 - report every failing trace
            failed += 1
            print('FAIL %s: %s: %s' % (fn.__name__, type(e).__name__, str(e)[:2000]))
            continue
        passed += 1
        print('ok   %-66s %5.2fs' % (fn.__name__, time.time() - t))
    neg = sum(1 for fn in TESTS if fn.__name__.startswith('neg_') and (not k or k in fn.__name__))
    print('test_d39_model: %d passed, %d failed (%d negative controls), %.1fs'
          % (passed, failed, neg, time.time() - t0))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
