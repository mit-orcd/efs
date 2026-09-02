# Client sessions, fencing, open-unlinked files, and POSIX locking

[Architecture](../../architecture.md) · [Transactions](transactions.md) ·
[Data protocol](data.md)

These four mechanisms share one foundation: a real client-session protocol.
It is specified here together with everything built on it.

## Client sessions and fencing

Several mechanisms — distributed locks, open-unlinked inode lifetime, append
reservation recovery, duplicate suppression — all depend on one magic
transition: "the client incarnation becomes stale." That transition is a
real protocol, because the naive version is one of the hardest problems in
distributed filesystems: **a network partition is indistinguishable from a
dead client**, and reclaiming a merely-partitioned client's locks or open
files on a timeout alone would change authoritative state on the word of a
failure detector.

**The protocol.** Every client operates under a **session**:

```text
session = { client_uuid, session_epoch, state }     (committed via Raft)
session authority for a client = shard hash(client_uuid) & 0xFFF
```

- Session records live on **sharded session authorities** (not one global
  session server — that would be a serializer and a single point of
  failure). A mount establishes its session with a Raft-committed epoch.
- Every mutating request carries `(client_uuid, session_epoch, op_id)`.
  **Shard leaders fence on the epoch**: a request naming a session epoch
  older than the committed one is rejected as stale (P3 — stale work is
  harmless, never accepted as authoritative). A leader answers from locally
  established session authority (below) — never from a cache it merely hopes
  is fresh, and never from a clock.
  **Data targets do not fence sessions.** A fenced client can still PUT an
  immutable candidate fragment — harmlessly: the target stores an orphan,
  and the metadata authority rejects its publication, so it never becomes
  visible (exactly P3, and exactly the "targets are dumb" model of
  [the data protocol](data.md)). Synchronously validating session authority
  on every data target would put an RPC/cache-coherence problem on the
  fastest path for zero correctness benefit.
- **The failure detector and the authority are separate roles:** heartbeat
  loss / connection death only decides *when* a session replacement may be
  **attempted**; **consensus decides *which* session is authoritative** —
  the epoch bump is a Raft commit on the session shard. A timeout alone
  never changes anything.
- Once the epoch bump has passed the **revocation barrier** below, the old
  session is dead everywhere: its **POSIX locks** are reclaimed by the lock
  authorities, its **open-unlinked leases** are dropped (the orphan inode
  becomes reclaimable, below), its pending **append reservations** are
  resolved as committed zero holes ([data protocol](data.md)), and its
  **dedup records are retained** so its in-flight retries are still
  recognized as duplicates rather than re-executed (I16).
- A fenced client learns on its next operation (`FENCED`) and must
  re-establish a session (new epoch, re-open state) before doing further
  work.

**The honest trade-off:** a partitioned-but-alive client can be fenced —
that is an availability sacrifice, never a safety violation, and it is the
standard fencing-token pattern (the session epoch *is* the fencing token,
checked at every authority). The alternative — never fencing on suspicion —
leaves dead clients' locks and reservations held forever.

### Revocation is a barrier, not an announcement

Committing `epoch+1` on the session shard does **not**, by itself, fence
anything. Metadata shards are independent Raft groups; a shard that has not
heard about `epoch+1` and is willing to act on its own cached view of epoch
`E` can still commit a mutation for the old client *after* the bump —
directly violating I23. "Leaders re-validate on doubt" is not a protocol,
because nothing tells a leader it should be in doubt.

So session state is a three-phase transition with a real barrier, and the
session record carries the set of shards that could possibly act on it:

```text
ACTIVE(E)  ->  FENCING(E+1)  ->  ACTIVE(E+1)

session record:
    { client_uuid, session_epoch, state, touched_shards }

touched_shards: a 4096-bit bitmap — bounded, and small (512 B)
```

**Registration (first use of a session on a shard).** Before shard S accepts
epoch E as authoritative, S must be recorded in that session's
`touched_shards`:

```text
S receives a mutation naming (client_uuid, E), and has no local record:
    S asks the session authority to register S for (client_uuid, E)
    session authority Raft-commits the bitmap bit
    -> only then may S cache "E is authoritative" and proceed
```

This is **once per (session, shard)**, not per request: a client that works
against 200 shards pays 200 registrations over the life of its mount. The
hot path is unaffected — an established shard answers from its own committed
state with no session lookup at all.

**Fencing.**

