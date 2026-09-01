# Architecture

[Design](design.md) · [Failure tolerance](failure-tolerance.md) · [Operations](operations.md) · [Scaling roadmap](scaling-roadmap.md)

This is the document the system is built against. It states the goal, the
failure model, the consistency model, the invariants, and the architecture
that satisfies them. The [scaling roadmap](scaling-roadmap.md) is the
*increment plan* for the current implementation; this is the *specification*.
When a design question comes up, the answer is decided here first, then
reflected in the roadmap.

**Status: ratified Sep 1 2026; revised Sep 1 2026 after external protocol
review.** The metadata layer described here replaces the current whole-table
snapshot + 2PC design. The data path is unchanged in mechanism (client-direct
RDMA, 2+1 EC) but its commit semantics are now specified precisely (§5.7).

> **Naming intent.** The bar for this design is that it earns the *idea* of an
> "extreme filesystem": it scales as close as possible to the raw hardware.
> Every design decision below is checked against that bar — a component is
> wrong if it serializes work the hardware could have done in parallel.
>
> **On the name itself.** "extremfs" collides phonetically with **XtreemFS**
> (an existing open-source distributed FS; the XtreemFS® trademark is
> registered by Quobyte), and "EFS" is overwhelmingly associated with Amazon
> Elastic File System. Neither is a good public identity for a project meant
> to be found and attributed. The *idea* — never serialize what the hardware
> allows in parallel — is the identity; the public **name should be chosen to
> be distinctive and searchable**, and is deliberately left open here pending
> a proper trademark/search check. This document uses "efs" as the working
> name only.

---

## 0. Governing principles

Three principles decide every hard case below. When a design question has no
obvious answer, the answer is whichever option these three point to.

- **P1 · Never serialize work that the semantics and the hardware allow to
  happen in parallel.** A component is wrong if it serializes work the
  hardware could have done concurrently. The filesystem must not invent a
  conflict merely because two operations share an inode, a file, or a
  directory. Serialization is accepted only where the *semantics* demand it
  (O_APPEND EOF allocation, overlapping byte ranges, truncate, rename).
- **P2 · Co-locate what must commit atomically on the common path; distribute
  what can evolve independently.** Before reaching for a distributed
  transaction, first ask whether the two pieces of state can be placed so the
  common operation is one Raft entry on one shard. This is the answer to the
  extending-write problem (§5.7): chunk publication and its size lane share a
  shard, so they commit atomically with no cross-shard protocol.
- **P3 · Make stale work harmless instead of trying to prevent stale work
  from happening.** Inode generations, content epochs, immutable chunk
  generations, orphan candidate writes, Raft terms, and node incarnations are
  all the same move: a stale actor may *do* work, but it cannot make that
  work *visible*. This is cheaper and more robust than fencing every stale
  actor at every boundary.

---

## 1. Goal

A high-performance parallel POSIX file system.

| Property | Target |
|---|---|
| Objects (files + dirs) | ≥ 2³² (4.29 billion) **live** |
| Cluster size | 3 nodes (smallest) to 64 nodes |
| Reference size | 2³² objects on 4 nodes |
| Failure tolerance | survive any 1 node down with full read+write |
| Consistency | immediate cross-client visibility (no TTL caches) |
| Data path | client-direct, RDMA, 2+1 EC |
| **Scaling bar** | throughput tracks aggregate hardware (NVMe + NIC), not a software serialization point |

The 2³²-on-4-nodes number is the binding constraint. It forces metadata out
of RAM and onto local NVMe, and forces the metadata store to be paged rather
than serialized whole. The scaling bar forces the *write path* — not just the
read path — to parallelize across nodes and across clients, including many
writers to the **same** file.

## 2. Failure model

- Nodes fail by crashing (stop, restart, rejoin). No Byzantine behavior.
- The network can drop, delay, reorder, duplicate, and partition messages.
  Partitions heal.
- Up to **1 node** may be down at once and the system stays available (see
  §3 for the precise CAP-accurate meaning). RF=3 on metadata and 2+1 EC on
  data both tolerate exactly one failure.
- Clocks are loosely synchronized (NTP). Used for expiry/TTL and cache hints
  only — **never** for correctness of replication, fencing, or reads. In
  particular there are **no clock-based leader leases** on the authoritative
  read path.
- A node that loses its local storage rejoins empty and rebuilds from peers.
- Clients are **fail-stop / non-Byzantine**: they may crash, disconnect,
  retry, duplicate, or hold stale state, but they are not adversarial.
  Authorization/fencing at storage targets is therefore about *stale*
  clients, not malicious ones (see §5.7). If efs is ever exposed to hostile
  clients, capability-based data authorization becomes a requirement, not an
  option.

## 3. Consistency model

- **Single-shard metadata operations are linearizable** within the
  authoritative shard's Raft group.
- **Multi-shard metadata operations are atomic** across their participant
  shards according to the cross-shard transaction protocol (§5.6). The
  intended property for those is strict serializability of transactions.
- There is **no global ordering** between two independent operations on
  unrelated shards, and none is needed.
- **Data:** a `write()` that has returned success is durable and visible (see
  the precise commit state machine in §5.7). Un-`fsync`ed data can be lost on
  client crash only where POSIX permits it.

**Readdir concurrency contract (explicitly weak, by design).** POSIX leaves
readdir-under-concurrency loose, and efs takes that room rather than
promising distributed snapshot isolation. A `readdir` scan guarantees:

- every returned name **existed at some point during the scan**;
- **no name is returned twice** within one scan (the ordered-KV cursor is
  monotonic per shard; a spread-directory merge dedups by name);
- a `telldir` **cookie remains valid** for resuming the scan (it encodes the
  per-shard cursor position; a leader change does not invalidate it, since
  the KV state — not the leader — is what is scanned);
- a name that existed for the *entire* scan **may** be missed only if it was
  concurrently renamed/moved across the cursor boundary — the same allowance
  local POSIX filesystems make.

It does **not** promise a point-in-time snapshot: a concurrent create may or
may not appear, a concurrent unlink may or may not disappear. If a future
feature needs snapshot readdir, that is an MVCC read (§8) introduced
deliberately, not an accident of the scan.

**CAP-accurate availability.** With any one storage/server node crashed or
unreachable, every shard retains a quorum and operations remain available to
clients that can reach that quorum. A minority partition never serves
authoritative writes or stale authoritative reads. Brief unavailability
during leader election is expected. "Survive one node" means safety plus
continued operation with the remaining quorum — not literally zero
interruption for every client.

## 4. Invariants

These are the test oracle for the simulator (§9) and for `fsck`. A bug is a
violation of one of these. Invariants are stated over the **committed,
externally-visible projection** of state; cross-shard transaction *intents*
(§5.6) are internal and are governed by I17/I18, not by the naïve forms of
I6/I7.

