#!/usr/bin/env python3
"""D39 consolidated executable model (no daemon).

The rules modelled here are stated once in
docs/status/d39-consolidated-model-20261023.md; section references "C<n>"
point at that page, "§<n>" at the review history in
docs/status/d39-cluster-membership-proposal-20261022.md.

The model is a deterministic state machine. Every daemon thread is a Python
generator that yields at each scheduling point; the scheduler (Cluster.step)
advances one generator by one step. Check and register are separate steps
taken while the binding leaf lock is held, so a competing declaration is
schedulable only before the lock is taken or after registration (C7).

`Rules` selects the consolidated rules (CONSOLIDATED) or a historical rule
set (S9, S13, S15, S21, S29, S31) for negative controls. A historical set
reverts only the rules the named section had and later reviews superseded;
everything else stays consolidated.

`Monitor` checks the invariants after every step and raises Violation.
"""
import hashlib
import random
import re
from collections import deque
from dataclasses import dataclass, replace

# ---------------------------------------------------------------------------
# C1. Configuration file and canonical digest

MIN_MEMBERS, MAX_MEMBERS = 3, 16
CONF_RECORD_VERSION = 1
GROUPS = (0, 2)
_DEC = re.compile(r'(0|[1-9][0-9]*)\Z')


class ConfigError(ValueError):
    pass


def canonical_endpoint(text):
    """Return the canonical advertised endpoint or raise ConfigError.

    Only canonical dotted IPv4 with a decimal port is an identity; wildcard,
    broadcast, host names and non-canonical numerals are refused (§6 item 6).
    """
    host, sep, port = text.rpartition(':')
    if not sep or not host:
        raise ConfigError('endpoint %r has no port' % text)
    parts = host.split('.')
    if len(parts) != 4 or not all(_DEC.match(p) and int(p) <= 255 for p in parts):
        raise ConfigError('endpoint %r is not a canonical dotted IPv4 address' % text)
    if host in ('0.0.0.0', '255.255.255.255'):
        raise ConfigError('endpoint %r is a wildcard/broadcast address' % text)
    if not _DEC.match(port) or not 1 <= int(port) <= 65535:
        raise ConfigError('endpoint %r has no canonical port 1..65535' % text)
    return '%s:%s' % (host, port)


@dataclass(frozen=True)
class Config:
    members: tuple  # ((id, endpoint), ...) sorted by id

    @property
    def n(self):
        return len(self.members)

    @property
    def ids(self):
        return [m for m, _ in self.members]

    def endpoint(self, mid):
        return dict(self.members).get(mid)

    def canonical(self):
        out = 'efs-cluster-config %d\n' % CONF_RECORD_VERSION
        out += ''.join('%d %s\n' % (m, e) for m, e in self.members)
        return out.encode()

    def digest(self):
        return hashlib.sha256(self.canonical()).hexdigest()