```text
1. session authority Raft-commits FENCING(E+1)
      -> no new shard may register for E from this point
2. touched_shards(E) is frozen by that same commit
3. FENCE(E+1) is sent to every shard in the frozen set
4. each shard, durably:
      rejects epoch E from now on
      resolves or aborts that session's undecided work
      ACKs the fence
5. only after every shard in the set has ACKed:
      session authority Raft-commits ACTIVE(E+1)
6. locks, open leases and append reservations of E may now be reclaimed,
   and the new session may begin authoritative work
```

Step 5 is the barrier: **nothing that depends on the old session being dead
may happen before it.** If a touched shard cannot be reached or cannot
establish authority, the new session **waits** — the correct
availability-for-safety trade, and not a new availability loss in practice,
since a shard that cannot establish authority cannot commit for the old
client either.

Note what this protocol deliberately avoids: it puts **no session lookup on
the I/O path**. Registration is once per shard per session; the barrier runs
only during recovery. Normal operations proceed on locally established
authority, which is exactly why the fence has to be explicit — nobody is
checking anything per request.

### Duplicate suppression must be bounded state

I16 requires that a replayed mutation cannot apply twice, and the fencing
rules above require dedup records to survive an epoch bump. At billions of
files and trillions of RPCs that must **not** mean one permanent row per
operation — that is an unbounded, permanently growing table on every shard.
The retained state is a window, not a history:

```text
per (client_uuid, session_epoch) on each shard that served it:
    highest_contiguous_seq      one integer: everything at or below is done
    completion window           small bitmap for out-of-order completions
                                above the watermark
    bounded reply cache         responses for the few in-flight ops that
                                could still be retried
```

A request at or below `highest_contiguous_seq` is a duplicate by definition
and needs no per-op record. Only the ragged edge — operations completed out
of order above the watermark — costs a bit each, and the window is bounded
by the client's allowed in-flight depth.

**Reclamation is driven by acknowledgement, not by elapsed time.** The
failure model allows a message to be delayed arbitrarily and duplicated
([the spec §2](../../architecture.md)), so no timer can *prove* that a
straggler will not arrive — a "retention window" would be an assumption the
network never agreed to. The client instead carries a **response-ack
watermark**: the **highest *contiguous*** sequence number whose reply it has
received, and the shard may release result state at or below it. Contiguity
is not pedantry — replies arrive out of order, so a watermark defined as
"the highest sequence whose reply arrived" would let reply 100 arriving
before 99 authorize discarding 99's result while 99's retry is still in
flight. A later duplicate at or below the watermark is then answered as
already-completed — cheaply, and without re-executing.

The reply cache matters more than it first appears, because for some
operations "it already happened" is not a sufficient answer. If an
`O_APPEND` reservation reply is lost, the retry of that same op-ID must
recover **the same offset** — a fresh reservation would allocate a second
range and leave a permanent hole. The retained reply, not merely a
completion bit, is what makes the retry idempotent.

For a **live** session, records are reclaimable once the client's ack
watermark has passed them. For a **fenced** one, requiring the watermark too
would leak forever: a dead client never sends another acknowledgement, so
its reply state could never be released. The revocation barrier is what
makes that unnecessary — once `epoch+1` is committed and every touched shard
has been fenced, a request carrying the **old epoch** is rejected on the
epoch alone, without consulting any per-request state. So the rule is:

```text
live session      release at or below the ack watermark
fenced session    once the revocation barrier has completed and the
                  outstanding ambiguity is resolved, drop the whole
                  epoch's dedup state — old-epoch requests are rejected
                  by epoch, not by lookup
```

Transaction decision records
([transactions.md](transactions.md)) have the matching condition:
reclaimable once every participant has acknowledged the decision and no
recovering participant can still ask for it.

## Open-unlinked inode lifetime

I6/I7 allow `nlink = 0` while a file is open. The lifecycle:

```text
nlink -> 0            => inode enters ORPHAN state
valid open references => still readable/writable (I19)
reclaim only when     nlink == 0 AND no valid open reference can exist
```

**All of this state is keyed by `FileID = (ino, inode_generation)`, not by
`ino`.** The data plane already scopes every object by the file's
incarnation ([data.md](data.md)); inode-scoped *ephemeral* state needs the
same protection, and for the same reason. An ino is reclaimed and reissued;
a delayed CLOSE, UNLOCK, or reservation-resolution naming `(ino=100, gen=7)`
must not touch `(ino=100, gen=8)`, or one file's straggler silently releases
another file's lock or drops its open reference. The rule is general: **an
operation acting through an inode handle validates the inode generation
before it changes anything**, and a generation mismatch is a stale-handle
error, never a successful no-op.