**Replication / leadership (Raft's actual safety properties)**

- **I1 · election uniqueness.** At most one server is elected leader for a
  shard in a given term.
- **I2 · leader completeness.** Every subsequently elected leader contains
  every previously committed entry. (This follows from Raft's election
  restriction + log-matching + commit rules together — not from majority
  intersection alone.)
- **I3 · term fencing.** A node that observes a higher term immediately stops
  exercising old-term leader authority.
- **I4 · quorum acknowledgement.** No metadata mutation is acknowledged unless
  committed according to the shard's *currently authoritative* Raft
  configuration. ("No dual owner" refers to commit authority, not a node's
  subjective belief — a partitioned old leader may still believe it leads but
  cannot commit.)

**Metadata state**

- **I5 · name uniqueness.** Within a directory, one visible name maps to at
  most one inode.
- **I6 · no visible dangling dentry.** Every committed visible dentry resolves
  to a valid inode.
- **I7 · nlink.** `nlink` equals the number of committed visible namespace
  links.
- **I8 · no resurrection.** A committed namespace removal is never reverted by
  replay, recovery, rebuild, or a stale client.
- **I9 · absent ≠ unavailable.** A failure to establish authoritative read
  state never returns `ENOENT`. `ENOENT` is only ever returned for a name
  that is genuinely absent. (The Sep 1 dangling-dentry class.)
- **I10 · metadata durability.** An acknowledged committed metadata op
  survives any single-node failure.
- **I16 · request idempotency.** Replaying a mutation with the same operation
  ID cannot apply its logical effect more than once and returns a response
  compatible with the original completed operation.
- **I17 · transaction atomic visibility.** A cross-shard transaction is
  externally visible as committed everywhere or not committed anywhere.
- **I18 · safe reconfiguration.** No shard configuration transition permits
  two disjoint authoritative quorums for the same logical history.
- **I19 · open-unlinked lifetime.** An inode with `nlink=0` remains valid for
  pre-existing authorized open handles until those references are safely
  retired; it is never reclaimed or reused while such access is valid.

**Data**

- **I11 · EC durability.** A published chunk generation has at least two
  reconstructable fragments on distinct nodes / failure domains.
- **I12 · chunk consistency.** Every returned chunk corresponds to one valid
  committed generation and a legal serialization of the committed byte-range
  mutations.
- **I13 · generation coherence.** A reconstruction never mixes fragments from
  different generations.
- **I14 · publish-after-durability.** Metadata cannot publish generation G
  before G satisfies the EC durability requirement.
- **I15 · orphan-generation safety.** Fragments from a generation that was
  never published may be reclaimed without changing any committed state.
- **I20 · stale-writer fencing.** A stale client/node cannot overwrite or
  publish data over a newer committed chunk generation.
- **I21 · publish+size atomicity.** An extending write never commits its
  chunk publication without the corresponding size-lane update, nor the
  reverse — they are one Raft entry on one shard (§5.7). A reader never sees
  a size that covers unpublished bytes, nor published bytes that no committed
  size exposes.
- **I22 · epoch fencing.** A read or stat considers only the inode's current
  content epoch; a publication tagged with a superseded epoch is never
  visible (§5.7).

**Liveness (testable bounded forms)**

- **L1.** With a stable quorum and eventually-delivered messages, each shard
  eventually elects a leader.
- **L2.** Every committed op is eventually applied by every healthy replica.
- **L3.** A restarting replica with intact storage eventually catches up.
- **L4.** An empty replacement replica eventually catches up and can safely
  join.
- **L5.** Every prepared cross-shard transaction eventually reaches COMMIT or
  ABORT while the required quorums remain reachable.
- **L6.** Zero-link orphan inodes eventually reclaim after references
  disappear.
- **L7.** Unpublished EC generations eventually reclaim.
- **L8.** Reconfiguration eventually converges desired placement to actual
  placement once failures stop.

## 5. Architecture

### 5.0 Three planes, not two

- **Data plane** — chunk PUT/GET, 2+1 EC, client-direct over RDMA.
- **Metadata plane** — virtual shards, RF=3 Raft per shard, ordered applied
  KV on NVMe.
- **Control plane** — cluster membership, desired placement, shard
  reconfiguration, node incarnation, drain/rebuild. This is its own Raft
  group and is *not* an implicit authority for operations it does not itself
  make safe (see §5.8 on reconfiguration).

### 5.1 Identity and addressing

- **`efs_ino_t` is 64-bit.** The target is ≥ 2³² *live* objects, so the ID
  space must exceed that to cover deleted/recreated objects, avoid dangerous
  immediate reuse, tolerate stale client handles, and leave headroom.
- **Inode generation / incarnation.** Each inode carries a generation that
  changes on reuse, so a stale handle for a deleted `ino` never silently
  becomes a handle for a new object (ABA protection). A handle is
  `(ino, generation)`.
- **Allocation.** Inos are allocated in per-shard (or per-allocation-domain)
  ranges so allocation is shard-local and globally unique by construction.
  The exact scheme (range handout vs. `[domain | counter]`) is an
  implementation detail; the invariant is global uniqueness + ABA protection.

### 5.2 Sharding

- **4096 metadata shards.** `shard = ino & 0xFFF` (`shard_bits = 12`).
  Low-bit masking yields **interleaved buckets**, not contiguous ranges —
  which is what we want, because sequentially-allocated inos then balance
  across shards naturally. (The earlier `shard_bits = 20` was an arithmetic
  error: 2³² objects / 2²⁰ objects-per-shard = 2¹² = 4096 shards.)
- **Shard ≈ 1M inodes** at the 2³² target. Shards are cheap; a node hosts
  many.

### 5.3 Metadata placement — the decisive design decision

This is the choice everything else hangs off, so it is made explicitly.

- **`inode_shard(ino) = ino & 0xFFF`.** The authoritative inode row lives
  here.
- **`dentry_shard(parent_ino, name)`** is the open question the review
  flagged. efs chooses **dentry lives on the parent's shard**
  (`dentry_shard = inode_shard(parent_ino)`), with a **threshold-based
  spread** for huge directories (below).

**Why parent-shard dentries.** It makes the two dominant namespace ops
single-shard: `create`/`unlink` inside a directory touch only
`inode_shard(parent)` for the dentry. readdir is one ordered range scan on
one shard. The cost — a hot directory funnels to one leader — is real but
bounded, and it is the *right* trade because directory-locality is the common
case and the rsync/ecopy wins came from exactly that locality.

**The two-index problem, resolved.** We do **not** store the full mutable
inode row in both indexes. The dentry index stores a **projection**:

```text
(parent_ino, name) -> (ino, generation, type)        // dentry: small, lookup hint
(ino)              -> authoritative inode_row        // inode: single source of truth
```

The dentry value carries only what LOOKUP needs to route and to validate a
handle (`ino`, `generation`, `type`). All mutable attributes (mode, uid, gid,
size, mtime, nlink, …) live **only** in the inode row. This eliminates the
chmod/chown/mtime/size/nlink divergence the review warned about, and it makes
hardlinks clean: many `(parent,name)` keys point at one authoritative inode.

**Cost we accept:** a LOOKUP that needs attributes does a dentry get (parent
shard) then an inode get (inode shard) — two point-gets, possibly two shards.
That is the price of not duplicating mutable state, and it is paid only on
the attribute path, not the name-exists path.

**The CREATE co-location rule (load-bearing).** Parent-shard dentries only
make CREATE single-shard if the new inode lands on the *same* shard as the
dentry. So inode allocation is constrained by placement (P2):

```text
normal directory:  inode_shard(new_ino) = inode_shard(parent_ino)
spread directory:  inode_shard(new_ino) = hash(parent_ino, name) & 0xFFF
```

The allocator picks an ino whose low 12 bits equal the required shard (inos
are allocated per-shard, §5.1, so this is free — the shard's allocator simply
hands out its own inos). CREATE then writes the dentry *and* the inode row in
**one Raft entry on one shard**. Without this rule every CREATE would be a
distributed transaction; with it the dominant namespace op stays a single
log append. UNLINK of a last link is likewise single-shard (dentry + inode
row on the same shard). This rule has enormous performance consequences and
is not negotiable in the implementation.

**Operation → participant matrix (authoritative).** The earlier claim that
"only rename/hardlink/unlink-open touch two shards" was wrong under this
placement. The real matrix, given the co-location rule:

| Op | State touched | Shard(s) | Protocol |
|---|---|---|---|
| CREATE / MKDIR | dentry + inode row | **1** (co-located) | single Raft entry |
| UNLINK (last link) | dentry + inode row | **1** (co-located) | single Raft entry |
| UNLINK (nlink>1) | dentry on parent shard, nlink on inode shard | 2 | transaction (§5.6) |
| LOOKUP (name only) | dentry | 1 | single read |
| GETATTR / SETATTR | inode row | 1 | single Raft entry |
| LINK (hardlink) | new dentry on its parent shard, nlink on inode shard | 2 | transaction (§5.6) |
| RENAME same-dir | two dentries on parent shard | **1** | single Raft entry |
| RENAME cross-dir | src dentry, dst dentry, ino row, parent nlinks | ≥2 | transaction (§5.6) |
| WRITE non-extending | chunk-map entry | 1 (`chunk_meta_shard`) | single Raft entry |
| WRITE extending | chunk-map entry + size lane | **1** (co-located, §5.7) | single Raft entry |
| O_APPEND | EOF reserve + chunk + size | inode shard + chunk shard | reserve (§5.7) then write |
| TRUNCATE | content epoch on inode row | 1 | single Raft entry (§5.7) |
| READDIR | dentries | 1 (normal) / many (spread) | range scan / scatter-merge |
| CHMOD / CHOWN | inode row | 1 | single Raft entry |

**Hot-directory spread is a layout-epoch protocol, not a flag.** Crossing
`EFS_DIR_SPREAD_MIN` does not flip a bit — a 100M-entry directory cannot be
re-partitioned atomically, and lookups/creates/unlinks/readdir must keep
working *during* the move. The directory carries a **layout epoch**:

```text
dir.layout = LOCAL            all dentries on the parent shard
    |
    v  (threshold crossed; owner Raft-commits SPLITTING)
dir.layout = SPLITTING(e)     new writes go to hash(parent,name) shards;
                              a migrator moves existing dentries
                              idempotently, in batches
    |
    v  (all dentries moved; owner Raft-commits HASHED)
dir.layout = HASHED(e)        all dentries on hash shards; old local
                              range is GC'd
```

Read/write rules during `SPLITTING`: **writes go only to the hashed
location**; **reads check the hashed location first, then the not-yet-moved
local range**; the migrator's moves are idempotent (a dentry already moved is
a no-op), so a crash mid-split resumes cleanly; readdir merges both. The
layout epoch `e` lets a client cache the layout and detect a stale decision.
This is one of the hardest namespace problems in the design and is specified
as a protocol precisely because "threshold spread" alone is an intention, not
a solution.

