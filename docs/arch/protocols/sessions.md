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

Records are reclaimable once **all three** hold: the session is fenced, all
of its outstanding ambiguity is resolved (the barrier above has completed),
and the retention window has passed. Transaction decision records
([transactions.md](transactions.md)) have the matching condition: reclaimable
once every participant has acknowledged the decision and no recovering
participant can still ask for it.

## Open-unlinked inode lifetime

I6/I7 allow `nlink = 0` while a file is open. The lifecycle:

```text
nlink -> 0            => inode enters ORPHAN state
valid open references => still readable/writable (I19)
reclaim only when     nlink == 0 AND no valid open reference can exist
```

Open/close must **not** become heavyweight durable metadata mutations on the
hot path. The mechanism is **one session-scoped open lease per
(session, inode)** — not one mutation per file descriptor:

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
  `inode_shard(ino)`, as `(ino, start, end, owner)` records with overlap
  conflict checking, mutated through that shard's Raft log. This deliberately
  serializes lock *management* for one file — acceptable, because an
  application using record locks has explicitly asked for coordination; it is
  a semantic serialization (P1), not an accidental one. Splitting arbitrary
  overlapping ranges across shards is rejected: range overlap is not
  partitionable, and the common case (whole-file `flock`, small-range
  `fcntl`) is cheap on one authority.
- **Owner identity and death.** A lock owner is `(client_uuid,
  session_epoch, process_id)`. Locks are released on close/process exit as
  usual; a *dead client* is handled by the session protocol above: once its
  session is fenced, the lock authority reclaims its locks. Blocked waiters
  are woken in order.
- **Lock namespaces (Linux target, stated precisely).** `flock()` is not
  POSIX at all, and the non-interaction between `flock()` and `fcntl()`
  namespaces is **Linux behavior**, not a POSIX guarantee — efs targets
  Linux/POSIX, so it reproduces the Linux contract: classic `fcntl()`
  (process-associated) locks, POSIX.1-2024 **OFD `fcntl()` locks**
  (open-file-description-associated — included in the target), and `flock()`
  are three separate namespaces, all on the same per-inode authority, and
  (as on Linux) locks from different namespaces do not conflict with each
  other.