def parse_config(text):
    members, endpoints = {}, {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 2:
            raise ConfigError('line %d: expected "<id> <addr>:<port>"' % lineno)
        sid, sep = fields
        if not _DEC.match(sid):
            raise ConfigError('line %d: member id %r is not a canonical decimal' % (lineno, sid))
        mid = int(sid)
        ep = canonical_endpoint(sep)
        if mid in members:
            raise ConfigError('line %d: duplicate member id %d' % (lineno, mid))
        if ep in endpoints:
            raise ConfigError('line %d: endpoint %s already belongs to member %d'
                              % (lineno, ep, endpoints[ep]))
        members[mid], endpoints[ep] = ep, mid
    ids = sorted(members)
    if not MIN_MEMBERS <= len(ids) <= MAX_MEMBERS:
        raise ConfigError('%d members; a cluster has %d..%d' % (len(ids), MIN_MEMBERS, MAX_MEMBERS))
    if ids != list(range(1, len(ids) + 1)):
        raise ConfigError('member ids must be exactly 1..%d' % len(ids))
    return Config(tuple((m, members[m]) for m in ids))


def default_conf(n, base_port=18900):
    return ''.join('%d 10.0.0.%d:%d\n' % (i, 200 + (i + 1) // 2, base_port + i) for i in range(1, n + 1))


def group_voters(g, n):
    """efs_md_group_voters: n <= 3 -> all; else group 0 = {1,2,3}, group 2 = {2,3,4}."""
    if n <= 3:
        return frozenset(range(1, n + 1))
    return frozenset((1, 2, 3)) if g == 0 else frozenset((2, 3, 4))


def shard_group(key):
    return 0 if key & 1 else 2


# ---------------------------------------------------------------------------
# C2. Store ownership and the durable boot counter

BOOT_MAGIC, BOOT_VERSION = 'EFSBOOT', 1
COUNTER_MAX = 2 ** 64 - 1


class StartRefused(Exception):
    pass


def boot_encode(counter):
    return (BOOT_MAGIC, BOOT_VERSION, counter, _boot_sum(counter))


def _boot_sum(counter):
    return hashlib.sha256(b'%s %d %d' % (BOOT_MAGIC.encode(), BOOT_VERSION, counter)).hexdigest()[:8]


def boot_decode(rec):
    if not isinstance(rec, tuple) or len(rec) != 4:
        raise StartRefused('boot counter is short or unreadable')
    magic, ver, counter, ck = rec
    if magic != BOOT_MAGIC or ver != BOOT_VERSION:
        raise StartRefused('boot counter has an unsupported encoding')
    if not isinstance(counter, int) or not 0 <= counter <= COUNTER_MAX or ck != _boot_sum(counter):
        raise StartRefused('boot counter checksum mismatch')
    return counter


class RaftDisk:
    def __init__(self):
        self.term, self.vote = 0, None
        self.base, self.base_term = 0, 0
        self.log = []
        self.snap = None  # KV copy at base


class KV:
    """A group's applied state machine; durable at apply (C10, C12)."""

    def __init__(self):
        self.applied = 0
        self.pair, self.pair_index = None, 0
        self.receipts = {}   # opid -> (result, index)
        self.data = {}
        self.published = {}  # fid -> opid
        self.effects = []    # (index, opid) for the duplicate-publication invariant

    def copy(self):
        k = KV()
        k.applied, k.pair, k.pair_index = self.applied, self.pair, self.pair_index
        k.receipts, k.data = dict(self.receipts), dict(self.data)
        k.published, k.effects = dict(self.published), list(self.effects)
        return k

    def apply(self, index, cmd, g):
        op = cmd[0]
        self.applied = index
        if op == 'NOOP':
            return 'noop'
        if op in ('MKFS', 'SALT'):
            # C12: compare-and-install on the complete pair.
            if (op == 'MKFS') != (g == 0):
                return 'wrong group'
            if self.pair is None:
                self.pair, self.pair_index = cmd[1], index
                return 'installed'
            return 'same' if self.pair == cmd[1] else 'conflict'
        if op in ('APP', 'PUBLISH'):
            opid = cmd[1]
            if opid in self.receipts:
                return 'duplicate'
            if op == 'APP':
                self.data[cmd[2]] = cmd[3]
                res = 'ok'
            else:
                fid = cmd[2]
                if fid in self.published:
                    res = 'already published'
                else:
                    self.published[fid] = opid
                    res = 'ok'
            self.receipts[opid] = (res, index)
            self.effects.append((index, opid))
            return res
        return 'unsupported'


class Store:
    """Everything that survives a process: the store directory (C2)."""

    def __init__(self, member):
        self.member = member
        self.owner = None
        self.initialized = False
        self.boot = None          # None: missing file
        self.fail_fsync = False
        self.formation_state = 'unformed'
        self.formation_pair = None
        self.raft = {}
        self.kv = {}
        self.frags = {}           # fid -> 'durable' | 'orphan'


def start_process(store, pid, fresh=0):
    """Exclusive ownership, then the checked durable bump (C2, §19 R5', §21, §23)."""
    if store.owner is not None:
        raise StartRefused('store of member %d is owned by process %s' % (store.member, store.owner))
    store.owner = pid
    try:
        if not store.initialized:
            if store.boot is not None:
                raise StartRefused('uninitialized store already has a boot counter')
            if store.fail_fsync:
                raise StartRefused('boot counter initialization failed')
            store.boot = boot_encode(0)
            store.initialized = True
        elif store.boot is None:
            raise StartRefused('initialized store has no boot counter')
        saved = boot_decode(store.boot)
        if saved >= COUNTER_MAX:
            raise StartRefused('boot counter exhausted')
        nxt = max(fresh, saved + 1)
        if nxt > COUNTER_MAX:
            raise StartRefused('boot counter exhausted')
        if store.fail_fsync:
            raise StartRefused('boot counter persist failed')
        store.boot = boot_encode(nxt)
        return nxt
    except StartRefused:
        store.owner = None
        raise


# ---------------------------------------------------------------------------
# Rules: the consolidated set and the historical sets for negative controls


@dataclass(frozen=True)
class Rules:
    name: str
    complete: str = 'both_groups'        # S9: 'hosted_groups'
    formed_requires_pair: bool = False   # S9: a formed admitter refuses pair=none
    mgmt_role: bool = True               # S9: mgmt is an ordinary client
    binding: str = 'member'              # S13/S15: 'connection'
    frame_check: str = 'role_predicate'  # S13: 'declaration_only'; S15: 'per_conn_generation'
    check_at: str = 'effect'             # S13/S15: 'dispatch'
    validate_decl_reply: bool = True     # S13: False
    drain: str = 'hwm'                   # S21: 'old_tuple'
    fence: str = 'check_register'        # S29: 'atomic_load'
    snapshot_term: str = 'rpc_term'      # S21: 'included_vs_current'
    raft_counter_exception: bool = True  # S31: False


CONSOLIDATED = Rules('consolidated')
# §9 had three rules later superseded; each negative control reverts one of them.
S9_PAIR = replace(CONSOLIDATED, name='§9 formed admission requires the pair', formed_requires_pair=True)
S9_LOCAL = replace(CONSOLIDATED, name='§9 formed on hosted groups only', complete='hosted_groups')
S9_CLIENTS = replace(CONSOLIDATED, name='§9 mgmt is an ordinary client', mgmt_role=False)
S13 = replace(CONSOLIDATED, name='§13', binding='connection', frame_check='declaration_only',
              check_at='dispatch', validate_decl_reply=False)
S15 = replace(CONSOLIDATED, name='§15', binding='connection', frame_check='per_conn_generation',
              check_at='dispatch')
S21 = replace(CONSOLIDATED, name='§21', drain='old_tuple', snapshot_term='included_vs_current')
S29 = replace(CONSOLIDATED, name='§29', fence='atomic_load')
S31 = replace(CONSOLIDATED, name='§31', raft_counter_exception=False)

PEER_CONTROL = {'RAFT', 'FR_REQ', 'STATUS'}
PEER_APP = {'FWD_REQ'}
MGMT_ALLOW = {'RAFT_MKFS', 'FORMATION_READ', 'STATUS', 'RAFT_STEP_DOWN'}
CLIENT_ALLOW = {'APP', 'PUT', 'PUBLISH'}


class Violation(AssertionError):
    def __init__(self, name, detail):
        AssertionError.__init__(self, '%s: %s' % (name, detail))
        self.name = name


# ---------------------------------------------------------------------------
# Scheduling primitives


class Step:
    kind = 'step'

    def __init__(self, stage):
        self.stage = stage

    def ready(self):
        return True


class Wait:
    kind = 'wait'

    def __init__(self, stage, pred):
        self.stage, self.pred = stage, pred

    def ready(self):
        return bool(self.pred())


class Lock:
    """Next step acquires the node's binding leaf lock (C7)."""
    kind = 'lock'

    def __init__(self, stage, node):
        self.stage, self.node = stage, node

    def ready(self):
        return self.node.lock is None


class Thread:
    def __init__(self, c, owner, name, gen, prio=0, peek=None):
        self.c, self.owner, self.name, self.prio = c, owner, name, prio
        self.gen, self._peek = gen, peek
        self.done = False
        try:
            self.cond = next(gen)
        except StopIteration:
            self.done, self.cond = True, Step('done')

    @property
    def alive(self):
        return not self.done and getattr(self.owner, 'alive', True)

    @property
    def stage(self):
        return self.cond.stage

    @property
    def node_id(self):
        return getattr(self.owner, 'id', None)

    def peek(self):
        return self._peek() if self._peek else None

    def ready(self):
        return self.alive and self.cond.ready()

    def __repr__(self):
        return '<%s %s @%s>' % (self.name, getattr(self.owner, 'id', self.owner), self.stage)


# ---------------------------------------------------------------------------
# Identity, bindings, frames, connections


@dataclass(frozen=True)
class Tag:
    member: int
    inc: int
    counter: int
    digest: str
    pair: object

    def tup(self):
        return (self.inc, self.counter, self.pair)


@dataclass(frozen=True)
class Binding:
    inc: int
    counter: int
    pair: object

    def tup(self):
        return (self.inc, self.counter, self.pair)


def f_opid(f):
    if f is None or f.kind not in ('FWD_REQ', 'CLI_REQ'):
        return None
    return f.body.get('args', {}).get('opid')


def conflict(a, b):
    return a is not None and b is not None and a != b


def channel(remote_pair, local_pair):
    if remote_pair is None and local_pair is None:
        return 'bootstrap'
    if remote_pair is None or local_pair is None:
        return 'catchup'
    return 'formed' if remote_pair == local_pair else None


class Frame:
    _ids = 0

    def __init__(self, kind, tag, body, src, intended=None):
        Frame._ids += 1
        self.id, self.kind, self.tag, self.body, self.src = Frame._ids, kind, tag, body, src
        self.intended = intended

    @property
    def msg(self):
        return self.body.get('m', {}).get('t') if self.kind == 'RAFT' else None

    def __repr__(self):
        return '<F%d %s%s from %s %s>' % (self.id, self.kind, '/' + self.msg if self.msg else '',
                                         self.src, self.tag.tup() if self.tag else '')


class Conn:
    def __init__(self, cid, declarer, admitter, role, intended, dpid, apid):
        self.id, self.declarer, self.admitter, self.role = cid, declarer, admitter, role
        self.intended, self.dpid, self.apid = intended, dpid, apid
        self.fwd, self.rev = deque(), deque()
        self.state = 'declaring'   # declaring | ready | refused | discarded
        self.retired = False
        self.dead = False
        self.admitted = False
        self.peer = None
        self.bound = None          # S13/S15 per-connection binding
        self.pending = 0
        self.refusal = None

    def __repr__(self):
        return '<C%d %s->%s %s %s%s>' % (self.id, self.declarer, self.admitter, self.role, self.state,
                                        ' dead' if self.dead else ' retired' if self.retired else '')


class Item:
    def __init__(self, kind, frame=None, conn=None, member=None, inc=None):
        self.kind, self.frame, self.conn, self.member, self.inc = kind, frame, conn, member, inc
        self.noted = False

    def __repr__(self):
        return '<I %s %r>' % (self.kind, self.frame)


class Reg:
    """An effect record (C7): identity authority for named effects only."""

    def __init__(self, kind, tag, effects, seq, op=None, registered=True):
        self.kind, self.tag, self.effects, self.seq, self.op = kind, tag, frozenset(effects), seq, op
        self.registered = registered


class Op:
    """An admitted operation: a token, later possibly an obligation (C8)."""
    _ids = 0

    def __init__(self, node, kind, remote, tup, effects, units, conn=None, body=None):
        Op._ids += 1
        self.id, self.node, self.kind, self.remote = Op._ids, node, kind, remote
        self._tup = tup
        self._tup_hash = hash(tup)
        self.effects = frozenset(effects)
        self.units = units
        self.conn, self.body = conn, dict(body or {})
        self.owner = 'token'       # token | obligation | control | None
        self.resolver = None
        self.terminal = None
        self.state = 'admitted'
        self.accepted = False
        self.g = self.idx = self.term = None
        self.req_conn = None
        self.replied = False
        self.hops = 0
        self.result = None

    @property
    def tup(self):
        return self._tup

    @property
    def opid(self):
        return self.body.get('opid')

    def __repr__(self):
        return '<Op%d %s %s %s %s>' % (self.id, self.kind, self.state, self.owner, self.body)


# ---------------------------------------------------------------------------
# Raft (one group on one node). Only what the D39 traces need: elections,
# replication, commit, apply, hard-state persistence before dependent
# replies, ReadIndex, snapshots with the C9 predicate and suffix rule.


class Stage:
    def __init__(self, m, reg):
        self.sid, self.m = m['sid'], m
        self.chunks = set()
        self.installing = False
        self.reg = reg

    @property
    def ready(self):
        return len(self.chunks) == self.m['total']


class Raft:
    def __init__(self, node, g, voters):
        self.node, self.g, self.voters = node, g, voters
        self.me = node.id
        d = node.store.raft.setdefault(g, RaftDisk())
        self.disk = d
        self.term, self.vote = d.term, d.vote
        self.base, self.base_term = d.base, d.base_term
        self.log = list(d.log)
        self.persisted = self.last_index()
        self.kv = node.store.kv.setdefault(g, KV())
        self.commit = self.applied = self.kv.applied
        self.role, self.leader = 'follower', None
        self.votes, self.next, self.match = set(), {}, {}
        self.silence, self.last_tick = 0, -1
        self.peer_boot = {}
        self.reads, self.read_round = {}, 0
        self.noop_index = None
        self.dirty = False
        self.held = []
        self.stage = None
        self.snap_sent = {}

    # log helpers
    def last_index(self):
        return self.base + len(self.log)

    def term_at(self, i):
        if i == self.base:
            return self.base_term
        if i < self.base or i > self.last_index():
            return None
        return self.log[i - self.base - 1][0]

    def entry(self, i):
        return self.log[i - self.base - 1]

    def last_term(self):
        return self.term_at(self.last_index())

    def majority(self):
        return len(self.voters) // 2 + 1

    def needs_persist(self):
        return self.dirty or self.held or self.persisted < self.last_index()

    def out(self, dst, msg, hold=False):
        msg = dict(msg, term=self.term)
        if hold or self.needs_persist() and msg['t'] in ('VOTE_REP', 'AE_REP', 'VOTE_REQ'):
            self.held.append((dst, msg))
        else:
            self.node.send_peer(dst, 'RAFT', {'g': self.g, 'm': msg})

    def set_term(self, t):
        if t > self.term:
            self.term, self.vote = t, None
            self.dirty = True
            if self.role != 'follower':
                self.step_down()

    def step_down(self):
        self.role = 'follower'
        self.reads.clear()
        self.node.after_raft(self.g)

    # persistence cohort (C11, §32/§33: hard state before dependent replies)
    def capture(self):
        cap = dict(term=self.term, vote=self.vote, base=self.base, base_term=self.base_term,
                   log=list(self.log), held=self.held)
        self.held, self.dirty = [], False
        return cap

    def commit_persist(self, cap):
        d = self.disk
        d.term, d.vote, d.base, d.base_term, d.log = cap['term'], cap['vote'], cap['base'], cap['base_term'], cap['log']
        last = cap['base'] + len(cap['log'])
        p = min(last, self.last_index())
        while p > self.base and self.term_at(p) != (cap['log'][p - cap['base'] - 1][0]
                                                    if p > cap['base'] else cap['base_term']):
            p -= 1
        self.persisted = max(p, self.base) if p >= self.base else self.base
        for dst, msg in cap['held']:
            if msg['t'] == 'VOTE_REP' and msg.get('granted'):
                if d.term < msg['term'] or d.term == msg['term'] and d.vote != dst:
                    raise Violation('vote_not_durable', (self.me, dst, msg))
            self.node.send_peer(dst, 'RAFT', {'g': self.g, 'm': msg})
        if self.role == 'leader':
            self.match[self.me] = self.persisted
            self.advance_commit()

    # timers
    def tick(self):
        if self.role == 'leader':
            self.broadcast()
            return
        self.silence += 1
        if self.silence >= 3 + 2 * self.me:
            self.campaign()

    def campaign(self):
        self.term += 1
        self.vote, self.role, self.votes = self.me, 'candidate', {self.me}
        self.dirty, self.silence, self.leader = True, 0, None
        for v in sorted(self.voters - {self.me}):
            self.out(v, dict(t='VOTE_REQ', lli=self.last_index(), llt=self.last_term()), hold=True)

    def become_leader(self):
        self.role, self.leader = 'leader', self.me
        self.node.c.monitor.leader(self.node, self.g, self.term)
        self.next = {v: self.last_index() + 1 for v in self.voters}
        self.match = {v: 0 for v in self.voters}
        self.log.append((self.term, ('NOOP',)))
        self.noop_index = self.last_index()
        self.match[self.me] = self.persisted
        self.broadcast()
        self.node.after_raft(self.g)

    def propose(self, cmd):
        if self.role != 'leader':
            return None
        self.log.append((self.term, cmd))
        self.broadcast()
        return self.last_index(), self.term

    def broadcast(self, rid=None):
        for v in sorted(self.voters - {self.me}):
            self.send_ae(v, rid)

    def send_ae(self, f, rid=None):
        nxt = self.next.get(f, self.last_index() + 1)
        if nxt <= self.base:
            self.send_snapshot(f)
            return
        prev = nxt - 1
        ents = [self.entry(i) for i in range(nxt, min(self.last_index(), prev + 4) + 1)]
        self.out(f, dict(t='AE_REQ', prev_i=prev, prev_t=self.term_at(prev), ents=ents,
                         commit=self.commit, rid=self.read_round if rid is None else rid))

    def send_snapshot(self, f):
        if self.disk.snap is None:
            return
        now = self.node.c.now
        if self.snap_sent.get(f, -99) > now - 3:
            return
        self.snap_sent[f] = now
        sid = (self.me, self.term, self.base)
        for chunk in (0, 1):
            self.out(f, dict(t='SNAP_REQ', sid=sid, chunk=chunk, total=2, last_i=self.base,
                             last_t=self.base_term, voters=sorted(self.voters), commit=self.commit,
                             state=self.disk.snap.copy() if chunk == 1 else None))

    def advance_commit(self):
        if self.role != 'leader':
            return
        self.match[self.me] = self.persisted
        for n in range(self.last_index(), self.commit, -1):
            if self.term_at(n) != self.term:
                continue
            if sum(1 for v in self.voters if self.match.get(v, 0) >= n) >= self.majority():
                self.set_commit(n)
                break

    def set_commit(self, n):
        if n > self.commit:
            old, self.commit = self.commit, n
            self.node.c.monitor.commit_advanced(self, old, n)

    # receive (Raft owner validation; identity authority came from the effect record)
    def on_msg(self, src, m, tag, reg):
        t = m['t']
        if reg is not None and reg.registered and t not in reg.effects:
            raise Violation('effect_outside_record', (self.me, t, reg.effects))
        if tag.inc < self.peer_boot.get(src, 0):
            self.node.refuse('raft boot fence', src, t)
            return
        self.peer_boot[src] = max(self.peer_boot.get(src, 0), tag.inc)
        if src not in self.voters:
            return
        if m['term'] > self.term:
            self.set_term(m['term'])
        getattr(self, 'on_' + t)(src, m, reg)

    def on_VOTE_REQ(self, src, m, reg):
        up = (m['llt'], m['lli']) >= (self.last_term(), self.last_index())
        grant = m['term'] == self.term and self.vote in (None, src) and up
        if grant:
            self.vote, self.dirty, self.silence = src, True, 0
        self.out(src, dict(t='VOTE_REP', granted=grant), hold=self.dirty)

    def on_VOTE_REP(self, src, m, reg):
        if self.role == 'candidate' and m['term'] == self.term and m['granted']:
            self.votes.add(src)
            if len(self.votes) >= self.majority():
                self.become_leader()

    def on_AE_REQ(self, src, m, reg):
        if m['term'] < self.term:
            self.out(src, dict(t='AE_REP', ok=False, match=0, hint=self.last_index(), rid=m['rid']))
            return
        if self.role != 'follower':
            self.step_down()
        self.leader, self.silence = src, 0
        prev, ents = m['prev_i'], list(m['ents'])
        if prev < self.base:
            skip = self.base - prev
            ents, prev = ents[skip:], self.base
            if skip > len(m['ents']):
                self.out(src, dict(t='AE_REP', ok=True, match=self.base, rid=m['rid']))
                return
            prev_t = self.base_term
        else:
            prev_t = m['prev_t']
        if self.term_at(prev) != prev_t:
            self.out(src, dict(t='AE_REP', ok=False, match=0, hint=min(self.last_index(), prev - 1),
                               rid=m['rid']))
            return
        i = prev
        for e in ents:
            i += 1
            have = self.term_at(i)
            if have is None:
                self.log.append(e)
            elif have != e[0]:
                if i <= self.commit:
                    raise Violation('committed_suffix_overwritten', (self.me, self.g, i))
                del self.log[i - self.base - 1:]
                self.persisted = min(self.persisted, i - 1)
                self.dirty = True
                self.log.append(e)
                self.node.after_raft(self.g)
        if m['commit'] > self.commit:
            self.set_commit(min(m['commit'], i))
        self.out(src, dict(t='AE_REP', ok=True, match=i, rid=m['rid']), hold=self.needs_persist())

    def on_AE_REP(self, src, m, reg):
        if self.role != 'leader' or m['term'] != self.term:
            return
        if m['ok']:
            self.match[src] = max(self.match.get(src, 0), m['match'])
            self.next[src] = self.match[src] + 1
            self.note_read_ack(src, m['term'], m['rid'])
            self.advance_commit()
        else:
            self.next[src] = max(1, min(self.next.get(src, 1) - 1, m['hint'] + 1))
            self.send_ae(src)

    def on_SNAP_REQ(self, src, m, reg):
        if m['term'] < self.term:
            self.out(src, dict(t='SNAP_REP', ok=False, match=self.applied))
            return
        if self.role != 'follower':
            self.step_down()
        self.leader, self.silence = src, 0
        if self.node.rules.snapshot_term == 'included_vs_current' and m['last_t'] < self.term:
            self.node.refuse('snapshot', src, 'snapshot term below current term (§21 rule)')
            return
        if self.stage is not None and self.stage.sid != m['sid']:
            if self.stage.installing:
                return
            self.stage = None
        if self.stage is None:
            self.stage = Stage(m, reg)
        self.stage.chunks.add(m['chunk'])
        if m['state'] is not None:
            self.stage.m = m

    def on_SNAP_REP(self, src, m, reg):
        if self.role == 'leader' and m['term'] == self.term:
            self.match[src] = max(self.match.get(src, 0), m['match'])
            self.next[src] = self.match[src] + 1
            self.advance_commit()

    def snapshot_verdict(self, m):
        """C9 predicate; returns None (install), 'no-import' or a refusal."""
        if m['last_t'] > m['term']:
            return 'last-included term above RPC term'
        if m['last_i'] > m['commit']:
            return 'snapshot above attested commit'
        if frozenset(m['voters']) != self.voters:
            return 'membership differs from configured voters'
        if m['last_i'] <= self.applied:
            return 'no-import'
        return None

    def install(self, m):
        st = m['state']
        keep = []
        if self.term_at(m['last_i']) == m['last_t']:
            keep = [self.entry(i) for i in range(m['last_i'] + 1, self.last_index() + 1)]
        self.base, self.base_term, self.log = m['last_i'], m['last_t'], keep
        kv = st.copy()
        self.node.store.kv[self.g] = self.kv = kv
        d = self.disk
        d.base, d.base_term, d.log, d.snap = self.base, self.base_term, list(keep), st.copy()
        d.term, d.vote = self.term, self.vote
        self.persisted = self.last_index()
        old = self.commit
        self.commit = max(self.commit, m['last_i'])
        self.applied = m['last_i']

    def compact(self):
        if self.applied <= self.base:
            return False
        keep = self.log[self.applied - self.base:]
        self.base_term, self.base = self.term_at(self.applied), self.applied
        self.log = keep
        d = self.disk
        d.snap, d.base, d.base_term = self.kv.copy(), self.base, self.base_term
        d.log = list(self.log[:max(0, self.persisted - self.base)])
        return True

    # ReadIndex (FORMATION_READ answers)
    def start_read(self, cb):
        if self.role != 'leader' or self.noop_index is None or self.commit < self.noop_index:
            return False
        self.read_round += 1
        self.reads[self.read_round] = dict(index=self.commit, acks={self.me}, term=self.term, cb=cb)
        self.broadcast(self.read_round)
        return True

    def note_read_ack(self, src, term, rid):
        if self.role != 'leader' or term != self.term:
            return
        for r, rd in self.reads.items():
            if r <= rid and rd['term'] == term:
                rd['acks'].add(src)
        self.check_reads()

    def check_reads(self):
        for r in sorted(self.reads):
            rd = self.reads[r]
            if len(rd['acks']) >= self.majority() and self.applied >= rd['index']:
                del self.reads[r]
                rd['cb'](self)


# ---------------------------------------------------------------------------
# The daemon process


class Node:
    def __init__(self, c, mid, inc, pid):
        self.c, self.id, self.inc, self.pid = c, mid, inc, pid
        self.rules = c.rules
        self.cfg = c.node_cfg.get(mid, c.cfg)
        self.digest = self.cfg.digest()
        self.endpoint = self.cfg.endpoint(mid)
        self.store = c.stores[mid]
        self.alive, self.stopped = True, None
        self.counter = 0
        self.salt = 'salt-%d-%d-%08x' % (mid, inc, c.rng.getrandbits(32))
        self.pair = self.store.formation_pair
        self.known = {0: None, 2: None}
        self.hosted = [g for g in GROUPS if mid in group_voters(g, self.cfg.n)]
        self.bindings, self.fence, self.pending = {}, {}, {}
        self.lock = None
        self.inbox = deque()
        self.raft = {g: Raft(self, g, group_voters(g, self.cfg.n)) for g in self.hosted}
        self.ops = {}
        self.others = [m for m in self.cfg.ids if m != mid]
        self.pool = {m: [] for m in self.others}
        self.outq = {m: deque() for m in self.others}
        self.retry_at = {m: 0 for m in self.others}
        self.fr = {}
        self.fd_next = 0
        self.refused = []
        self.last_write = None
        self.refuse_handoff = set()
        self.resolution_paused = set()
        self.batch = c.batch
        self._cur = None
        self.was_complete = False
        self.threads = []

    def boot(self):
        for g in self.hosted:
            kp = self.raft[g].kv.pair
            if kp is not None:
                if self.pair != kp:
                    return self.stop('persisted pair %s conflicts with group %d record %s' % (self.pair, g, kp))
                self.known[g] = kp
        c = self.c
        add = self.threads.append
        add(Thread(c, self, 'pump', self.pump(), 0, peek=lambda: self.inbox[0] if self.inbox else None))
        add(Thread(c, self, 'persist', self.persister(), 0))
        add(Thread(c, self, 'early_reads', self.early_reads(), 0))
        add(Thread(c, self, 'resolver', self.resolver(), 0))
        add(Thread(c, self, 'formation', self.formation_driver(), 1))
        for g in self.hosted:
            add(Thread(c, self, 'apply%d' % g, self.applier(g), 0))
            add(Thread(c, self, 'snap%d' % g, self.snap_installer(g), 0))
            add(Thread(c, self, 'timer%d' % g, self.timer(g), 1))
        for m in self.others:
            add(Thread(c, self, 'send%d' % m, self.pool_sender(m), 0,
                       peek=lambda m=m: self.outq[m][0] if self.outq[m] else None))
            add(Thread(c, self, 'open%d' % m, self.pool_opener(m), 1))
        self.update_complete()
        return self

    # -- identity -----------------------------------------------------------
    def tag(self):
        return Tag(self.id, self.inc, self.counter, self.digest, self.pair)

    @property
    def state(self):
        return self.store.formation_state

    @property
    def complete(self):
        if self.rules.complete == 'hosted_groups' and self.hosted:
            return all(self.known[g] is not None for g in self.hosted)
        return self.known[0] is not None and self.known[0] == self.known[2]

    def stop(self, reason):
        self.stopped = reason
        self.c.monitor.note('stop', self.id, reason)
        self.c.kill(self.id)
        return None

    def unlock(self):
        assert self.lock is not None
        self.lock = None

    def identity_event(self, pair, how):
        """Local pair learned or formation record installed (C2, C6). Under the lock."""
        assert self.lock is not None, 'identity change outside the binding lock'
        if conflict(self.pair, pair):
            self.stop('%s pair %s conflicts with local pair %s' % (how, pair, self.pair))
            return False
        if self.pair is None:
            self.pair = pair
            if self.store.formation_pair is None:
                self.store.formation_pair = pair
            self.store.formation_state = 'forming' if self.store.formation_state == 'unformed' else self.store.formation_state
        self.counter += 1
        self.c.monitor.note('identity', self.id, how, self.counter)
        self.retire_pool()
        self.update_complete()
        return True

    def retire_pool(self):
        for m in self.others:
            for cn in self.pool[m]:
                cn.retired = True
            self.pool[m] = []

    def update_complete(self):
        if self.complete and not self.was_complete:
            self.was_complete = True
            if self.store.formation_state != 'formed':
                self.store.formation_state = 'formed'
                if self.rules.formed_requires_pair:
                    for cn in self.c.conns.values():
                        if cn.admitter == self.id and cn.role == 'peer' and cn.bound is None and not cn.dead \
                                and self.bindings.get(cn.peer) and self.bindings[cn.peer].pair is None:
                            self.c.close(cn)
            self.c.monitor.note('complete', self.id)

    # -- R2'' (C5) and publication (C6) ------------------------------------
    def declaration_predicate(self, member, endpoint, digest, inc, counter, pair):
        if member not in self.cfg.ids or member == self.id:
            return 'refuse', 'member %s not configured' % member
        if endpoint != self.cfg.endpoint(member):
            return 'refuse', 'endpoint %s is not the configured record %s of member %d' % (
                endpoint, self.cfg.endpoint(member), member)
        if digest != self.digest:
            return 'refuse', 'digest %s differs from local %s' % (digest[:12], self.digest[:12])
        if conflict(self.pair, pair):
            return 'refuse', 'pair %s conflicts with local pair %s' % (pair, self.pair)
        if self.rules.formed_requires_pair and self.state == 'formed' and pair is None:
            return 'refuse', 'formed admitter requires the pair (§9)'
        if self.rules.binding == 'connection':
            return 'accept', None
        b = self.newest(member)
        if b is not None and b.pair is not None and pair != b.pair:
            return 'refuse', 'member %d once bound to %s may not redeclare %s' % (member, b.pair, pair)
        if b is None or inc > b.inc:
            return 'accept', None
        if inc < b.inc:
            return 'refuse', 'stale incarnation %d < %d' % (inc, b.inc)
        if counter > b.counter:
            return 'accept', None
        if counter < b.counter:
            return 'refuse', 'stale counter %d < %d' % (counter, b.counter)
        if pair == b.pair:
            return 'idempotent', None
        return 'refuse', 'equal counter with a different pair'

    def newest(self, member):
        return self.pending.get(member) or self.bindings.get(member)

    def publish_binding(self, member, inc, counter, pair):
        assert self.lock is not None, 'binding publication outside the lock'
        new = Binding(inc, counter, pair)
        old = self.bindings.get(member)
        self.c.monitor.decl(self, member, new)
        if self.rules.drain == 'old_tuple' and old is not None and old.tup() != new.tup() \
                and self.inflight(member) > 0:
            self.pending[member] = new
            return
        self.pending.pop(member, None)
        self.bindings[member] = new
        self.fence[member] = inc  # C11: binding fields first, then the fence word
        self.inbox.append(Item('FENCE', member=member, inc=inc))

    def on_declaration(self, conn, f):
        b = f.body
        role = b['role']
        if role == 'mgmt' and self.rules.mgmt_role:
            if b['digest'] != self.digest:
                return self.decl_refused(conn, 'mgmt digest %s differs from local %s' % (b['digest'][:12], self.digest[:12]))
            conn.admitted = True
            return self.decl_reply(conn, 'mgmt')
        if role in ('client', 'mgmt'):
            if not self.complete:
                return self.decl_refused(conn, 'not formed')
            conn.role = 'client'
            conn.admitted = True
            return self.decl_reply(conn, 'client')
        verdict, why = self.declaration_predicate(b['member'], b['endpoint'], b['digest'],
                                                  b['inc'], b['counter'], b['pair'])
        if verdict == 'refuse':
            return self.decl_refused(conn, why)
        conn.admitted, conn.peer = True, b['member']
        if self.rules.binding == 'connection':
            conn.bound = (b['inc'], b['counter'], b['pair'])
            cur = self.bindings.get(b['member'])
            if cur is None or (b['inc'], b['counter']) > (cur.inc, cur.counter):
                self.c.monitor.decl(self, b['member'], Binding(b['inc'], b['counter'], b['pair']))
                self.bindings[b['member']] = Binding(b['inc'], b['counter'], b['pair'])
        elif verdict == 'accept':
            self.publish_binding(b['member'], b['inc'], b['counter'], b['pair'])
        return self.decl_reply(conn, channel(b['pair'], self.pair))

    def decl_reply(self, conn, chan):
        t = self.tag()
        return dict(ok=True, member=self.id, endpoint=self.endpoint, digest=self.digest,
                    inc=t.inc, counter=t.counter, pair=t.pair, channel=chan)

    def decl_refused(self, conn, why):
        conn.refusal = why
        self.refuse('declaration', conn.declarer, why)
        return dict(ok=False, reason=why, member=self.id, digest=self.digest, pair=self.pair)

    def on_decl_reply(self, conn, f):
        """The declarer validates the responder (C5): intended peer, then R2''."""
        b = f.body
        if not b['ok']:
            conn.state, conn.refusal = 'refused', b['reason']
            self.refuse('declaration refused by %s' % conn.admitter, conn.intended, b['reason'])
            return
        if self.rules.validate_decl_reply:
            if b['member'] != conn.intended or b['endpoint'] != self.cfg.endpoint(conn.intended):
                return self.discard(conn, 'reply from member %s at %s, intended %d' % (
                    b['member'], b['endpoint'], conn.intended))
            verdict, why = self.declaration_predicate(b['member'], b['endpoint'], b['digest'],
                                                      b['inc'], b['counter'], b['pair'])
            if verdict == 'refuse':
                return self.discard(conn, why)
            if verdict == 'accept' and self.rules.binding == 'member':
                self.publish_binding(b['member'], b['inc'], b['counter'], b['pair'])
        conn.state = 'ready'

    def discard(self, conn, why):
        conn.state, conn.refusal, conn.retired = 'discarded', why, True
        self.refuse('declaration reply', conn.intended, why)
        if conn in self.pool.get(conn.intended, []):
            self.pool[conn.intended].remove(conn)

    def refuse(self, what, who, why):
        self.refused.append((what, who, why))
        self.c.monitor.note('refuse', self.id, what, who, why)

    # -- role-specific predicates (C7) --------------------------------------
    def pred_control(self, tag):
        if tag is None or tag.member not in self.cfg.ids or tag.member == self.id:
            return 'not a configured peer'
        if tag.digest != self.digest:
            return 'digest'
        b = self.bindings.get(tag.member)
        if b is None:
            return 'redeclare required'
        if tag.inc < b.inc:
            return 'stale incarnation'
        if tag.inc > b.inc:
            return 'redeclare required'
        if conflict(self.pair, tag.pair):
            return 'pair conflict'
        if b.pair is not None and tag.pair != b.pair:
            return 'downgrade or conflict against the binding'
        if not self.rules.raft_counter_exception and tag.counter != b.counter:
            return 'counter differs (§31 exact equality)'
        if self.rules.drain == 'old_tuple' and (tag.counter, tag.pair) != (b.counter, b.pair):
            return 'redeclare required (draining tuple only)'
        return None

    def pred_app(self, tag, kind):
        b = self.bindings.get(tag.member) if tag else None
        if b is None:
            return 'redeclare required'
        if tag.digest != self.digest:
            return 'digest'
        if tag.tup() != b.tup():
            return 'tuple %s is not the current binding %s' % (tag.tup(), b.tup())
        if tag.member in self.pending:
            return 'binding draining (§21)'
        if not self.complete:
            return 'not formed'
        if channel(b.pair, self.pair) != 'formed':
            return 'channel is not formed'
        if kind not in PEER_APP:
            return 'not in the peer allowlist'
        return self.capacity(tag.member, 1, self.retry_of(f_opid(self._cur)))

    def pred_client(self, conn, kind, units):
        if conn.role != 'client' or not conn.admitted:
            return 'not a client channel'
        if not self.complete:
            return 'not formed'
        if kind not in CLIENT_ALLOW:
            return 'not in the client allowlist'
        return self.capacity(conn.declarer, units, self.retry_of(f_opid(self._cur)))

    def pred_cont(self, tag, f):
        op = self.ops.get(f.body.get('ref'))
        if op is None or op.owner not in ('token', 'obligation', 'control'):
            return 'no owned operation'
        if tag is None or op.remote != tag.member:
            return 'not the operation peer'
        b = self.bindings.get(tag.member)
        if b is None or tag.inc != b.inc or tag.inc != op.tup[0]:
            return 'stale incarnation'
        if conflict(self.pair, tag.pair) or b.pair is not None and tag.pair != b.pair:
            return 'pair conflict'
        if f.kind not in op.effects:
            return 'effect %s not authorized by the operation' % f.kind
        return None

    def check(self, kind, f, conn=None):
        self._cur = f
        if kind == 'control':
            return self.pred_control(f.tag)
        if kind == 'app':
            return self.pred_app(f.tag, f.kind)
        if kind == 'client':
            return self.pred_client(conn, f.body['op'], 2 if f.body['op'] == 'PUT' else 1)
        if kind == 'cont':
            return self.pred_cont(f.tag, f)
        raise ValueError(kind)

    def capacity(self, who, units, held=None):
        """A retry of a held obligation reuses its reservation (C8): the obligation's units
        are counted as released when the bounds are checked."""
        total, mine = self.units() + units, self.units(who) + units
        if held is not None:
            total -= held.units
            if held.remote == who:
                mine -= held.units
        if total > self.c.total_bound or mine > self.c.member_bound:
            return 'BUSY'
        return None

    def retry_of(self, opid):
        """The obligation a retry with this identity resolves, if this node holds one."""
        if opid is None:
            return None
        for op in self.ops.values():
            if op.owner == 'obligation' and op.opid == opid and op.resolver in ('raft_entry', 'receipt'):
                return op
        return None

    def units(self, who=None):
        return sum(op.units for op in self.ops.values()
                   if op.owner in ('token', 'obligation') and (who is None or op.remote == who))

    def inflight(self, member):
        return sum(1 for op in self.ops.values() if op.owner == 'token' and op.remote == member)

    def effects_for(self, kind, f):
        if kind == 'control':
            return {f.msg} if f.kind == 'RAFT' else {f.kind}
        if kind == 'cont':
            return {f.kind}
        return {'admit'}

    def authorize(self, f, kind, conn=None):
        """Check-and-register (C7). Returns a Reg, or None when refused."""
        if self.rules.fence == 'atomic_load':
            why = self.check(kind, f, conn)  # §29: a load and compare, no registration
            yield Step('checked')
            if why:
                self.refuse(kind, f.src, why)
                return None
            return self.make_reg(kind, f, conn, registered=False)
        yield Lock('lock', self)
        why = self.check(kind, f, conn)
        yield Step('checked')            # still holding the lock
        if why:
            self.unlock()
            self.refuse(kind, f.src, why)
            return None
        reg = self.make_reg(kind, f, conn, registered=True)
        self.c.monitor.acquire(self, reg, f)
        self.unlock()
        yield Step('registered')
        return reg

    def make_reg(self, kind, f, conn, registered):
        reg = Reg(kind, f.tag, self.effects_for(kind, f), self.c.seq, registered=registered)
        if kind in ('app', 'client'):
            body = f.body
            units = 2 if body.get('op') == 'PUT' else 1
            remote = f.tag.member if kind == 'app' else conn.declarer
            tup = f.tag.tup() if kind == 'app' else ('client', conn.id)
            held = self.retry_of(f_opid(f))
            if held is not None:            # reservation transfer: obligation -> retry token
                held.units = 0
                self.finish(held, 'retried', 'taken over by a retry')
            reg.op = self.new_op(body.get('op', 'APP') if kind == 'client' else body['args'].get('op', 'APP'),
                                 remote, tup, {'PUT_IO'}, units, conn, dict(body.get('args', {}), ref=body['ref']))
        elif kind == 'cont':
            reg.op = self.ops[f.body['ref']]
        return reg

    def new_op(self, kind, remote, tup, effects, units, conn, body, owner='token'):
        op = Op(self, kind, remote, tup, effects, units, conn, body)
        op.owner = owner
        self.ops[op.id] = op
        self.c.monitor.op_created(self, op)
        return op

    # -- threads --------------------------------------------------------------
    def pump(self):
        while True:
            yield Wait('idle', lambda: self.inbox)
            batch = [self.inbox.popleft() for _ in range(min(self.batch, len(self.inbox)))]
            self.pump_batch = batch
            yield Step('taken')
            regs = []
            for it in batch:
                if it.kind == 'FENCE':
                    for r in self.raft.values():   # pump-owned peer_boot cache (C11)
                        r.peer_boot[it.member] = max(r.peer_boot.get(it.member, 0), it.inc)
                    continue
                if self.rules.check_at == 'dispatch':
                    regs.append((it, None))
                    continue
                reg = yield from self.authorize(it.frame, 'control')
                if reg is not None:
                    regs.append((it, reg))
            for it, reg in regs:
                self.raft_effect(it.frame, reg)
                yield Step('effect')

    def raft_effect(self, f, reg):
        self.c.monitor.effect(self, f, reg, 'control')
        g = f.body['g']
        if g in self.raft:
            self.raft[g].on_msg(f.tag.member, f.body['m'], f.tag, reg)
            self.after_raft(g)

    def early_reads(self):
        """host_note_queued_reads: read-proof inspection of queued acks (C7, C11)."""
        def pending():
            for it in self.inbox:
                if it.kind == 'RAFT' and not it.noted and it.frame.msg == 'AE_REP' and it.frame.body['m'].get('ok'):
                    return it
            return None
        while True:
            yield Wait('idle', lambda: pending() is not None)
            it = pending()
            it.noted = True
            yield Step('inspect')
            if self.rules.check_at == 'dispatch':
                reg = None
            else:
                reg = yield from self.authorize(it.frame, 'control')
                if reg is None:
                    continue
            self.c.monitor.effect(self, it.frame, reg, 'control')
            g, m = it.frame.body['g'], it.frame.body['m']
            if g in self.raft and m['term'] == self.raft[g].term:
                self.raft[g].note_read_ack(it.frame.tag.member, m['term'], m['rid'])
                self.after_raft(g)

    def persister(self):
        while True:
            yield Wait('idle', lambda: any(r.needs_persist() for r in self.raft.values()))
            cohort = [(r, r.capture()) for r in self.raft.values() if r.needs_persist()]
            yield Step('persist_io')    # h->mu released: declarations and receives proceed
            for r, cap in cohort:
                r.commit_persist(cap)
                self.after_raft(r.g)
            yield Step('persisted')

    def applier(self, g):
        r = self.raft[g]
        while True:
            yield Wait('idle', lambda: r.applied < min(r.commit, r.persisted)
                       and not (r.stage and r.stage.installing))
            yield Step('apply')
            if r.applied >= min(r.commit, r.persisted):
                continue
            i = r.applied + 1
            cmd = r.entry(i)[1]
            res = r.kv.apply(i, cmd, g)
            r.applied = i
            self.c.monitor.applied(self, g, i, cmd, res)
            if cmd[0] in ('MKFS', 'SALT') and res in ('installed', 'same'):
                yield Lock('install', self)
                pair = r.kv.pair
                ok = self.identity_event(pair, '%s apply in group %d' % (cmd[0], g))
                if ok:
                    self.known[g] = pair
                    self.update_complete()
                self.unlock()
                if not ok:
                    return
            r.check_reads()
            self.after_raft(g)

    def snap_installer(self, g):
        r = self.raft[g]
        while True:
            yield Wait('idle', lambda: r.stage is not None and r.stage.ready and not r.stage.installing)
            st = r.stage
            st.installing = True     # the group's install token: one installation at a time
            yield Step('install_begin')
            m = st.m
            verdict = r.snapshot_verdict(m)
            if verdict is None:
                yield Step('install_io')   # non-instantaneous; lock not held
                verdict = r.snapshot_verdict(m)
                if verdict is None:
                    self.c.monitor.effect(self, None, st.reg, 'snapshot')
                    r.install(m)
            if verdict not in (None, 'no-import'):
                self.refuse('snapshot', m['sid'], verdict)
            else:
                r.out(m['sid'][0], dict(t='SNAP_REP', ok=True, match=max(r.applied, m['last_i']
                                        if verdict is None else r.applied)))
            r.stage = None
            self.after_raft(g)

    def timer(self, g):
        r = self.raft[g]
        while True:
            yield Wait('tick', lambda: self.c.now > r.last_tick)
            r.last_tick = self.c.now
            r.tick()
            self.after_raft(g)

    def pool_sender(self, m):
        rr = [0]

        def ready():
            return [cn for cn in self.pool[m] if cn.state == 'ready' and not cn.dead and not cn.retired]
        while True:
            yield Wait('idle', lambda: self.outq[m] and ready())
            yield Step('send')
            conns = ready()
            if not conns or not self.outq[m]:
                continue
            kind, body, op = self.outq[m].popleft()
            cn = conns[rr[0] % len(conns)]
            rr[0] += 1
            f = Frame(kind, self.tag(), body, self.id, intended=m)
            cn.fwd.append(f)
            if op is not None:          # a request record's authority starts when it is sent
                b = self.bindings.get(m)
                if b is not None:
                    op._tup, op._tup_hash = b.tup(), hash(b.tup())
                op.req_conn = cn
                cn.pending += 1

    def pool_opener(self, m):
        while True:
            yield Wait('idle', lambda: len([cn for cn in self.pool[m] if not cn.dead and not cn.retired])
                       < self.c.pool_size and self.c.now >= self.retry_at[m])
            yield Step('open')
            self.pool[m] = [cn for cn in self.pool[m] if not cn.dead and not cn.retired]
            cn = self.c.connect(self, self.cfg.endpoint(m), 'peer', intended=m)
            if cn is None:
                self.retry_at[m] = self.c.now + 2
                continue
            self.pool[m].append(cn)
            t = self.tag()
            cn.fwd.append(Frame('DECL', t, dict(role='peer', member=self.id, endpoint=self.endpoint,
                                                digest=self.digest, inc=t.inc, counter=t.counter,
                                                pair=t.pair), self.id, intended=m))

    def send_peer(self, m, kind, body, op=None):
        q = self.outq[m]
        if kind == 'RAFT' and len(q) > 24:
            for i, (k, _, _) in enumerate(q):
                if k == 'RAFT':
                    del q[i]
                    break
        q.append((kind, body, op))

    def reply(self, conn, kind, body):
        if conn.dead:
            return
        conn.rev.append(Frame(kind, self.tag(), body, self.id))

    # -- connection handlers ---------------------------------------------------
    def handler_admit(self, conn):
        while True:
            yield Wait('recv', lambda: conn.fwd or conn.dead or conn.retired)
            if not conn.fwd:
                return
            f = conn.fwd.popleft()
            self.c.monitor.delivered(self, conn, f)
            yield Step('recv')
            if f.kind == 'DECL':
                yield Lock('decl', self)
                rep = self.on_declaration(conn, f)
                self.unlock()
                self.reply(conn, 'DECL_REP', rep)
                continue
            if not conn.admitted:
                self.refuse('undeclared frame', f.src, f.kind)
                continue
            if conn.role == 'peer':
                if f.kind == 'RAFT':
                    self.dispatch_raft(conn, f)
                elif f.kind == 'FR_REQ':
                    yield from self.serve_formation_read(conn, f, 'control')
                elif f.kind in PEER_APP:
                    yield from self.peer_app(conn, f)
                else:
                    self.refuse('peer allowlist', f.src, f.kind)
                    self.reply(conn, 'ERR', dict(ref=f.body.get('ref'), reason='not in the peer allowlist'))
            elif conn.role == 'mgmt':
                yield from self.mgmt_request(conn, f)
            else:
                yield from self.client_request(conn, f)

    def handler_reply(self, conn):
        while True:
            yield Wait('recv', lambda: conn.rev or conn.dead or conn.retired and conn.pending <= 0
                       and conn.state != 'declaring')
            if not conn.rev:
                return
            f = conn.rev.popleft()
            yield Step('recv')
            if f.kind == 'DECL_REP':
                yield Lock('decl_reply', self)
                self.on_decl_reply(conn, f)
                self.unlock()
                continue
            conn.pending -= 1
            self.c.monitor.note('reply', self.id, conn.id, f.kind, f.body.get('ref'), f.body.get('result'))
            reg = yield from self.authorize(f, 'cont')
            if reg is None:
                rec = self.ops.get(f.body.get('ref'))
                parent = self.ops.get(rec.body.get('parent')) if rec is not None and rec.kind == 'FWD' else None
                if parent is not None and parent.owner == 'token' and parent.body.get('fwd_ref') == rec.id:
                    yield Lock('reply_lost', self)     # the reply is unusable: outcome uncertain
                    rec.owner, rec.terminal = None, 'reply refused'
                    self.make_uncertain(parent, 'receipt')
                    self.unlock()
                continue
            self.c.monitor.effect(self, f, reg, 'cont')
            if reg.op.kind == 'FR':
                yield Lock('fr_install', self)   # a learned pair is an identity change (C6)
                self.resolve_reply(reg.op, f)
                if self.alive:
                    self.unlock()
            else:
                self.resolve_reply(reg.op, f)

    def dispatch_raft(self, conn, f):
        r = self.rules
        if r.check_at == 'dispatch':
            why = None
            if r.frame_check == 'per_conn_generation':
                b = conn.bound
                if conflict(self.pair, f.tag.pair):
                    why = 'pair conflict'
                elif b[2] is not None and f.tag.pair is None:
                    why = 'none frame on a pair-bound connection'
                elif (f.tag.inc, f.tag.counter) < (b[0], b[1]):
                    why = 'stale generation on this connection'
                else:
                    conn.bound = (f.tag.inc, f.tag.counter, f.tag.pair or b[2])
            if why:
                self.refuse('raft dispatch', f.src, why)
                return
        elif self.fence.get(f.tag.member, 0) > f.tag.inc:
            self.refuse('fence pre-filter', f.src, 'incarnation %d below fence %d' % (
                f.tag.inc, self.fence[f.tag.member]))
            return
        self.inbox.append(Item('RAFT', f, conn))

    # -- formation (C10) -------------------------------------------------------
    def serve_formation_read(self, conn, f, kind):
        g = f.body['g']
        if kind == 'control':
            reg = yield from self.authorize(f, 'control')
            if reg is None:
                self.reply(conn, 'FR_REP', dict(ref=f.body['ref'], g=g, ok=False, reason='refused'))
                return
            self.c.monitor.effect(self, f, reg, 'control')
        r = self.raft.get(g)

        def answer(r, conn=conn, ref=f.body['ref']):
            kv = r.kv
            self.reply(conn, 'FR_REP' if kind == 'control' else 'MGMT_REP',
                       dict(ref=ref, g=g, ok=True, pair=kv.pair, index=kv.pair_index, by=self.id))
        if r is None or not r.start_read(answer):
            self.reply(conn, 'FR_REP' if kind == 'control' else 'MGMT_REP',
                       dict(ref=f.body['ref'], g=g, ok=False, reason='not leader',
                            hint=r.leader if r else None))

    def formation_driver(self):
        while True:
            yield Wait('idle', lambda: self.c.now >= self.fd_next and self.formation_work())
            yield Step('formation')
            self.fd_next = self.c.now + 1
            for g in GROUPS:
                old = self.fr.get(g)
                if old is not None and self.c.now - old.body['sent'] > 3:
                    old.owner, old.terminal = None, 'expired'   # control read: retried, never charged
                    del self.fr[g]
                if g not in self.hosted and self.known[g] is None and g not in self.fr:
                    voters = [v for v in sorted(group_voters(g, self.cfg.n)) if v in self.bindings]
                    if not voters:
                        continue
                    v = voters[(self.c.now + self.id) % len(voters)]
                    op = self.new_op('FR', v, self.bindings[v].tup(), {'FR_REP'}, 0, None,
                                     dict(g=g, sent=self.c.now), owner='control')
                    self.fr[g] = op
                    self.send_peer(v, 'FR_REQ', dict(g=g, ref=op.id), op)
            r = self.raft.get(2)
            mk = self.known[0]
            if r and r.role == 'leader' and mk is not None and r.kv.pair is None and not self.c.pause_salt \
                    and r.noop_index and r.commit >= r.noop_index \
                    and not any(e[1][0] == 'SALT' for e in r.log[r.applied - r.base:]):
                r.propose(('SALT', mk))
                self.after_raft(2)

    def formation_work(self):
        for g in GROUPS:
            if g not in self.hosted and self.known[g] is None:
                op = self.fr.get(g)
                if op is None or self.c.now - op.body['sent'] > 3:
                    return True
        r = self.raft.get(2)
        return bool(r and r.role == 'leader' and self.known[0] is not None and r.kv.pair is None)

    def resolve_reply(self, op, f):
        b = f.body
        if op.kind == 'FR':
            self.fr.pop(op.body['g'], None)
            op.owner, op.terminal = None, 'resolved'
            if not b.get('ok') or b.get('pair') is None:
                self.fd_next = self.c.now + 1
                return
            g = op.body['g']
            if g in self.hosted or f.tag.member not in group_voters(g, self.cfg.n):
                self.refuse('formation read', f.src, 'answer for a hosted group or from a non-voter')
                return
            if conflict(self.pair, b['pair']):
                self.stop('formation read pair %s conflicts with local pair %s' % (b['pair'], self.pair))
                return
            if self.pair is None:
                if not self.identity_event(b['pair'], 'FORMATION_READ group %d' % g):
                    return
            self.known[g] = b['pair']
            self.update_complete()
            return
        if op.kind == 'FWD':
            op.owner, op.terminal = None, 'resolved'
            parent = self.ops.get(op.body['parent'])
            if parent is None or parent.owner is None or parent.body.get('fwd_ref') != op.id:
                return
            if parent.owner == 'obligation':   # a late reply resolves the handed-off record
                if b.get('result') in ('ok', 'duplicate', 'already published') \
                        and 'receipt' not in self.resolution_paused:
                    self.finish(parent, 'resolved', b['result'])
                return
            op = parent
        if op.kind in ('APP', 'PUBLISH'):
            res = b.get('result')
            if res in ('ok', 'duplicate', 'already published'):
                self.publish(op, dict(result=res))
                self.resolve_opid(op.opid, res)
            elif res == 'not_leader' and b.get('hint') and op.hops < 3:
                op.hops += 1
                self.forward(op, b['hint'])
            elif res in ('uncertain', 'timeout'):
                self.make_uncertain(op, 'receipt')
            else:
                self.finish(op, 'aborted', res or 'retry')

    # -- operations (C8) -------------------------------------------------------
    def peer_app(self, conn, f):
        reg = yield from self.authorize(f, 'app', conn)
        if reg is None:
            why = self.refused[-1][2] if self.refused else 'refused'
            self.reply(conn, 'FWD_REP', dict(ref=f.body['ref'], result='busy' if why == 'BUSY' else 'refused'))
            return
        self.c.monitor.effect(self, f, reg, 'app')
        if reg.op.owner == 'token':     # the waiter may have left while the step was paused
            self.start_app(reg.op)

    def client_request(self, conn, f):
        reg = yield from self.authorize(f, 'client', conn)
        if reg is None:
            why = self.refused[-1][2] if self.refused else 'refused'
            self.reply(conn, 'CLI_REP', dict(ref=f.body.get('ref'), result='busy' if why == 'BUSY' else why))
            return
        self.c.monitor.effect(self, f, reg, 'client')
        op = reg.op
        if op.owner != 'token':         # the waiter may have left while the step was paused
            return
        if op.kind == 'PUT':
            yield from self.put(op)
        else:
            self.start_app(op)

    def start_app(self, op):
        key = op.body['key'] if op.kind == 'APP' else op.body['fid']
        g = shard_group(key)
        op.g = g
        r = self.raft.get(g)
        if r is not None and r.role == 'leader':
            cmd = ('APP', op.opid, key, op.body['val']) if op.kind == 'APP' else ('PUBLISH', op.opid, op.body['fid'])
            op.idx, op.term = r.propose(cmd)
            op.state = 'proposed'
            self.after_raft(g)
            return
        if op.remote in self.cfg.ids:   # admitted forward: do not forward again
            self.publish(op, dict(result='not_leader', hint=r.leader if r else None))
            return
        hint = r.leader if r is not None and r.leader else sorted(group_voters(g, self.cfg.n))[0]
        if hint == self.id:
            self.publish(op, dict(result='retry'))
            return
        self.forward(op, hint)

    def forward(self, op, to):
        b = self.bindings.get(to)
        if to not in self.others or b is None:
            self.finish(op, 'aborted', 'retry')   # nothing sent: proven pre-acceptance
            return
        op.state = 'forwarded'
        prev = self.ops.get(op.body.get('fwd_ref'))
        if prev is not None and prev.owner is not None:
            prev.owner, prev.terminal = None, 'superseded'
        fop = self.new_op('FWD', to, b.tup(), {'FWD_REP'}, 0, None, dict(parent=op.id), owner='control')
        op.body['fwd_ref'], op.body['fwd_at'] = fop.id, self.c.now
        args = dict(opid=op.opid, key=op.body.get('key'), val=op.body.get('val'), fid=op.body.get('fid'),
                    op=op.kind)
        self.send_peer(to, 'FWD_REQ', dict(ref=fop.id, args=args, op='APP'), fop)

    def after_raft(self, g):
        r = self.raft.get(g)
        if r is None:
            return
        for op in list(self.ops.values()):
            if op.g != g or op.state != 'proposed' or op.owner is None:
                continue
            t = r.term_at(op.idx)
            if t is not None and t == op.term and r.commit >= op.idx:
                op.accepted = True
            rec = r.kv.receipts.get(op.opid)
            if rec is not None:
                if op.owner == 'token':
                    self.publish(op, dict(result=rec[0]))
                elif op.owner == 'obligation' and 'raft_entry' not in self.resolution_paused:
                    self.finish(op, 'resolved', rec[0])
                continue
            if (t is not None and t != op.term or op.idx <= r.base and rec is None
                    or op.idx > r.last_index()) and op.owner == 'token':
                op.state = 'uncertain'
                self.make_uncertain(op, 'raft_entry')

    def publish(self, op, body):
        """Publication: the reply that makes the result visible (C8)."""
        self.send_result(op, body)
        self.finish(op, 'published' if body.get('result') in ('ok', 'duplicate', 'already published')
                    else 'aborted', body.get('result'))

    def send_result(self, op, body):
        if op.conn is None or op.replied:
            return
        kind = 'FWD_REP' if op.remote in self.cfg.ids else 'CLI_REP'
        self.reply(op.conn, kind, dict(ref=op.body.get('ref'), **body))
        op.replied = True

    def finish(self, op, how, result=None):
        if op.owner is None:
            return
        op.owner, op.terminal, op.result = None, how, result
        self.c.monitor.op_finished(self, op)
        if op.kind in ('APP', 'PUBLISH') and op.body.get('fwd_ref') in self.ops:
            f = self.ops[op.body['fwd_ref']]
            if f.owner is not None:
                f.owner, f.terminal = None, 'resolved'
        if how in ('aborted', 'transferred'):
            self.send_result(op, dict(result=result))
        if self.rules.drain == 'old_tuple':
            for m in list(self.pending):
                if self.inflight(m) == 0:
                    b = self.pending.pop(m)
                    self.bindings[m] = b
                    self.fence[m] = b.inc
                    self.inbox.append(Item('FENCE', member=m, inc=b.inc))

    def resolve_opid(self, opid, res):
        for op in self.ops.values():
            if op.owner == 'obligation' and op.opid == opid and op.resolver == 'receipt' \
                    and 'receipt' not in self.resolution_paused:
                self.finish(op, 'resolved', res)

    def make_uncertain(self, op, resolver):
        """Atomic handoff with reservation transfer (C8); refused -> stays a token."""
        if op.kind in self.refuse_handoff or resolver in self.refuse_handoff:
            self.c.monitor.note('handoff refused', self.id, op.id)
            return False
        if op.owner != 'token':
            return op.owner == 'obligation'
        op.owner, op.resolver = 'obligation', resolver
        self.c.monitor.note('handoff', self.id, op.id, resolver)
        self.send_result(op, dict(result='uncertain'))
        return True

    def timeout(self, op):
        """The waiter leaves (C8): proven abort, accepted transfer, or uncertain handoff."""
        if op.owner != 'token':
            return 'not a token'
        if op.state in ('admitted', 'put_staged'):
            self.finish(op, 'aborted', 'timeout')
            return 'aborted'
        if op.state == 'proposed' and op.accepted or op.state == 'put_durable':
            if op.state == 'put_durable':
                self.store.frags[op.body['fid']] = 'orphan'
            self.finish(op, 'transferred', 'timeout')
            return 'transferred'
        resolver = {'proposed': 'raft_entry', 'uncertain': 'raft_entry', 'forwarded': 'receipt',
                    'put_io': 'fragment'}.get(op.state)
        if resolver and self.make_uncertain(op, resolver):
            return 'handed off'
        return 'kept'

    def resolver(self):
        def work():
            for op in self.ops.values():
                if op.owner == 'obligation' and op.resolver not in self.resolution_paused:
                    if op.resolver == 'fragment' and op.state == 'put_done':
                        return op
                    if op.resolver == 'receipt' and op.req_conn is not None and op.req_conn.dead \
                            and op.body.get('retried_ok'):
                        return op
                    if op.resolver == 'raft_entry' and op.g in self.raft and op.opid in self.raft[op.g].kv.receipts:
                        return op
            for op in self.ops.values():
                if op.owner == 'token' and op.state == 'forwarded':
                    fop = self.ops.get(op.body.get('fwd_ref'))
                    if fop is not None and fop.req_conn is not None and fop.req_conn.dead:
                        return op
                    if fop is not None and fop.req_conn is None and self.c.now - op.body['fwd_at'] > 2:
                        return op
            return None
        while True:
            yield Wait('idle', lambda: work() is not None)
            op = work()
            yield Step('resolve')
            if op.owner == 'token':
                fop = self.ops.get(op.body.get('fwd_ref'))
                if fop is not None and fop.req_conn is None:
                    q = self.outq[fop.remote]       # never sent: proven pre-acceptance, withdraw
                    for i, (k, b, o) in enumerate(q):
                        if o is fop:
                            del q[i]
                            break
                    fop.owner, fop.terminal = None, 'withdrawn'
                    self.finish(op, 'aborted', 'retry')
                    continue
                yield Lock('handoff', self)
                self.make_uncertain(op, 'receipt')
                self.unlock()
                continue
            res = self.raft[op.g].kv.receipts[op.opid][0] if op.resolver == 'raft_entry' else op.result
            self.finish(op, 'resolved', res)

    def put(self, op):
        fid = op.body['fid']
        st = self.store.frags.get(fid)
        busy = any(o is not op and o.owner and o.kind == 'PUT' and o.body.get('fid') == fid
                   for o in self.ops.values())
        if busy:
            self.publish(op, dict(result='busy'))
            return
        if st in ('durable', 'orphan'):
            self.store.frags[fid] = 'durable'
            self.last_write = ('ack', fid)
            self.publish(op, dict(result='ok'))
            return
        op.state = 'put_staged'
        yield Step('io_begin')
        if op.owner != 'token':
            return
        op.state = 'put_io'
        self.c.monitor.io(self, op, 'begin')
        yield Wait('io_end', lambda: self.c.io_ready(self, op))
        outcome = self.c.io_outcome(self, op)
        self.c.monitor.io(self, op, 'end')
        if outcome == 'fail':
            op.state = 'put_done'
            if op.owner == 'token':
                self.last_write = ('error', fid)
                self.publish(op, dict(result='io error'))
            else:
                op.result = 'none'
            return
        durable = outcome in ('ok', 'uncertain_durable')
        if durable:
            self.store.frags[fid] = 'durable' if op.owner == 'token' and outcome == 'ok' else 'orphan'
        op.state = 'put_done'
        if op.owner == 'token' and outcome == 'ok':
            op.state = 'put_durable'
            yield Step('put_reply')
            if op.owner == 'token':
                self.last_write = ('ack', fid)
                self.publish(op, dict(result='ok'))
            return
        if op.owner == 'token':
            self.make_uncertain(op, 'fragment')
        op.state = 'put_done'
        op.result = 'orphan' if durable else 'none'

    # -- management (C4 mgmt row) ---------------------------------------------
    def mgmt_request(self, conn, f):
        cmd = f.body['cmd']
        yield Step('mgmt')
        if cmd not in MGMT_ALLOW:
            self.refuse('mgmt allowlist', f.src, cmd)
            self.reply(conn, 'MGMT_REP', dict(ref=f.body['ref'], ok=False, reason='%s not permitted' % cmd))
            return
        if cmd == 'STATUS':
            self.reply(conn, 'MGMT_REP', dict(ref=f.body['ref'], ok=True, status=self.status()))
        elif cmd == 'FORMATION_READ':
            yield from self.serve_formation_read(conn, f, 'mgmt')
        elif cmd == 'RAFT_STEP_DOWN':
            g = f.body.get('g', 0)
            r = self.raft.get(g)
            if r and r.role == 'leader':
                r.step_down()
                r.silence = -10
            self.reply(conn, 'MGMT_REP', dict(ref=f.body['ref'], ok=True))
        else:
            self.mkfs(conn, f)

    def present(self, m):
        return m in self.bindings and any(cn.state == 'ready' and not cn.dead for cn in self.pool[m])

    def mkfs(self, conn, f):
        ref = f.body['ref']
        if f.body['digest'] != self.digest:
            self.reply(conn, 'MGMT_REP', dict(ref=ref, ok=False, reason='digest %s differs from local %s' % (
                f.body['digest'][:12], self.digest[:12])))
            return
        r = self.raft.get(0)
        if r is not None and r.kv.pair is not None:
            self.reply(conn, 'MGMT_REP', dict(ref=ref, ok=True, pair=r.kv.pair))
            return
        if r is None or r.role != 'leader':
            self.reply(conn, 'MGMT_REP', dict(ref=ref, ok=False, reason='not leader',
                                              hint=r.leader if r else sorted(group_voters(0, self.cfg.n))[0]))
            return
        absent = [m for m in self.others if not self.present(m)]
        if absent:
            self.reply(conn, 'MGMT_REP', dict(ref=ref, ok=False, reason='members absent', absent=absent))
            return
        pending = [e for e in r.log[r.applied - r.base:] if e[1][0] == 'MKFS']
        if not pending:
            r.propose(('MKFS', (self.salt, self.digest)))
        op = self.new_op('MKFS', conn.declarer, ('mgmt',), set(), 0, conn, dict(ref=ref), owner='control')
        op.g, op.state = 0, 'waiting'
        self.after_raft(0)
        self.c.mkfs_waiters.append((self, op))

    def status(self):
        q = {}
        for g in GROUPS:
            r = self.raft.get(g)
            q[g] = None if r is None else dict(role=r.role, leader=r.leader, term=r.term)
        return dict(member=self.id, state=self.state, complete=self.complete, pair=self.pair,
                    known=dict(self.known), absent=[m for m in self.others if not self.present(m)],
                    metadata_quorum=q,
                    observed_health={m: self.present(m) for m in self.others},
                    last_write=self.last_write,
                    voters={g: sorted(group_voters(g, self.cfg.n)) for g in GROUPS})


# ---------------------------------------------------------------------------
# External actors


class Mgmt:
    def __init__(self, c, name, target, digest, cmd='RAFT_MKFS', args=None):
        self.c, self.id, self.target, self.digest = c, name, target, digest
        self.cmd, self.args = cmd, dict(args or {})
        self.alive, self.done, self.result, self.log = True, False, None, []

    def run(self):
        while not self.done:
            node = self.c.nodes.get(self.target)
            cn = self.c.connect(self, self.c.cfg.endpoint(self.target), 'mgmt') if node else None
            if cn is None:
                t = self.c.now + 2
                yield Wait('retry', lambda: self.c.now >= t)
                continue
            cn.fwd.append(Frame('DECL', None, dict(role='mgmt', digest=self.digest), self.id))
            yield Wait('decl', lambda: cn.rev or cn.dead)
            if cn.dead:
                continue
            rep = cn.rev.popleft().body
            if not rep['ok']:
                self.log.append(('refused', rep['reason']))
                t = self.c.now + 2
                yield Wait('retry', lambda: self.c.now >= t)
                continue
            while not self.done and not cn.dead:
                cn.fwd.append(Frame('MGMT_REQ', None, dict(cmd=self.cmd, ref=0, digest=self.digest,
                                                           **self.args), self.id))
                yield Wait('reply', lambda: cn.rev or cn.dead)
                if cn.dead:
                    break
                rep = cn.rev.popleft().body
                self.log.append(rep)
                if rep['ok']:
                    self.done, self.result = True, rep
                    break
                if rep.get('reason') == 'not leader' and rep.get('hint'):
                    self.target = rep['hint']
                    t = self.c.now + 1          # back off: never a zero-delay retry loop
                    yield Wait('retry', lambda: self.c.now >= t)
                    break
                if 'not permitted' in rep.get('reason', '') or 'digest' in rep.get('reason', ''):
                    self.done, self.result = True, rep
                    break
                t = self.c.now + 2
                yield Wait('retry', lambda: self.c.now >= t)
            self.c.close_actor_conns(self)


class Client:
    def __init__(self, c, name, target, ops):
        self.c, self.id, self.target = c, name, target
        self.ops = list(ops)      # [('APP', opid, key, val) | ('PUT', opid, fid) | ('PUBLISH', opid, fid)]
        self.results = {}
        self.log = []
        self.alive = True
        self.done = False

    def run(self):
        i = 0
        cn = None
        while i < len(self.ops):
            if cn is None or cn.dead or cn.state == 'refused':
                node = self.c.nodes.get(self.target)
                cn = self.c.connect(self, self.c.cfg.endpoint(self.target), 'client') if node else None
                if cn is None:
                    self.target = self.c.next_target(self.target)
                    t = self.c.now + 2
                    yield Wait('retry', lambda: self.c.now >= t)
                    continue
                cn.fwd.append(Frame('DECL', None, dict(role='client', digest=None), self.id))
                yield Wait('decl', lambda: cn.rev or cn.dead)
                if cn.dead:
                    continue
                rep = cn.rev.popleft().body
                if not rep['ok']:
                    self.log.append(('refused', rep['reason']))
                    cn.state = 'refused'
                    t = self.c.now + 2
                    yield Wait('retry', lambda: self.c.now >= t)
                    continue
                cn.state = 'ready'
            op = self.ops[i]
            kind, opid = op[0], op[1]
            args = dict(opid=opid)
            if kind == 'APP':
                args.update(key=op[2], val=op[3])
            else:
                args.update(fid=op[2])
            cn.fwd.append(Frame('CLI_REQ', None, dict(op=kind, ref=i, args=args, **args), self.id))
            yield Wait('reply', lambda: cn.rev or cn.dead)
            if cn.dead:
                continue
            rep = cn.rev.popleft().body
            while rep.get('ref') != i and cn.rev:
                rep = cn.rev.popleft().body
            if rep.get('ref') != i:
                continue
            self.log.append((i, rep['result']))
            if rep['result'] in ('ok', 'duplicate', 'already published'):
                self.c.monitor.client_ok(self, op, rep['result'])
                self.results[opid] = rep['result']
                i += 1
                continue
            t = self.c.now + 1
            yield Wait('retry', lambda: self.c.now >= t)
        self.done = True
        self.c.close_actor_conns(self)


# ---------------------------------------------------------------------------
# Monitor: invariants after every step


class Monitor:
    def __init__(self, c):
        self.c = c
        self.events = []
        self.hwm = {}            # (node id, member) -> Binding (highest accepted)
        self.leaders = {}
        self.committed = {g: {} for g in GROUPS}
        self.persisted_pair = {}
        self.formed = set()
        self.ops = []
        self.io_open = {}
        self.acquired = []
        self.enabled = True

    def note(self, *ev):
        self.events.append((self.c.seq,) + ev)

    def violate(self, name, detail):
        raise Violation(name, detail)

    # declarations and authority
    def decl(self, node, member, b):
        old = self.hwm.get((node.id, member))
        if old is not None and (b.inc, b.counter) < (old.inc, old.counter):
            self.violate('lower_generation_newest', (node.id, member, old, b))
        if old is not None and old.pair is not None and b.pair != old.pair:
            self.violate('binding_downgrade', (node.id, member, old, b))
        self.hwm[(node.id, member)] = b
        self.note('publish', node.id, member, b.tup())

    def global_complete(self):
        p0 = next((e[1][1] for i, e in sorted(self.committed[0].items()) if e[1][0] == 'MKFS'), None)
        p2 = next((e[1][1] for i, e in sorted(self.committed[2].items()) if e[1][0] == 'SALT'), None)
        return p0 is not None and p0 == p2

    def acquire(self, node, reg, f):
        """Oracle at the moment an item acquires authority (C7 invariant)."""
        tag = f.tag if f is not None else None
        self.acquired.append((self.c.seq, node.id, reg.kind, tag))
        if reg.kind == 'client':
            if not node.complete or not self.global_complete():
                self.violate('premature_application', (node.id, 'client admission before complete'))
            return
        if tag is None:
            return
        h = self.hwm.get((node.id, tag.member))
        if h is None:
            self.violate('obsolete_authority', (node.id, 'no accepted declaration', tag))
        if conflict(node.pair, tag.pair) or conflict(h.pair, tag.pair):
            self.violate('conflicting_identity', (node.id, tag, h, node.pair))
        if h.pair is not None and tag.pair is None:
            self.violate('obsolete_authority', (node.id, 'downgrade to none', tag.tup(), h.tup()))
        if tag.inc != h.inc:
            self.violate('obsolete_authority', (node.id, 'incarnation', tag.tup(), h.tup()))
        if reg.kind == 'app':
            if tag.tup() != h.tup():
                self.violate('obsolete_authority', (node.id, 'application tuple', tag.tup(), h.tup()))
            if not node.complete or not self.global_complete():
                self.violate('premature_application', (node.id, tag))

    def effect(self, node, f, reg, kind):
        if reg is None or not reg.registered:
            if reg is None:
                reg = Reg(kind, f.tag if f else None, (), self.c.seq, registered=False)
            self.acquire(node, reg, f)

    # raft
    def leader(self, node, g, term):
        k = (g, term)
        if k in self.leaders and self.leaders[k] != node.id:
            self.violate('two_leaders', (g, term, self.leaders[k], node.id))
        self.leaders[k] = node.id

    def commit_advanced(self, r, old, new):
        for i in range(max(old, r.base) + 1, new + 1):
            e = r.entry(i)
            have = self.committed[r.g].get(i)
            if have is None:
                self.committed[r.g][i] = e
            elif have != e:
                self.violate('committed_divergence', (r.g, i, have, e, r.me))

    def applied(self, node, g, i, cmd, res):
        have = self.committed[g].get(i)
        if have is not None and have[1] != cmd:
            self.violate('applied_divergence', (node.id, g, i, have, cmd))
        kv = node.raft[g].kv
        seen = [o for _, o in kv.effects]
        if len(seen) != len(set(seen)):
            self.violate('duplicate_publication', (node.id, g, seen))
        fids = list(kv.published)
        if len(fids) != len(set(fids)):
            self.violate('duplicate_publication', (node.id, g, fids))
        if cmd[0] == 'PUBLISH' and res == 'ok':
            fid = cmd[2]
            if not any(s.frags.get(fid) == 'durable' for s in self.c.stores.values()):
                self.violate('published_without_body', (fid,))

    # operations
    def op_created(self, node, op):
        self.ops.append(op)

    def op_finished(self, node, op):
        if hash(op.tup) != op._tup_hash:
            self.violate('token_overwritten', op)

    def io(self, node, op, what):
        if what == 'begin':
            self.io_open[op.id] = op
        else:
            self.io_open.pop(op.id, None)

    def delivered(self, node, conn, f):
        if f.intended is not None and conn.role == 'peer' and f.kind != 'DECL' and f.intended != node.id:
            self.violate('misdirected', (f, f.intended, node.id))

    def client_ok(self, cl, op, res):
        opid = op[1]
        if op[0] == 'PUT':
            if not any(s.frags.get(op[2]) == 'durable' for s in self.c.stores.values()):
                self.violate('false_success', (cl.id, op, 'no durable body'))
            return
        g = shard_group(op[2])
        if not any(e[1][0] in ('APP', 'PUBLISH') and e[1][1] == opid for e in self.committed[g].values()):
            # entries may have been compacted on every node; fall back to receipts
            if not any(opid in s.kv.get(g, KV()).receipts for s in self.c.stores.values()):
                self.violate('false_success', (cl.id, op))

    # whole-state invariants after each step
    def check(self):
        c = self.c
        for mid, s in c.stores.items():
            if s.formation_pair is not None:
                p = self.persisted_pair.setdefault(id(s), s.formation_pair)
                if p != s.formation_pair:
                    self.violate('persisted_pair_overwritten', (mid, p, s.formation_pair))
            if s.formation_state == 'formed':
                self.formed.add(id(s))
            elif id(s) in self.formed:
                self.violate('formed_store_regressed', (mid, s.formation_state))
        for node in c.nodes.values():
            if node is None or not node.alive:
                continue
            if node.units() > c.total_bound:
                self.violate('bound_exceeded', (node.id, node.units(), c.total_bound))
            for who in {op.remote for op in node.ops.values() if op.owner in ('token', 'obligation')}:
                if node.units(who) > c.member_bound:
                    self.violate('member_bound_exceeded', (node.id, who, node.units(who)))
            for (nid, m), h in self.hwm.items():
                b = node.bindings.get(m) if nid == node.id else None
                if b is not None and c.rules.binding == 'member' and c.rules.drain == 'hwm' and b != h:
                    self.violate('binding_not_hwm', (node.id, m, b, h))
            for g, r in node.raft.items():
                if r.voters != group_voters(g, node.cfg.n):
                    self.violate('voter_mask_changed', (node.id, g))
        for op in list(self.io_open.values()):
            if op.owner not in ('token', 'obligation') and op.node.alive:
                self.violate('unowned_io', op)


# ---------------------------------------------------------------------------
# The cluster and the scheduler


class Cluster:
    def __init__(self, n=3, rules=CONSOLIDATED, seed=0, pool_size=2, batch=2, total_bound=8,
                 member_bound=4, conf_text=None, node_conf=None):
        self.rules = rules
        self.cfg = parse_config(conf_text if conf_text is not None else default_conf(n))
        self.node_cfg = {m: parse_config(t) for m, t in (node_conf or {}).items()}
        self.pause_salt = False
        self.digest = self.cfg.digest()
        self.rng = random.Random(seed)
        self.pool_size, self.batch = pool_size, batch
        self.total_bound, self.member_bound = total_bound, member_bound
        self.now = self.seq = self.rr = 0
        self.stores = {m: Store(m) for m in self.cfg.ids}
        self.nodes = {}
        self.conns = {}
        self.threads = []
        self.holds = []
        self.route = {ep: m for m, ep in self.cfg.members}
        self.monitor = Monitor(self)
        self.next_pid = 100
        self.next_conn = 0
        self.actors = []
        self.mkfs_waiters = []
        self.io_policy = {}
        self.io_hold = set()
        self.random_io = False
        self.threads.append(Thread(self, self, 'clock', self.clock(), 2))
        self.threads.append(Thread(self, self, 'mkfs_replies', self.mkfs_replier(), 0))

    alive = True

    def clock(self):
        while True:
            yield Step('tick')
            self.now += 1

    def mkfs_replier(self):
        def ready():
            return [(n, op) for n, op in self.mkfs_waiters
                    if not n.alive or op.owner is None or n.raft[0].kv.pair is not None
                    or n.raft[0].role != 'leader']
        while True:
            yield Wait('idle', lambda: ready())
            for n, op in ready():
                self.mkfs_waiters.remove((n, op))
                if not n.alive or op.owner is None:
                    continue
                r = n.raft[0]
                if r.kv.pair is not None:
                    n.reply(op.conn, 'MGMT_REP', dict(ref=op.body['ref'], ok=True, pair=r.kv.pair))
                else:
                    n.reply(op.conn, 'MGMT_REP', dict(ref=op.body['ref'], ok=False, reason='retry'))
                op.owner, op.terminal = None, 'resolved'

    # processes
    def start(self, mid, fresh=0):
        pid = self.next_pid
        self.next_pid += 1
        inc = start_process(self.stores[mid], pid, fresh)
        node = Node(self, mid, inc, pid)
        self.nodes[mid] = node
        node.boot()
        if node.alive:
            self.threads.extend(node.threads)
        self.monitor.note('start', mid, inc)
        return node

    def start_all(self):
        for m in self.cfg.ids:
            self.start(m)
        return self

    def kill(self, mid):
        node = self.nodes.get(mid)
        if node is None or not node.alive:
            return
        node.alive = False
        self.stores[mid].owner = None
        for cn in self.conns.values():
            if cn.apid == node.pid:
                cn.dead = True
                cn.fwd.clear()
            if cn.dpid == node.pid:
                cn.dead = True
                cn.rev.clear()
        self.threads = [t for t in self.threads if t.alive]
        self.monitor.note('kill', mid)

    def crash(self, mid):
        self.kill(mid)

    def restart(self, mid, fresh=0):
        self.kill(mid)
        return self.start(mid, fresh)

    def connect(self, who, endpoint, role, intended=None):
        m = self.route.get(endpoint)
        node = self.nodes.get(m)
        if node is None or not node.alive:
            return None
        self.next_conn += 1
        dpid = getattr(who, 'pid', who.id)
        cn = Conn(self.next_conn, who.id, node.id, role, intended, dpid, node.pid)
        self.conns[cn.id] = cn
        t = Thread(self, node, 'admit%d' % cn.id, node.handler_admit(cn), 0,
                   peek=lambda: cn.fwd[0] if cn.fwd else None)
        self.threads.append(t)
        node.threads.append(t)
        if isinstance(who, Node):
            t2 = Thread(self, who, 'reply%d' % cn.id, who.handler_reply(cn), 0,
                        peek=lambda: cn.rev[0] if cn.rev else None)
            self.threads.append(t2)
            who.threads.append(t2)
        return cn

    def close(self, cn):
        cn.dead = True
        cn.fwd.clear()
        cn.rev.clear()

    def close_actor_conns(self, actor):
        for cn in self.conns.values():
            if cn.declarer == actor.id:
                cn.dead = True

    def next_target(self, t):
        ids = self.cfg.ids
        return ids[(ids.index(t) + 1) % len(ids)]

    def add_mgmt(self, target=1, cmd='RAFT_MKFS', digest=None, args=None, name=None):
        a = Mgmt(self, name or 'mgmt%d' % len(self.actors), target, digest or self.digest, cmd, args)
        self.actors.append(a)
        self.threads.append(Thread(self, a, 'actor', a.run(), 1))
        return a

    def add_client(self, target, ops, name=None):
        a = Client(self, name or 'client%d' % len(self.actors), target, ops)
        self.actors.append(a)
        self.threads.append(Thread(self, a, 'actor', a.run(), 1))
        return a

    # storage I/O policy
    def io_ready(self, node, op):
        return op.body.get('fid') not in self.io_hold

    def io_outcome(self, node, op):
        if self.random_io:
            return self.rng.choice(('ok', 'ok', 'ok', 'fail', 'uncertain_durable', 'uncertain_none'))
        return self.io_policy.get(op.body.get('fid'), 'ok')

    # scheduling
    def hold(self, pred):
        self.holds.append(pred)
        return pred

    def release(self, h=None):
        if h is None:
            self.holds.clear()
        elif h in self.holds:
            self.holds.remove(h)

    def held(self, t):
        return any(h(t) for h in self.holds)

    def enabled(self, holds=True):
        return [t for t in self.threads if t.ready() and not (holds and self.held(t))]

    def step(self, t):
        if not t.ready():
            raise RuntimeError('thread %r is not enabled' % t)
        self.seq += 1
        if t.cond.kind == 'lock':
            t.cond.node.lock = t
        try:
            t.cond = t.gen.send(None)
        except StopIteration:
            t.done = True
        self.monitor.check()
        return t

    def find(self, pred, holds=False):
        for t in self.threads:
            if t.alive and pred(t):
                return t
        return None

    def step_where(self, pred):
        t = self.find(lambda t: pred(t) and t.ready())
        if t is None:
            raise RuntimeError('no enabled thread matches')
        return self.step(t)

    def run(self, until=None, max_steps=20000, prio_max=2):
        for _ in range(max_steps):
            if until is not None and until():
                return True
            self.threads = [t for t in self.threads if t.alive]
            ts = [t for t in self.enabled() if t.prio <= prio_max]
            if not ts:
                return until() if until is not None else True
            p = min(t.prio for t in ts)
            ts = [t for t in ts if t.prio == p]
            self.rr += 1
            self.step(ts[self.rr % len(ts)])
        return until() if until is not None else False

    def quiesce(self, max_steps=20000):
        return self.run(until=lambda: not [t for t in self.enabled() if t.prio < 2], max_steps=max_steps)

    def ticks(self, k, max_steps=20000):
        end = self.now + k
        return self.run(until=lambda: self.now >= end, max_steps=max_steps)

    # convenience predicates
    def live(self):
        return [n for n in self.nodes.values() if n.alive]

    def all_formed(self):
        return all(n.complete and n.state == 'formed' for n in self.live()) and \
            len(self.live()) == self.cfg.n

    def leader_of(self, g):
        for n in self.live():
            r = n.raft.get(g)
            if r and r.role == 'leader':
                return n.id
        return None

    def form(self, max_steps=60000, target=1):
        self.add_mgmt(target)
        ok = self.run(until=self.all_formed, max_steps=max_steps)
        if not ok:
            raise AssertionError('formation did not complete: %s' % [n.status() for n in self.live()])
        return self

    def converged(self):
        for g in GROUPS:
            rs = [n.raft[g] for n in self.live() if g in n.raft]
            lead = [r for r in rs if r.role == 'leader']
            if len(lead) != 1:
                return False
            c = lead[0].commit
            if any(r.applied != c or r.commit != c for r in rs):
                return False
        return True

    def settled(self):
        if not all(a.done for a in self.actors):
            return False
        for n in self.live():
            if any(op.owner in ('token', 'obligation') for op in n.ops.values()):
                return False
        return self.converged()

    def inject(self, src_tag, dst, kind, body, conn=None):
        """A frame from member src_tag.member arriving at dst on conn (C4 tests)."""
        node = self.nodes[dst]
        if conn is None:
            conn = next(cn for cn in self.conns.values() if cn.admitter == dst and cn.declarer == src_tag.member
                        and cn.admitted and not cn.dead)
        f = Frame(kind, src_tag, body, src_tag.member, intended=dst)
        conn.fwd.append(f)
        return f, conn


# ---------------------------------------------------------------------------
# Randomized interleaving search (C14)


def random_search(seed, n=3, steps=1500, rules=CONSOLIDATED, crashes=2, settle_steps=120000):
    """Random schedule with faults around formation and again under client load, then a fair
    fault-free settle.

    Safety invariants are checked after every step (Monitor); at the end the settle must form
    the cluster, complete every client operation, resolve every token and obligation and
    converge both groups (formation and completion liveness). Returns counters; raises on any
    failure.
    """
    c = Cluster(n=n, rules=rules, seed=seed, total_bound=4, member_bound=3)
    rng = random.Random(seed * 7919 + n)
    c.random_io = True
    c.start_all()
    c.add_mgmt(rng.choice(c.cfg.ids))
    clients = []
    for k in range(2):
        ops = []
        for j in range(3):
            key = rng.randrange(1, 40)
            ops.append(('APP', 's%d-c%d-o%d' % (seed, k, j), key, 'v%d' % j))
        fid = 1000 + 2 * k + 1
        ops += [('PUT', 's%d-c%d-p' % (seed, k), fid), ('PUBLISH', 's%d-c%d-pub' % (seed, k), fid)]
        clients.append(c.add_client(rng.choice(c.cfg.ids), ops))
    state = dict(left=crashes, faults=0)

    def restart_dead():
        for m in c.cfg.ids:
            if not c.nodes[m].alive:
                c.start(m)

    def phase(k):
        for _ in range(k):
            x = rng.random()
            if x < 0.004 and state['left']:
                victim = rng.choice(c.cfg.ids)
                if c.nodes[victim].alive:
                    c.crash(victim)
                    state['left'] -= 1
                    state['faults'] += 1
                    continue
            if x < 0.012:
                dead = [m for m in c.cfg.ids if not c.nodes[m].alive]
                if dead:
                    c.start(rng.choice(dead))
                    state['faults'] += 1
                    continue
            if x < 0.05:
                toks = [op for nd in c.live() for op in nd.ops.values() if op.owner == 'token']
                if toks:
                    op = rng.choice(toks)
                    if op.node.lock is None:      # the waiter leaves (C8)
                        op.node.lock = 'timeout'
                        op.node.timeout(op)
                        if op.node.alive:
                            op.node.lock = None
                        state['faults'] += 1
                        continue
            if x < 0.055:
                nd = rng.choice(c.live())
                if nd.raft:
                    nd.raft[rng.choice(list(nd.raft))].compact()
                    state['faults'] += 1
                    continue
            if x < 0.06:
                cs = [cn for cn in c.conns.values() if not cn.dead and cn.role == 'peer']
                if cs:
                    c.close(rng.choice(cs))
                    state['faults'] += 1
                    continue
            c.step(rng.choice(c.enabled()))

    phase(steps // 2)                         # formation under faults
    restart_dead()
    c.run(until=c.all_formed, max_steps=settle_steps)
    phase(steps)                              # client load under faults
    c.random_io = False
    restart_dead()
    done = c.run(until=lambda: c.all_formed() and c.settled(), max_steps=settle_steps)
    if not done:
        raise AssertionError('seed %d n %d: no convergence: %s; clients %s; ops %s' % (
            seed, n, [nd.status() for nd in c.live()], [(cl.id, cl.log[-3:]) for cl in clients],
            [(nd.id, [op for op in nd.ops.values() if op.owner in ('token', 'obligation')]) for nd in c.live()]))
    ev = c.monitor.events
    return dict(seed=seed, n=n, seq=c.seq, faults=state['faults'],
                ops=sum(len(cl.results) for cl in clients),
                restarts=sum(1 for e in ev if e[1] == 'start') - n,
                handoffs=sum(1 for e in ev if e[1] == 'handoff'),
                stale=sum(1 for e in ev if e[1] == 'refuse' and 'stale' in str(e[5])),
                busy=sum(1 for e in ev if e[1] == 'refuse' and e[5] == 'BUSY'))