### 5.4 One Raft group per shard

Each shard is an independent Raft group, replicated to **3 nodes** (RF=3).
One node loss leaves a majority. On a 3-node cluster every shard is on all
three; on 4–64 nodes shards spread by placement. Each group elects its own
leader; leaders spread across nodes, so metadata write throughput scales with
node count — there is no single metadata primary.

Every mutation is a log entry `(term, index)`. It is committed when a
majority of the group's replicas hold it durably. This is what makes I1–I4
and I10 true.

### 5.5 Applied state: a logical ordered KV per shard

Each replica applies its log to a **local, embedded, ordered key-value store**
on NVMe. RAM is a bounded cache, not the store.

- **Logical, not one DB instance per shard.** Each shard has an isolated
  ordered applied-state *namespace*. At 64 nodes a node hosts ~192 shard
  replicas (4096×3/64); hundreds of independent storage-engine instances
  would waste threads, memtables, block caches, WALs, and file descriptors.
  Implementation multiplexes many shard namespaces into one local storage
  engine using a shard-prefix key. Raft logs remain independently ordered per
  group.
- **Keys.** `(parent_ino, name) -> (ino, gen, type)` and `(ino) -> inode_row`
  (see §5.3). Ordered, so readdir is a range scan.
- **Why a log + KV, not a snapshot.** The current design serializes the whole
  table to discover what changed (`ser=350ms`/flush at 10M, `incr=0` on 100%
  of growth flushes). A log only ever writes what changed. Snapshots exist
  only for **log truncation** (a lagging replica catches up from a snapshot +
  recent log), never as the durability mechanism.
- **Capacity model (honest).** The naive "2³² × 128 B ≈ 512 GiB" is only one
  logical inode-row copy. Real persistent consumption = `(N_inodes ×
  inode_value_size + N_dentries × dentry_value_size + names) ×
  KV_amplification × RF`, plus Raft log, snapshots, tombstones, transaction
  and dedup state, and compaction headroom. Note `N_dentries ≥ N_inodes`
  (hardlinks). NVMe capacity is designed around this explicitly; it is not a
  512 GiB fits-and-done claim.

### 5.6 Cross-shard transactions

The operations that genuinely touch more than one shard (see the matrix in
§5.3: cross-dir rename, hardlink, unlink with nlink>1) need **atomic
visibility**, which a reconcile rule alone does not provide. efs uses a
**Raft-backed distributed transaction** — decentralized, no global
transaction server:

```text
txid (stable across all participants)
coordinator = a deterministic participant shard
participant set

PREPARE  -> each participant writes a durable intent
DECISION -> coordinator Raft-commits COMMIT | ABORT (durable)
RESOLVE  -> participants apply/abandon per the decision
```

Atomic *commitment* is only half the problem. The other half is
**concurrency control** — without it, two concurrent transactions over the
same keys (`rename(a,b)` vs `unlink(a)`; `rename(a,b)` vs `rename(a,c)`) have
no serializable history even though each commits atomically.

- **Conflict detection at PREPARE.** A prepare is conditional:
  `prepare(T, key, expected_version)`. It fails if another live transaction
  holds a conflicting intent on `key`, or if `key`'s current version no
  longer equals `expected_version` (the key changed since the transaction
  read it). An intent on a key is that key's lock: at most one live
  transaction holds it.
- **Deadlock freedom by deterministic ordering.** Participants and keys are
  prepared in a **canonical global order** (by shard id, then key). Because
  every transaction acquires intents in the same order, the wait-for graph is
  acyclic and deadlock cannot form. This is the minimalist choice — no
  wound-wait / wait-die machinery. (If a prepare finds a key already
  intended by another transaction, the later transaction aborts and retries:
  no-wait + retry, which is safe *because* ordering already makes true
  deadlock impossible; the retry bound is a liveness tuning knob, not a
  correctness mechanism.)