Open/close must **not** become heavyweight durable metadata mutations on the
hot path. The mechanism is **one session-scoped open lease per
(FileID, session)** — not one mutation per file descriptor:

```text
session's FIRST open of an inode:
    inode authority (inode_shard) Raft-commits  add open_session(session_id)
further local opens/dups of the same inode:
    free — tracked inside the client session, no metadata mutation
session's LAST close of the inode:
    inode authority commits  remove open_session(session_id)
session fenced:
    its open_session entries are stale; any authority may lazily verify
    against the session authority and drop them

reclaim when:  nlink == 0  AND  open_sessions == empty
```

This deliberately puts open-lifetime coordination on the inode shard, but
only **once per session/file lifetime** — not per I/O and not per
descriptor. That is a semantic serialization (P1): the coordination exists
because POSIX open-unlinked semantics require it, and it costs one Raft
entry at each end of a session's use of a file. A fenced (dead) client's
leases are safely retired by recovery, and a stale session's lease can
neither keep an inode alive nor be reused (I19). Reclaim is L6.

## Distributed POSIX locking

`fcntl()` byte-range locks and `flock()` must have **one global lock state**
across clients — a lock taken on client A must conflict with a lock requested
on client B. POSIX advisory locks do not fence ordinary reads/writes (an
application that ignores locks is allowed to), so the data path is untouched;
but every participating lock request must see the same authoritative state.

- **One lock authority per inode.** Lock state for a file lives on
  `inode_shard(ino)`, as `(FileID, start, end, owner)` records with overlap
  conflict checking, mutated through that shard's Raft log. This deliberately
  serializes lock *management* for one file — acceptable, because an
  application using record locks has explicitly asked for coordination; it is
  a semantic serialization (P1), not an accidental one. Splitting arbitrary
  overlapping ranges across shards is rejected: range overlap is not
  partitionable, and the common case (whole-file `flock`, small-range
  `fcntl`) is cheap on one authority.
- **Owner identity is per namespace, because Linux ownership is.** A single
  process-based owner token would model only classic `fcntl()`. Both
  `flock()` locks and OFD `fcntl()` locks are associated with the **open
  file description**, so duplicated descriptors (and descriptors inherited
  across `fork`) *share* them, while classic `fcntl()` locks are owned by
  the process and are dropped by closing any descriptor for the file. The
  owner token therefore differs by namespace:

  ```text
  classic fcntl  -> (client_uuid, session_epoch, process_id)
  OFD fcntl      -> (client_uuid, session_epoch, open_description_id)
  flock          -> (client_uuid, session_epoch, open_description_id)
  ```

  The client assigns a stable `open_description_id` per open file
  description and reuses it across `dup`/`fork`, which is what makes shared
  ownership work across a cluster. Locks are released on close/process exit
  as usual; a *dead client* is handled by the session protocol above — once
  its session is fenced, the lock authority reclaims every lock owned under
  that epoch, whichever namespace. Blocked waiters are woken in order.
- **Lock conflict domains (Linux target, stated precisely).** There are
  **two conflict domains, not three namespaces** — a distinction that is
  easy to get wrong, because the three lock *kinds* do not map one-to-one
  onto them:

  ```text
  record-lock domain (one domain, two ownership kinds)
      classic fcntl   owner = (client_uuid, session_epoch, process_id)
      OFD fcntl       owner = (client_uuid, session_epoch, open_description_id)
      -> the two kinds CONFLICT with each other by byte range, even
         within one process on one file descriptor

  flock domain
      flock           owner = (client_uuid, session_epoch, open_description_id)
      -> independent of the record-lock domain
  ```

  Ownership and conflict are separate questions. Classic and OFD `fcntl`
  locks differ in *who owns them* and in when they are released (any close
  vs. last close of the description), but they are the same kind of record
  lock and a conflicting pair blocks — POSIX.1-2024 specifies this for OFD
  locks, and Linux has behaved this way since 3.15. So the authority
  evaluates both kinds against one another in a single byte-range domain,
  keyed only by owner token. `flock()` is the genuinely separate one: it is
  not POSIX, and its independence from record locks is **Linux behavior**
  that efs reproduces because it targets Linux. Locks owned by an
  open file description are shared by duplicated and inherited descriptors
  in both domains. When a session is fenced, every lock owned under that
  epoch is reclaimed, in both domains.