- **Visibility rule.** An intent is never externally visible as a committed
  effect; readers that encounter an intent resolve it against the
  coordinator's decision (or block/EBUSY), so clients never observe an
  illegal intermediate (I17).
- **Idempotency / retry.** The whole transaction carries one `txid`;
  re-prepare and re-resolve are idempotent (I16).
- **Crash recovery.** A recovering participant or a reader that finds an
  intent queries the coordinator's durable decision and drives the
  transaction to completion (L5).
- **GC.** Resolved intents are reclaimed (L7-class).

With conditional prepare + canonical ordering, the protocol yields strict
serializability for multi-shard ops: conflicting transactions are ordered by
intent acquisition, non-conflicting ones run concurrently.

**Terminology fix.** What is being *deleted* is the old **global
root-snapshot 2PC durability mechanism** — the one whose commit point was an
insufficiently-replicated coordinator/root-generation. A per-operation
Raft-backed 2PC-like protocol for cross-shard transactions is a different
thing and is exactly what's needed here. Raft (replicated ordering within a
shard) and 2PC (atomic commitment across shards) solve different problems;
efs uses each where it fits.

### 5.7 The data plane, specified precisely

This is where "scales with raw hardware" is won or lost. The mechanism
(client-direct RDMA, 2+1 EC) is unchanged; what is new is a precise commit
and concurrency protocol.

**Immutable chunk generations.** Each logical chunk's fragments are keyed by
`(ino, chunk_index, generation, fragment_index)`. A generation is immutable
once written. Reconstruction never mixes generations (I13). Metadata
publishes generation G only after G satisfies EC durability (I14); an
unpublished generation's fragments are orphans, reclaimable without touching
committed state (I15). This is what makes stale-client writes safe: a stale
client may write an orphan generation, but it cannot make it visible without
winning the metadata publication protocol (I20).

**The write commit state machine (direct write).**

```text
1. allocate chunk generation G
2. encode + store fragments of G (client-direct RDMA to 3 nodes)
3. obtain >= 2 durable fragment ACKs            (EC durability)
4. Raft-commit the metadata publication of G    (size/chunk-map/mtime)
5. apply the publication
6. return write success
```

So a `write()` that has returned success **is** durable and visible — stronger
than POSIX, and deliberately so. `fsync()` then only has to cover whatever
client-side batching efs intentionally permits plus namespace ordering. There
is no ambiguous gap between "write succeeded" and "metadata committed."

**Partial-chunk and concurrent writers (same file).** This is the hot-file
problem (review P0.14) and it gets a real answer. The governing rule:
**never serialize work that the semantics and the hardware allow to run in
parallel** — the filesystem must not invent a conflict merely because two
writes share an inode.

- **Chunk metadata is itself sharded.** This is the load-bearing detail. If
  every chunk-map key for a file lived on `inode_shard(ino)`, then a million
  writers to disjoint chunks would still funnel through **one Raft group /
  one leader**, because Raft serializes the group's log — independent *keys*
  do not help when they share one *log*. So chunk metadata is placed by

  ```text
  chunk_meta_shard(ino, chunk_index) = hash(ino, chunk_index) & 0xFFF
  ```

  One file's chunk publications now spread across many Raft leaders:

  ```text
  same file
     ├── chunk 0    -> metadata shard 81
     ├── chunk 1    -> metadata shard 2117
     ├── chunk 2    -> metadata shard 994
     └── chunk 3    -> metadata shard 3301
  ```

  A disjoint full-chunk write publishes its `(ino, chunk_index, gen)` entry
  on the shard that owns that chunk's metadata — not on the inode's shard.
  The data is distributed *and* the metadata publication is distributed. This
  is what makes the hot-file claim structurally true rather than a slogan.
- **Size is a monotonic high-water mark, sharded — and co-located with the
  chunk it extends (P2).** Two problems must be solved at once. (a) If the
  authoritative `size` lived in the single inode row, every `MAX` would
  serialize on that row's leader. (b) If the size update lived on a
  *different* shard than the chunk publication, an extending write would be a
  cross-shard transaction — and a crash between "chunk published" and "size
  advanced" would leave acknowledged bytes invisible (or worse, a visible
  hole). Both are solved by one placement decision:

  ```text
  size_lane(chunk_index) = chunk_meta_shard(ino, chunk_index)
  ```

  The size lane for a chunk lives on the **same shard as that chunk's
  metadata**. An extending write then commits **one Raft entry on one shard**
  carrying both effects:

  ```text
  { publish (ino, epoch, chunk_index, gen) ; MAX(size_lane, end_offset) }
  ```

  atomically — no distributed transaction on the hot write path. This is P2
  applied: state that must commit atomically on the common path is co-located
  by construction. The logical size is the MAX over the current
  content-epoch's lanes; the number of distinct lanes a file uses grows with
  its extent count, so the write side is exactly as parallel as the chunk
  metadata itself.
- **stat() has a linearization point despite the distributed reduction.**
  Reading `MAX` over several independently-linearizable shards is not
  automatically an atomic snapshot — a truncate could interleave between the
  lane reads. The content epoch closes it:

  ```text
  1. read inode row -> content_epoch E
  2. read only size lanes tagged E, take MAX
  3. re-read inode row -> content_epoch still E?
       yes -> the MAX is the linearizable size
       no  -> retry (a truncate is in flight)
  ```

  The epoch double-check gives stat() a well-defined linearization point
  without any cross-shard locking.
- **mtime has exact semantics.** A `write()` that has returned success has
  committed its mtime update as part of the same metadata publication (the
  shard leader stamps `max(mtime, now)` on the publication entry — cheap,
  because it rides the chunk publication, not a separate inode-row write). So
  a `stat()` after a returned `write()` — from any client — sees an mtime at
  least as new as that write. mtime is *not* lazily advanced in a way that
  could regress or lag a committed write; "coalescing" only means multiple
  writes published in one Raft entry share one timestamp. This is a defined
  efs behavior, not "where POSIX permits."
- **Sub-chunk read-modify-write** uses generation CAS: a writer builds
  candidate generation `G+1` from committed base `G`, publishes with
  `CAS(expected_generation = G)`; on conflict it refetches the committed
  generation, reapplies its byte-range patch, and retries. Concurrent
  *disjoint* byte-range writes to the same chunk thus both land (I12), with
  no whole-file lock. The invariant is "every returned chunk is a valid
  serialization of committed byte-range writes," not "old whole chunk or one
  writer's whole chunk."
- **Truncate is a content-epoch bump, with a stated linearization rule.**
  Truncate (or any wholesale content replacement) advances the inode's
  `content_epoch` on the inode row — a single-shard Raft entry. Every chunk
  publication and every size lane is tagged `(ino, content_epoch, …)`, and
  readers consider only the inode's current epoch. The rule:

  ```text
  a write publishing under epoch E linearizes BEFORE the operation that
  advances the file from E to E+1;

  a write that begins after truncate has returned uses epoch E+1.
  ```

  A writer that read epoch 17 and publishes after the file moved to epoch 18
  produces an epoch-17 publication that no reader will ever consider — P3:
  the stale write is harmless, not prevented. Old-epoch chunks and lanes are
  GC'd (L7).
- **O_APPEND is a two-step: a serialized reserve, then a parallel write.**
  EOF allocation is the one same-file serialization the semantics truly
  require, and it lives on `inode_shard(ino)`:

  ```text
  append authority = inode_shard(ino)
  reserve_append(len):  old = append_eof; append_eof += len; return old
  ```

  `reserve_append` is one Raft mutation on the inode shard (this is the
  accepted hotspot). The client then writes its reserved `[old, old+len)`
  range through the **ordinary distributed chunk path** — chunk publications
  and size lanes spread across shards as for any write, so concurrent
  appenders serialize only on the *reservation*, not on the data movement.
  **Crash semantics:** a client that dies after reserving but before
  publishing leaves a reserved hole (a zero-filled byte range) — this is
  POSIX-legal for O_APPEND (the reservation is the linearization point, and a
  crashed appender's bytes simply never appear), and it is never rolled back,
  because later appenders already received offsets past it. The hole is
  indistinguishable from a sparse write and is safe.

**Stale-target fencing — targets are dumb, metadata decides.** Because
clients write directly to data targets, Raft terms on the metadata path do
not fence the data path. But the fencing is *not* "target rejects an older
generation" — generations are immutable candidate objects, and two concurrent
clients may legitimately produce different candidate generations for the same
chunk with no global ordering between them. The correct model:

- A fragment object `(ino, content_epoch, chunk_index, generation,
  fragment_index)` is **immutable and its PUT is idempotent**. A target
  stores it; storing the same object twice is a no-op. A target may hold
  several candidate generations of one chunk at once — that is fine, because
  they are distinct immutable objects.
- The target verifies only **placement epoch, target incarnation, and object
  identity** — i.e. "am I the current, legitimate home for this object?" It
  does **not** decide which candidate generation is logically newer.
- **The metadata authority alone decides which generation is committed**
  (via the publication CAS). Unpublished candidates are orphans, reclaimed
  without touching committed state (I15).

This keeps the data nodes dumb — which fits the minimalist design — and it is
what makes I20 precise: a stale writer cannot make its generation *visible*,
because visibility is granted only by the metadata publication protocol, not
by anything a data target does.

### 5.8 Control plane, membership, and reconfiguration

- **Node incarnation.** Every process/storage incarnation has an identity
  that changes across a destructive restart: `(node_uuid,
  storage_incarnation, process_boot_id)`. Delayed packets from an old
  incarnation must not be confused with current state — Raft protects the
  metadata protocol; the control and data planes carry incarnation info for
  stale-incarnation rejection.
- **Desired placement ≠ authoritative configuration.** The control group
  chooses *desired* shard→node placement. It does **not** atomically
  reconfigure a live Raft group — that would be unsafe (the old replica set
  `{A,B,C}` and new `{D,E,F}` have zero majority intersection and could both
  make progress).
- **Safe reconfiguration.** A shard's own Raft group transitions its actual
  membership through an overlapping-quorum (joint-consensus) path, e.g.
  `{A,B,C} → joint{A,B,C,D,E} → {B,D,E} → …`. A replacement replica catches
  up sufficiently before becoming a voting member. This is I18; an incorrect
  rebalancer would invalidate every Raft safety argument, so it is specified,
  not left to the implementation.

### 5.9 Reads: the linearizable read protocol

"Committed" does not make an arbitrary replica safe to read. Authoritative
metadata reads (LOOKUP/GETATTR/readdir) go to the **shard leader**, which
establishes current read authority through Raft (a quorum-backed
ReadIndex-style mechanism) and **waits until its applied index ≥ the read
index** before reading the KV. Because clocks are never correctness inputs
(§2), there are **no clock-based leader leases** on the authoritative read
path. Followers either forward to the leader or use an explicitly specified,
proven linearizable follower-read mechanism.

### 5.10 Client request deduplication

The failure model allows duplicated messages and dying clients, and Raft does
not give application RPCs exactly-once semantics by itself. Every mutating
request carries a stable logical identity — `(client/session UUID, request
sequence)` or an operation UUID. The replicated state machine records enough
completed-request state to make ambiguous retries idempotent (I16). This is
critical for O_APPEND, create, link, unlink, rename, truncate, mkdir. The
same `txid`/op-ID threads through all participants of a cross-shard
transaction.

### 5.11 Open-unlinked inode lifetime

I6/I7 allow `nlink = 0` while a file is open. The lifecycle:

```text
nlink -> 0            => inode enters ORPHAN state
valid open references => still readable/writable (I19)
reclaim only when     nlink == 0 AND no valid open reference can exist
```

Open/close must **not** become heavyweight durable metadata mutations on the
hot path. Open references are tracked with fencing so a dead client's
reference is safely retired (recovery) and a stale client's reference cannot
keep an inode alive or be reused (I19). Reclaim is L6.

### 5.12 Distributed POSIX locking

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
- **Owner identity and death.** A lock owner is `(client_incarnation,
  process_id)`. Locks are released on close/process exit as usual; a *dead
  client* is detected via its incarnation going stale (§5.8), and its locks
  are reclaimed by the authority. Blocked waiters are woken in order.
- **flock vs fcntl** are kept as separate namespaces (POSIX semantics), both
  on the same per-inode authority.

### 5.13 FUSE / kernel cache coherence — including file data

"No TTL caches" is not sufficient, because Linux/FUSE itself maintains a
dentry cache, negative dentry cache, attribute cache, and **page cache**
above the efs-fuse daemon. Immediate cross-client visibility must include
that kernel cache — a perfectly linearizable metadata layer is defeated if
client B's kernel serves a stale *data* page after client A committed a
write.

- **Metadata caches.** Default `entry_timeout = 0`, `negative_timeout = 0`,
  `attr_timeout = 0`; the low-level (inode-based) FUSE API gives explicit
  inode/dentry invalidation notifications, and efs uses server-driven
  invalidation to keep the kernel metadata cache honest without TTLs.
- **Data cache: direct-I/O semantics, correctness first (decided).** The
  kernel page cache is **not authoritative and is bypassed** for file data:
  reads and writes go through to the efs client, which serves them from the
  current committed chunk generations. This is the minimalist
  correctness-first answer — it makes cross-client data visibility exactly as
  strong as metadata visibility, with no coherence protocol to get wrong.
  Distributed range invalidation or a lease/cache-coherence protocol may be
  introduced **later**, only if it materially improves performance, and only
  after zero/stale-cache semantics are demonstrably correct without it.
- **mmap.** A genuinely coherent cross-client `MAP_SHARED` mapping is not a
  side effect of this architecture — if A maps a page and B writes the chunk
  directly, what A observes and when is a hard coherence problem.
  **Decision: full cross-client coherent `MAP_SHARED` is unsupported in the
  first implementation** (mmap'd writes are either rejected or given
  write-through, same-client-only visibility semantics, documented in the
  POSIX contract). It is documented as a limitation rather than implicitly
  promised. `MAP_PRIVATE` (copy-on-write, never shared) is unaffected.

### 5.14 Modularity as an architectural constraint

Modularity is not a style preference here — it is what makes the two things
this project depends on possible at all: **fast isolated testing** (§9, §9a)
and **bounded-context change** (a human or a model editing one component
without ingesting the whole codebase). A system this subtle cannot afford
either to be slow to test or to require global knowledge to change safely.

**The rule.** A module must be understandable, changeable, and testable from
**its own source plus its interface header alone**. If you have to read the
rest of the tree to change one component safely, the modularity has failed —
regardless of how the directories are named.

**Where we are today (honest).** The code is *not* there. Four files hold
~45% of the 36.5k-line tree — `metadata.c` (6042 lines), `efs_fuse.c` (3720),
`meta_server.c` (3654), `handler.c` (3648). That is why every change is slow,
every test pulls in the world, and every review needs the whole file in
context. The target module boundaries below are drawn to fix exactly this.

**Target boundaries** (aligned with the planes, so the architecture and the
code structure are the same map):

```text
raft/       the consensus core — a pure state machine, transport- and
            storage-agnostic; no I/O inline, no globals. Testable in the
            simulator and in a unit harness alike.
kv/         the ordered applied state — behind a storage interface
            (real NVMe engine / simulated fault-injecting disk).
meta/       the POSIX op handlers — pure-ish functions over the kv/ and
            raft/ interfaces; no socket or FUSE calls inline.
wire/       the protocol — versioned encode/decode, nothing else.
data/       the data plane — EC encode/decode, RDMA PUT/GET, generation
            fencing.
client/     the FUSE adapter — thin; translates FUSE ops to meta/data calls.
```

**Rules that enforce it:**

- **Depend on the interface, not the implementation.** Modules include each
  other's *headers*, never reach into another module's `.c` internals. The
  current `g_server->lock` / shared-global pattern is the anti-example — it
  is what forced whole-subsystem context for every change.
- **State machines are pure.** No hidden globals, no I/O inline; all I/O goes
  through the transport/storage interfaces. This is *also* the property that
  lets the same compiled state machine run under the simulator (§9) — purity
  buys testability and simulatability at once.
- **Every module has a unit test that links only its real dependencies** (or
  interface fakes) and runs in milliseconds. A change to `raft/` must not
  require a `client/` rebuild — mentally or literally.
- **Context budget.** A module plus its interface should fit in a few hundred
  lines, so a change can be made and reviewed with bounded context. Working
  target: **no source file over ~1000 lines**; split by responsibility when
  one crosses it. This is the property that lets a model (or a new
  contributor) load one module and make a correct change without the whole
  tree in scope.

**Why it is in the architecture doc and not a style guide:** the migration
(§10) lands Raft, KV, and the transaction protocol as *new* components. If
they are built to these boundaries from the start, the simulator and the unit
harness get them for free, and the dev cycle (§9a) stays fast as the system
grows. If they are built as another 6000-line `metadata.c`, no amount of
testing infrastructure will save the cycle time.

## 6. Why the theory holds up

Let's be precise about what is and isn't claimed. **Raft-per-shard over an
embedded KV is not a new mechanism.** The claim is stronger than novelty: the
design is a *consequence of the constraints*, and the argument is a
derivation, not an analogy. A copy-cat cites a working system and imitates
it. A derivation starts from what must be true and shows which shapes
survive.

### The constraints are forces, not preferences

| Constraint | Force it exerts | Design it eliminates |
|---|---|---|
| 2³² objects on 4 nodes | ~512 GiB+ of metadata cannot live in RAM | any fully-in-RAM table (today's) |
| immediate cross-client visibility | no authoritative cache may go stale | TTL / negative / attr caches as truth |
| survive 1 node down, read *and* write | a write must commit without the dead node | single-coordinator 2PC, primary-backup without failover |
| 3 → 64 nodes | work must spread, not funnel to one writer | a single cluster-wide metadata primary |
| **scale with raw hardware** | parallel work must not hit a software serializer | whole-table flush; per-write inode-row mutation |

### The constraints strongly favor one family; efs picks proven primitives within it

The constraints do not *mathematically* force "Raft + ordered KV"
specifically — Multi-Paxos or Viewstamped Replication, and a B-tree or LSM or
custom page store, satisfy the same forces. The honest statement:

> The constraints strongly favor a **partitioned replicated state machine
> with persistent, paged applied state**. efs chooses **Raft** and an
> **ordered embedded KV** because they satisfy those requirements with
> proven, understandable primitives and minimal new protocol invention.

That this is also where Ceph and CockroachDB landed is not imitation — it is
convergent evolution. The same forces act on any strongly-consistent,
horizontally-scaled, fault-tolerant store, so independent systems solve the
same equations and arrive at the same shape. Convergence is evidence the
derivation is correct, not that it was copied.

### The safety properties are inherited theorems plus explicit composition obligations

- **Core shard-replication safety is inherited from Raft** when implemented
  according to its assumptions (election restriction, log matching, commit
  rules, safe reconfiguration). Leader completeness (I2) is Raft's theorem,
  not majority intersection alone.
- **Quorum durability** (I10/I11): a majority-acknowledged write survives any
  failure that leaves a majority. With RF=3 that is exactly one node. This is
  why "survive 1 node down" is a theorem here. The old design's root/snapshot
  2PC did not provide this — its commit point was an under-replicated
  coordinator — and the bug history (dual-writer root, peers pinned at an old
  gen, adopt-before-pages-local) is the system repeatedly discovering that.
- **EFS-specific composition is *not* covered by Raft's proof.** RPC dedup,
  KV persistence ordering, cross-shard transaction logic, FUSE cache
  coherence, EC generation publication, chunk RMW, reconfiguration, and the
  orphan lifecycle remain **explicit proof / model / simulation
  obligations**. That is exactly what the simulator (§9) is for.

### What is genuinely ours — stated precisely

Let us be exact about the claim, because overclaiming here would be both
wrong and unsupportable. **Every primitive in this design has existed:**
Raft, distributed metadata, RDMA, erasure coding, immutable generations,
ordered KV stores, directory sharding, deterministic simulation. Several
systems get close to individual parts — WEKA (fully distributed data +
metadata, no dedicated MDS tier, hash placement, strong POSIX, NVMe), DAOS
(NVMe-native, low-latency fabric, distributed transactions, EC, versioned
writes — though its POSIX layer still lacks hardlinks and distributed flock),
VAST (distributed transactional metadata + EC + flash, but a shared-everything
DASE model, not shared-nothing Raft shards), CephFS (client-direct data +
dynamic metadata, but authority in a distinct MDS tier). IndexFS/GIGA+
precede the threshold-spread huge-directory idea. FoundationDB is the
canonical deterministic-simulation example. We are not claiming to have
invented any of these.

**The claim is the composition and the governing principle.** What we have
not found in a public system is this exact architecture presented as one open
POSIX filesystem:

```text
POSIX namespace
  + no dedicated MDS tier
  + metadata authority distributed across ordinary storage nodes
  + Raft only for small authoritative metadata state
  + client-direct RDMA data path
  + 2+1 EC immutable chunk generations
  + no whole-file serialization for parallel writers
  + no inode serialization where independent chunks can proceed
  + directory locality until it becomes a bottleneck
  + failure correctness designed before performance tuning
  + deterministic protocol simulation
  + small-cluster simplicity
```

And the principles that tie it together — the things that, if we execute
them, are the actual contribution (§0):

> **P1 · Never serialize work that the semantics and the hardware allow to
> happen in parallel.**
> **P2 · Co-locate what must commit atomically on the common path; distribute
> what can evolve independently.**
> **P3 · Make stale work harmless instead of trying to prevent it.**

Traditional PFS architectures handle `1000 clients → 1000 different files`
well. The hard problem is `1000 clients → ONE file → different byte/chunk
ranges`. efs's answer is that if writes do not conflict, the filesystem must
not invent a conflict merely because they share an inode: disjoint chunks
publish to different metadata shards (§5.7), size is a sharded high-water
mark co-located with the chunk it extends (P2), sub-chunk RMW is generation
CAS, stale publications are fenced by content epochs rather than prevented
(P3), and only the operations whose *semantics* require serialization
(O_APPEND EOF allocation, overlapping byte ranges, truncate, rename) are
serialized — because the semantics demand it, not because the filesystem
happens to have one inode lock.

That is a thesis strong enough to build a filesystem identity around. It is
also falsifiable: if a workload that the hardware could parallelize is found
to serialize in efs, that is a bug against this principle, and it is the
simulator's and the benchmarks' job to find it.

### The namespace is the database

A second, quieter part of the identity. Traditional filesystems hand you a
namespace and leave you to build `find`, `du`, crawlers, inode scanners, cron
lifecycle jobs, and external catalogs around it. But efs metadata is already
a distributed ordered database. So the questions operations actually asks —
largest files, files owned by X, files older than Y, everything under project
Z, storage consumed by a directory, data whose TTL expired — should be
**metadata queries, not filesystem traversals**:

```text
du         = a query over the ordered KV, not a namespace walk
find       = a range scan / index lookup, not a crawl
expiration = an indexed metadata operation, not a cron forest
```

The namespace metadata *is* the database; there is no separate catalog bolted
on and derived from periodic snapshots. This is a natural consequence of the
metadata design, not an extra subsystem — and it is part of what "minimalist"
means operationally (below).

**Stated honestly, this is a thesis the architecture makes *possible*, not a
mechanism it already specifies.** The base keys — `(parent,name)` and `(ino)`
— efficiently answer lookup, getattr, and readdir. They do not by themselves
answer "all files owned by uid 1000", "everything older than 30 days", or
"all .bam files > 1 TiB" without scanning billions of inode rows; and
`du /project/foo` is genuinely hard because subtree membership is
hierarchical. Making `find`/`du`/TTL real queries requires one of:
**secondary indexes** (which must stay consistent with inode mutation —
another same-shard co-location or transactional-indexing decision, P2),
**distributed materialized aggregates** (per-subtree size/count maintained
incrementally), or **query-time parallel shard scans** (the KV is ordered and
sharded, so a full scan at least parallelizes across all 4096 shards instead
of walking a tree serially). The architecture's commitment is narrower and
real: namespace-wide questions are answered **without walking the POSIX
namespace**, because the data is already in a queryable distributed store.
The derived-index design is a separate, explicitly-scoped follow-on.

### Minimalism as a design constraint

What may distinguish efs as much as any algorithm is operational simplicity:

```text
3 servers  ->  install  ->  efs init  ->  efs mount  ->  done
64 servers ->  the same architecture
```

No metadata servers to provision, no metadata-target sizing exercise, no
hierarchy of special node types, no weekly `du`, no crawler database, no
lifecycle cron forest, no kernel module, no twenty-component deployment.
Delivering that *and* serious performance *and* strong POSIX semantics is the
goal. The architecture serves it: ordinary storage nodes are the metadata
consensus participants, so there is nothing extra to operate.

**The thesis in one line:** the constraints force the family, Raft + KV are
the proven primitives within it, and the part that is ours — the composition,
the never-serialize-what-can-run-parallel principle, and the
namespace-as-database minimalism — is where the design earns an identity.

## 7. Why this satisfies the goal

- **2³² on 4 nodes:** metadata is paged from NVMe by the KV; RAM is a bounded
  cache. Capacity is modeled honestly (§5.5), not assumed to fit.
- **3 → 64 nodes:** shards re-balance through safe reconfiguration (§5.8);
  leaders spread; adding a node moves some shard replicas onto it. Smallest
  cluster (3) runs every shard at RF=3 on all three.
- **1-node tolerance:** RF=3 per shard + 2+1 EC on data both survive one
  loss (§3 for the precise availability claim).
- **Immediate visibility:** ops are applied on the leader in log order and
  acknowledged only when committed; kernel caches are actively invalidated
  and file data bypasses the page cache (§5.13). There is no cache to go
  stale.
- **Scale with raw hardware:** the data path is client-direct RDMA + EC, and
  the hot-file metadata path publishes independent chunk keys rather than
  serializing on an inode row (§5.7). Metadata op cost is one leader RTT + a
  majority replication — the floor for any consistent system — and it no
  longer degrades with table size or with same-file concurrency.

## 8. What is deliberately rejected

- **Millions of tiny consensus groups beyond the shard count.** 4096 groups
  is the ceiling; more re-creates the catchup-storm class.
- **MVCC / multi-version metadata.** Not required by the current filesystem
  semantics and intentionally omitted. The Raft log gives mutation *order*;
  it is not a substitute for MVCC. Introduce MVCC only if future snapshot,
  scan, or cross-shard transaction semantics require versioned reads.
- **A second storage path for metadata pages.** Everything goes through the
  same log + KV. (Two-paths-diverge has bitten twice.)
- **Client-side metadata caching for correctness.** Caches may exist for
  performance but are never authoritative (and the kernel cache is actively
  invalidated, §5.13).
- **Kernel module.** Stay on FUSE; the low-level (inode-based) FUSE API
  migration is a separate, orthogonal client rewrite.
- **Clock-based leader leases on the authoritative read path.** Clocks are
  not correctness inputs (§2, §5.9).

## 9. The simulator (build first, architecture-independent)

The single highest-leverage tool, and it pays for itself regardless of the
metadata design.

**What it is.** A deterministic discrete-event simulator that runs N logical
servers + M clients **in one process**. A seeded PRNG drives message order,
drops, delays, duplicates, partitions, node crashes/restarts, and clock
steps. The real metadata state machine (Raft groups, KV apply, op handlers,
cross-shard transactions, membership) runs inside it against a **simulated
network and simulated disk**, not sockets and NVMe.

**Why it changes the dev cycle.** Today every correctness gate is a 13-node
wipe + rsync + rebuild + ssh orchestration, and the signal is poor (99% of
posixstress "failures" are 15s timeouts = saturation, not correctness).
Distributed bugs are only findable end-to-end on physical hardware — the
slowest, least reproducible place possible. In the simulator a full cluster
scenario runs in milliseconds, a failure replays exactly from its seed, and
you explore millions of interleavings overnight. The invariants of §4 are
checked after every simulated step, so a bug is a seed + a violated
invariant, not a log line on a live cluster.

**How it is built.** The metadata core is written to be **transport- and
storage-agnostic**: it sends messages and reads/writes disk through
interfaces. Two backends implement those interfaces: the real one (sockets +
NVMe, what ships) and the simulated one (a message queue + a fault-injecting
in-memory disk, what the simulator drives). The same compiled state machine
runs in both, so "passes in simulation" is meaningful.

**Fault-injection events the generator must produce:**

```text
lost client replies · duplicated client RPCs
node restart with same disk · node restart empty / new incarnation
delayed packets from a previous incarnation
leader crash after local append but before follower send
leader crash after quorum commit but before reply
leader crash after commit but before local apply
snapshot creation during writes · snapshot transfer interruption/restart
membership-transition interruption at every step
cross-shard coordinator crash in every transaction phase
participant crash in every transaction phase
client crash after fragment PUT before metadata publish
client crash after metadata commit before reply
stale client placement map · stale client chunk generation
```

**Independent checking.** Record complete operation histories and run an
independent linearizability / transaction-history checker, rather than
relying exclusively on handwritten invariants. (This is the FoundationDB
methodology: deterministic whole-cluster single-process simulation, seeded
replay, network/disk/machine fault injection, and the goal of finding
correctness issues in simulation rather than production.)

**Scope — it models the *logical* data protocol, not the wire.** The
simulator does not model RDMA mechanics (verbs, packetization, bandwidth, NIC
behavior) — that is the perf harnesses' job. But it **must** model the
logical data-commit protocol and its interaction with metadata, or it cannot
check I11–I15 and I20. So the simulated world includes abstract data-plane
events:

```text
PUT fragment · durable ACK · dropped ACK · target crash · fragment lost
client crash mid-write · metadata publication · stale generation arrives
rebuild from fragments
```

A "fragment" in the simulator is an abstract durable object with an identity
and a home, not bytes on an RNIC. With those events the checker can verify
that no committed read ever reconstructs from mixed generations (I13), that
nothing is published before durability (I14), that orphans never become
visible (I15/I20), and that a one-node loss never loses a published chunk
(I11) — under every crash/interleaving the generator can produce.

## 9a. Shortening the code → signal cycle

The bottleneck is not writing code — it is **how long a change takes to prove
itself**. Today that proof is a 13-node wipe + rsync + rebuild + ssh
orchestration, and the signal is poor: 99% of posixstress "failures" are 15s
timeouts (saturation, not correctness), and a single run is noise that needs
≥3 fresh-wipe repeats. The strategy is to **push each class of bug to the
cheapest layer that can catch it** — and to fill the deterministic gap the
simulator occupies.

The layers, cheapest first:

| Layer | Answers | Cost | Catches |
|---|---|---|---|
| **unit** (`make test`) | is this function right | ms | logic, encode/decode, pack/unpack |
| **simulator** | is the *protocol* right under any interleaving / failure | ms | message-ordering, leader/fencing, recovery, the §4 invariants |
| **synthetic bench** (`efs-bench --meta`) | is it fast, did it regress | seconds | throughput/latency regressions, op-storm cost |
| **posix / posix2** | is it a correct filesystem (vs XFS) | minutes | semantic / peer-visibility gaps |
| **13-node cluster** | does it perform on real hardware | slowest | perf ceilings, RDMA, NVMe — **not** correctness |

A bug should be caught at the **lowest** layer that can see it. Today
correctness bugs fall all the way to the top because nothing in the middle is
deterministic. **The simulator is the next step because it is the only missing
layer that is both fast and deterministic** — unit tests can't see a message
race, the cluster can't reproduce one.

Deliberate moves that shorten the loop:

- **Correctness moves down, performance stays up.** The cluster stops being
  the correctness oracle; posix/posix2 confirm semantics; the cluster confirms
  speed. A correctness bug should never need a 13-node wipe to find.
- **Deterministic over flaky.** Replace fork-and-pray races with forced
  collisions (`threading.Barrier`) so a test either always collides or never
  runs — no more "passes isolated, fails under load."
- **Fail fast, not timeout.** A violated invariant aborts at the step, not
  after a 15s `POSIX_TEST_SEC`. Saturation (a timeout) is reported separately
  from correctness (an assertion) so the two are never conflated again.
- **Live attach over rebuild-and-reprobe.** ptrace is open on all 15 hosts —
  `gdb -p` on a wedged `efsd` answers in seconds what an NDJSON-probe redeploy
  answers in tens of minutes.
- **Bounded-context change.** Modularity (§5.14) keeps the unit of work small:
  a change loads one module + its interface header, not the whole tree. This
  is what makes both fast isolated tests and model-assisted editing tractable.
- **Invariants as executable checks** (simulator assertions + `fsck`), not
  prose — so "is this a bug" is decidable without a human reading a log.

## 10. Migration shape (high level)

Not a big-bang rewrite. Order chosen so each step is gated and the system
stays runnable. **Do not** go straight `KV → Raft → done`.

```text
0. Freeze the exact metadata placement model:
   inode_shard() · dentry_shard() · authoritative row ownership ·
   inode IDs + generations · the CREATE co-location rule (§5.3) ·
   size_lane = chunk_meta_shard (§5.7).
1. Simulator interfaces + the CURRENT state machine.
2. Define RPC operation IDs + the idempotency model.
3. Ordered KV applied state, incl. atomic batch semantics.
4. Single-shard Raft: persistence · election · replication · apply ·
   ReadIndex/linearizable reads · snapshots.
5. Simulator proves the single-shard invariants.
6. Safe Raft-group reconfiguration + control-plane desired placement.
7. Cross-shard transaction protocol, incl. concurrency control (§5.6).
8. Open-unlinked inode lifecycle.
9. Data-generation publication / fencing integration (§5.7), with the
   simulator checking the logical data protocol (§9).
10. Directory layout-epoch spread (§5.3) + distributed locking (§5.12).
11. Delete the old snapshot / root-2PC machinery.
12. FUSE cache-coherence optimization only after zero/stale-cache semantics
    are demonstrably correct (data path starts as direct-I/O, §5.13).
```

**Step 1 has a hard prerequisite: the carve-up.** A pure state machine behind
transport/storage interfaces does not exist today — four files hold ~45% of
the tree and the state machine is fused with its I/O. So the *concrete* first
move is **Phase M** in the roadmap: carve the monolith into `raft/ kv/ meta/
wire/ data/ client/` behind interfaces (§5.14), as behavior-preserving
refactor gated by the existing suites. It is the dev-cycle lever in its own
right *and* the thing that makes step 1 possible — the simulator can only
reuse a state machine that is already pure. Build nothing in steps 2–11 as
new monolith code; every new component lands inside the carved boundaries.

The detailed, gated steps live in the [scaling roadmap](scaling-roadmap.md);
this document is the invariant they are measured against.

---

## Appendix: terms

- **Shard** — an interleaved bucket of the inode space (`ino & 0xFFF`); the
  unit of replication and leadership.
- **Raft group** — the 3 replicas + log for one shard.
- **Leader** — the single writer for a shard in a given term.
- **Term** — a monotonically increasing leadership epoch; used for fencing.
- **Committed** — present on a majority of the group's current authoritative
  configuration.
- **KV** — the embedded on-disk ordered key-value store holding applied state
  (one logical namespace per shard, multiplexed into a shared local engine).
- **Control plane** — the small Raft group that owns cluster membership and
  desired placement.
- **Chunk generation** — the immutable version of a logical chunk's fragment
  set; publication is atomic via metadata commit.
- **Intent** — a durable prepared record written by a participant in a
  cross-shard transaction; never externally visible as a committed effect.
