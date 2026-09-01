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
RDMA, k+f EC) but its commit semantics are now specified precisely (§5.7).

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
- **P4 · The hot I/O path never crosses cores, NUMA nodes, or software locks
  unnecessarily.** A distributed design that scales perfectly on paper still
  fails the goal if each node leaves half its hardware idle. Ordinary
  PUT/GET and metadata commits run NUMA-local, asynchronous, queue-depth
  driven, zero/minimal-copy, with no cross-core lock on the common path
  (§5.15).

---

## 1. Goal

A high-performance parallel POSIX file system.

| Property | Target |
|---|---|
| Objects (files + dirs) | ≥ 2³² (4.29 billion) **live** |
| Cluster size | 3 nodes (smallest) to 64 nodes |
| Reference size | 2³² objects on 4 nodes |
| Failure tolerance | **configurable f = 1…3** simultaneous node losses (§2); no data loss at the configured f; automatic authority through f losses requires N ≥ 2f+1 (N = 2f is a labeled durability-only mode) |
| Consistency | immediate cross-client visibility (no TTL caches) |
| Data path | client-direct, RDMA, k+f EC (k=2 default) |
| **Scaling bar** | throughput scales with the **protection-adjusted** aggregate hardware ceiling (§7); no software serialization point may become the limiter before a physical resource does |
| **Hardware envelope** | **modern flash only** — NVMe SSDs + RDMA-capable NICs. Hard disks are explicitly out of scope (below) |

**Hardware envelope: flash/NVMe-only, by decision.** EFS does not support
spinning disks, and no design effort is spent on them. This is not an
omission — it is a scoping decision that the architecture actively spends:

- **Random I/O is first-class.** There is no seek penalty to hide, so no
  seek-aware layouts, no write-sequencing-for-disks, no elevator thinking.
  The KV pager's random page faults and the read path's random chunk reads
  are *cheap* by assumption.
- **Deep hardware queues are assumed.** The P4 execution model (§5.15) —
  asynchronous, queue-depth-driven, one NVMe queue pair per reactor — only
  exists because the devices do. A HDD cannot run that model at all.
- **Microsecond device latency is what the durability choice costs against.**
  `write()` = durable (§5.7) pays a synchronous round trip to *device*
  per write; that is affordable because the device is µs-scale flash, not
  ms-scale disk.
- **No SMR/shingled, no rotational-latency hiding, no track alignment** —
  entire problem classes deleted, not engineered around.

If a deployment needs HDDs, that is a different filesystem; efs's scaling
claims (§7) are made against flash and do not transfer.

The 2³²-on-4-nodes number is the binding constraint. It forces metadata out
of RAM and onto local NVMe, and forces the metadata store to be paged rather
than serialized whole. The scaling bar forces the *write path* — not just the
read path — to parallelize across nodes and across clients, including many
writers to the **same** file.

The scaling bar, stated measurably: **for operations without semantic
conflicts, EFS throughput must scale with the aggregate protection-adjusted
NIC, NVMe, CPU, and memory bandwidth of the participating nodes. No software
serialization point — a mutex, one inode leader, one metadata thread, FUSE
serialization, one WAL, one coordinator — may become the limiting resource
before one of those physical resources does.** When a scaling benchmark stops
early, the answer to "where did it stop" must be a physical resource (NIC,
NVMe, memory bandwidth, EC compute, fabric bisection); if the answer is
software, that is by definition an EFS bug. "Near-linear" is defined per
workload class in §7 — several POSIX operations are serial *by semantics* and
are excluded from the claim rather than silently failing it.

## 2. Failure model

- Nodes fail by crashing (stop, restart, rejoin). No Byzantine behavior.
  **"Losing a node" means its storage is gone**, not just the process — the
  guarantees below are for permanent loss; a crash-with-intact-disk is a
  strictly easier case covered by the same configuration.
- The network can drop, delay, reorder, duplicate, and partition messages.
  Partitions heal.
- **Configurable failure target f.** The cluster is initialized with a
  maximum number of simultaneous permanent node losses it must survive
  **without data loss**, capped at **f = 3** and bounded by cluster size.
  The cap is deliberate: metadata quorum size (write latency) and EC storage
  overhead both grow with f, and f=3 already covers the realistic
  correlated-failure envelope for a single rack/cluster.

**The math that sets the bound.** The two planes scale differently with f:

- **Data (EC):** a `k+f` stripe survives any f losses (any k of k+f
  fragments reconstruct). Needs **N ≥ k+f** distinct failure domains.
- **Metadata (Raft):** a committed entry lives on a quorum
  Q = ⌊RF/2⌋+1. Zero data loss through f *permanent* losses needs **Q > f**
  (every committed entry keeps at least one copy), and RF ≤ N — which forces
  **N ≥ 2f**. The system *staying authoritative with no human in the loop*
  through f failures needs the survivors to still form a quorum:
  **N ≥ 2f+1**.

So the headline rule is simple: **the configured-f guarantee requires
N ≥ max(2f+1, k+f)**, with RF = 2f+1 voting replicas per shard. Lose any f
nodes: a metadata majority survives, ≥k data fragments survive, and the
system remains authoritative **automatically** — no quorum override, no
disaster mode, no distinction between "bits survive" and "we can prove which
bits were committed."

| N nodes | max f (automatic) | metadata RF | data EC | guarantee |
|---|---|---|---|---|
| 3 | 1 | 3 | 2+1 | lose 1: no loss, stays authoritative |
| 4 | 1 | 3 | 2+1 | lose 1: no loss, stays authoritative |
| 5 | 2 | 5 | 2+2 | lose 2: no loss, stays authoritative |
| 6 | 2 | 5 | 2+2 | lose 2: no loss, stays authoritative |
| ≥ 7 | 3 | 7 | 2+3 (or wider k, e.g. 4+3 = 1.75×) | lose 3: no loss, stays authoritative |

**f=3 needs 7 nodes, not 5 or 6, for the automatic guarantee.** On 5 nodes
the best metadata configuration is RF=5, Q=3, and 3 ≯ 3 — the 3 dead nodes
could be exactly the quorum that acknowledged the most recent writes. The
config validator rejects any configuration that violates
N ≥ max(2f+1, k+f) for the automatic guarantee.

**N = 2f is offered only as an explicitly labeled durability-only mode.** At
N = 2f exactly (4 nodes configured f=2, 6 nodes configured f=3), the system
still survives f permanent losses with **no data loss** — every committed
entry keeps ≥1 surviving copy (Q + f > N), and ≥k data fragments survive —
but the affected shards **cannot re-establish authority on their own**: the
survivors are a minority, and plain Raft has no safe way to continue or
reconfigure without a majority. This mode pauses rather than loses
(CAP-consistent), and recovery is a **specified operator-gated
disaster-recovery protocol, not normal Raft**:

```text
per affected shard, only after the operator confirms the dead nodes are
permanently gone (`efs-mgmt force-reconfigure`):

1. FREEZE: fence the shard's old configuration (placement epoch bump)
2. COLLECT the logs of ALL surviving replicas
3. MERGE per log index: on conflict, the highest-term entry wins.
   A committed entry is never shadowed by a conflicting entry at the same
   index (Raft leader completeness: any later leader — and hence any
   higher-term entry — already contains every previously committed entry),
   and every committed entry is present on ≥1 survivor (Q+f > N), so the
   merge provably retains every acknowledged write. Uncommitted survivors
   of the merge were never acknowledged; op-ID idempotency (I9) makes
   their late application harmless.
4. FORCE a new single-node configuration from the merged log
5. re-add replicas to RF via normal joint consensus (§5.8)
```

This is deliberately the one operation in the system that requires a human.
(The single-survivor degenerate case is what etcd ships as
`force-new-cluster`; the multi-survivor merge above is what makes the
general N=2f case safe rather than a gamble on one member's log.)

**Changing f is an online control-plane operation with an explicit
transition state.** The cluster carries `effective_f` (the guarantee
currently in force) and, during a change, `target_f`. Raising f: add shard
replicas (via joint consensus, §5.8), re-stripe **every protected data
generation** to the new k+f width, verify — and only then commit
`effective_f = target_f`. The guarantee changes at that commit, not when the
operator asks. Lowering f runs the reverse. Both are placement changes, not
reformats. EC stripe width k is independently configurable (wider k on
larger clusters trades CPU for storage efficiency); the binding constraint
is N ≥ max(2f+1, k+f) for the automatic guarantee, N ≥ max(2f, k+f) for
durability-only mode.

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
- **One `write()`/`pwrite()` publishes atomically.** POSIX makes regular-file
  `read()`/`write()` effects atomic with respect to one another, so a
  concurrent reader never observes a mix of old and new chunks from a single
  in-flight write: publication is one atomic decision covering every chunk
  the call touched (§5.7), while the *data movement* stays fully parallel.
  The atomicity unit is one write syscall as delivered to the client (one
  FUSE write request stream; see the `max_write` boundary note in §5.7).
  Once the call has returned, every subsequent read sees all of it. An
  explicit **relaxed mode** (mount option, for applications that synchronize
  themselves — MPI-IO-style) publishes chunk by chunk for maximum
  publication throughput; it is documented as weaker than POSIX and is never
  the default.

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

**CAP-accurate availability.** At the configured failure target f (§2): any
f nodes may be permanently lost with **no data loss**. With the automatic
guarantee (N ≥ 2f+1), every shard retains a quorum after the losses and
operations remain available to clients that can reach it — no operator
action, ever. In the explicitly-labeled durability-only mode (N = 2f),
affected shards pause (no writes, no authoritative reads) rather than lose
data, and return via the operator-gated disaster-recovery protocol of §2. A
minority partition never serves authoritative writes or stale authoritative
reads. Brief unavailability during leader election is expected. "Survive f
nodes" means safety always, plus continued operation whenever a quorum
survives — not literally zero interruption for every client.

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
  survives any f simultaneous permanent node losses, where f is the
  configured failure target (§2; quorum Q > f by construction).
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

- **I11 · EC durability.** A generation published in a **healthy** cluster
  has all k+f fragments durable on distinct failure domains, so any f
  simultaneous losses leave at least k reconstructable fragments. A
  generation published while `u` failure domains are **already unavailable**
  has at least `k + (f − u)` durable fragments, is marked degraded, and is
  repaired when capacity returns (§5.7).
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
- **I23 · session fencing.** No mutation from a fenced (superseded) client
  session epoch is ever applied by any shard leader or data target, and no
  lock, open reference, or append reservation owned by a fenced session
  survives its fencing (§5.12a).
- **I24 · atomic write publication.** A returned `write()`/`pwrite()` is
  visible in its entirety to every subsequent read; a concurrent read during
  the call sees either all or none of that call's chunks (default mode;
  §5.7).

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

- **Data plane** — chunk PUT/GET, k+f EC (§2, §5.7), client-direct over RDMA.
- **Metadata plane** — virtual shards, one Raft group per shard at
  RF = min(N, 2f+1) (§2, §5.4), ordered applied KV on NVMe.
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
nlink, …) live **only** in the inode row — with two deliberate exceptions
defined in §5.7: *write-generated* size and mtime live in the file's bounded
write lanes (otherwise every write would serialize on the inode shard), and
`stat()` merges them under the double-collect snapshot protocol. Explicit
attribute changes (`utimens`, `chmod`, `chown`, truncate) always update the
inode row. This eliminates the chmod/chown/mtime/size/nlink divergence the
review warned about, and it makes hardlinks clean: many `(parent,name)` keys
point at one authoritative inode.

**Cost we accept:** a LOOKUP that needs attributes does a dentry get (parent
shard) then an inode get (inode shard) — two point-gets, possibly two shards.
That is the price of not duplicating mutable state, and it is paid only on
the attribute path, not the name-exists path.

**The CREATE co-location rule (load-bearing) — and its directory
correction.** Parent-shard dentries only make CREATE single-shard if the new
inode lands on the *same* shard as the dentry. So inode allocation is
constrained by placement (P2). But a naive "child inherits the parent's
shard" rule has a fatal consequence: a directory's children inherit its
shard, *their* children inherit it again, and an entire subtree funnels into
one Raft leader until each directory individually grows hot enough to spread
— "many independent directories → near-linear" would be false for exactly
the common case. So the rule distinguishes files from directories:

```text
CREATE regular file:
    normal directory:  inode_shard(new_ino) = inode_shard(parent_ino)
    spread directory:  inode_shard(new_ino) = hash(parent_ino, name) & 0xFFF

MKDIR (any directory, always):
    inode_shard(new_dir)  = hash(parent_ino, name, export_salt) & 0xFFF
```

**Files are local; directories scatter.** A new file shares its directory's
shard, so file CREATE writes dentry + inode row in **one Raft entry on one
shard** — the dominant namespace op stays a single log append. A new
*directory* is deliberately placed on its own hash shard: MKDIR pays one
two-shard transaction (dentry on the parent shard, inode row on the home
shard, via §5.6) and in return buys **an independently scalable subtree for
the directory's entire lifetime** — `/projectA/*` and `/projectB/*` live on
different shards from birth, not after a spread event. Directory creation is
rare next to file creation, so the transaction cost is amortized to nothing;
the parallelism it buys is permanent. The per-export `salt` (chosen at mkfs)
prevents correlated names from clustering across exports.

The allocator picks an ino whose low 12 bits equal the required shard (inos
are allocated per-shard, §5.1, so this is free — the shard's allocator simply
hands out its own inos).

**The stranded-set bound still holds, now for files.** A normal directory's
*files* keep their inode rows on the directory's shard forever — their ino
identity encodes the shard (`ino & 0xFFF`) and cannot migrate.
`EFS_DIR_SPREAD_MIN` is therefore a **scalability bound, not just a space
threshold**: it caps how many file rows can be permanently co-located on one
shard before the directory spreads. It is kept low enough that the stranded
set is irrelevant at scale (thousands of rows, never millions).

**Operation → participant matrix (authoritative).** The earlier claim that
"only rename/hardlink/unlink-open touch two shards" was wrong under this
placement. The real matrix, given the co-location rule:

| Op | State touched | Shard(s) | Protocol |
|---|---|---|---|
| CREATE (file) | dentry + inode row | **1** (co-located) | single Raft entry |
| MKDIR / RMDIR | dentry on parent shard + dir inode on its home shard | 2 | transaction (§5.6) |
| UNLINK (last link) | dentry + inode row | **1** if `dentry_shard == inode_shard` (and nlink==1), else 2 | single Raft entry / transaction |
| UNLINK (nlink>1) | dentry on parent shard, nlink on inode shard | 2 | transaction (§5.6) |
| LOOKUP (name only) | dentry | 1 | single read |
| GETATTR (stat) | inode row **+ L write lanes** (size/mtime merge, §5.7) | 1 + L reads | double-collect (§5.7) |
| SETATTR | inode row | 1 | single Raft entry |
| LINK (hardlink) | new dentry on its parent shard, nlink on inode shard | 2 | transaction (§5.6) |
| RENAME same-dir | two dentries on parent shard | **1** | single Raft entry |
| RENAME cross-dir | src dentry, dst dentry, ino row, parent nlinks | ≥2 | transaction (§5.6) |
| WRITE non-extending | chunk-map entry | 1 (`chunk_meta_shard`) | single Raft entry |
| WRITE extending | chunk-map entry + size lane | **1** (co-located, §5.7) | single Raft entry |
| O_APPEND | EOF reserve + chunk + size | inode shard + chunk shard | reserve (§5.7) then write |
| TRUNCATE | content epoch on inode row | 1 | single Raft entry (§5.7) |
| READDIR | dentries | 1 (normal) / many (spread) | range scan / scatter-merge |
| CHMOD / CHOWN | inode row | 1 | single Raft entry |

(The UNLINK-last-link row is conditional because of a hardlink corner case:
after `create /a/foo; link /a/foo /b/foo; unlink /a/foo`, the surviving dentry
is on `/b`'s shard while the inode row is on `/a`'s — the final unlink of
`/b/foo` is then a two-shard transaction.)

**Hot-directory spread is a layout-epoch protocol, not a flag.** An honest
admission first: a *normal* directory deliberately serializes its creates on
one shard leader — a locality choice, and the right one (directory locality
is the common case), but it means P1 is satisfied by the *spread mechanism*,
not by pretending the conflict doesn't exist. The spread trigger therefore
responds to **actual serialization pressure, not just namespace size**:

```text
spread when:  entries > EFS_DIR_SPREAD_MIN
           OR sustained op rate / queue depth / latency on the
              directory's shard crosses a pressure threshold
```

(a 100-entry directory with 100,000 clients creating and unlinking never
crosses the size threshold but melts its leader — pressure-triggered spread
catches it). Spread is one-way initially: once HASHED, a directory does not
collapse back. And crossing the trigger does not flip a bit — a 100M-entry
directory cannot be re-partitioned atomically, and
lookups/creates/unlinks/readdir must keep working *during* the move. The
directory carries a **layout epoch**:

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

Each shard is an independent Raft group, replicated to **RF = min(N, 2f+1)**
nodes, where f is the configured failure target (§2) — RF=3 at the default
f=1, up to RF=7 at f=3 on ≥7 nodes. Replicas of one shard always occupy
distinct nodes (failure domains). On a 3-node cluster every shard is on all
three; on larger clusters shards spread by placement. Each group elects its
own leader; leaders spread across nodes, so metadata write throughput scales
with node count — there is no single metadata primary.

Every mutation is a log entry `(term, index)`. It is committed when a
majority of the group's replicas hold it durably. This is what makes I1–I4
and I10 true. The quorum arithmetic (Q > f for durability, survivors ≥ Q for
availability) is derived in §2; the control plane's own membership group
follows the same RF rule.

**4096 logical groups are not 4096 physical mini-databases.** A node must
never run per-group timers, WALs, fsyncs, sockets, heartbeats, or threads —
on a 3-node cluster that would be ~1365 leaders per node doing bookkeeping
instead of work. The implementation is a **multi-Raft runtime**: a few
reactor threads (per NUMA domain, P4) drive many shard state machines; Raft
messages are batched by destination, heartbeats coalesced, AppendEntries
pipelined, WAL writes group-committed across groups, and KV applies batched —
while each shard keeps its own independent logical ordering, terms, and
commit indices. This is the same logical/physical split as §5.5's KV, and it
is an architectural requirement, not an optimization: without it the
bookkeeping cost of 4096 groups consumes the hardware the groups were meant
to exploit.

### 5.5 Applied state: a logical ordered KV per shard

Each replica applies its log to a **local, embedded, ordered key-value store**
on NVMe. RAM is a bounded cache, not the store.

- **Logical, not one DB instance per shard.** Each shard has an isolated
  ordered applied-state *namespace*. At 64 nodes a node hosts ~192 shard
  replicas at RF=3 (4096×3/64), ~448 at RF=7 (4096×7/64) — while the number
  of *leaders* per node stays ~64 (4096/64) at any RF, which is what matters
  for write scheduling; hundreds of
  independent storage-engine instances
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
§5.3: mkdir/rmdir, cross-dir rename, hardlink, unlink with nlink>1 or with
the dentry on a different shard than the inode) need **atomic
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
(client-direct RDMA, EC) is unchanged; what is new is a precise commit
and concurrency protocol.

**Erasure coding is k+f, matched to the failure target.** The stripe is k
data + f parity fragments on k+f distinct nodes, where f is the configured
failure target (§2): 2+1 at f=1, 2+2 at f=2, 2+3 at f=3 (storage overhead
1.5×/2×/2.5×). k=2 is the minimal-width default; larger clusters may widen k
(e.g. 4+3 = 1.75×) to trade encode CPU for capacity. Any k fragments
reconstruct the chunk, so reads survive any f losses by construction. Writes
place all k+f fragments; while nodes are down, a write that cannot place its
full stripe either blocks on repair or commits a degraded stripe that is
explicitly queued for re-striping — never silently under-protected.

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
2. encode + store fragments of G (client-direct RDMA to k+f nodes)
3. obtain ALL k+f durable fragment ACKs         (EC durability, see below)
4. Raft-commit the metadata publication of G    (size/chunk-map/mtime)
5. apply the publication
6. return write success
```

Step 3 waits for **all** k+f fragments, not the fastest k: a generation
published with only D durable fragments survives f losses only if
D − f ≥ k, i.e. D = k+f. Publishing earlier would open a window where
killing the right f nodes loses an acknowledged write — a violation of I11,
not a performance trade-off. (The previous "≥ 2 of 3 ACKs" rule had exactly
this hole at f=1.) Write latency therefore tracks the slowest fragment
target, not the fastest k; that is the honest price of the guarantee, and it
is hidden by queue depth and pipelining (§5.15), never by weakening the
durability definition.

**"Durable ACK" is defined precisely, because the whole
returned-write-is-durable semantic rests on it.** A durable fragment ACK
means the storage target has completed the required **persistent-NVMe
operation** for the fragment — an NVMe flush / FUA write, or a write into
power-loss-protected (PLP) media — *not* that the RDMA WRITE completed.
RDMA completion only reports on the RDMA operation; it says nothing about
the NVMe durability boundary. The same precision applies to Raft: a follower
ACK means the log entry has reached the persistence level efs defines as
durable (WAL on PLP media, or flushed/FUA), not that it sits in a volatile
write cache. If the deployment's SSDs have volatile write caches without
PLP, efs requires FUA/flush on the durability-critical writes; "returned
write == durable" is only as strong as this definition.

**Degraded publication.** There is one legitimate exception, and it is
precise: while the cluster is *already* operating with `u` unavailable
failure domains (`u ≤ f` failures already consumed), a generation may publish
with `D ≥ k + (f − u)` durable fragments — the invariant "survives the
remaining f−u further losses" is maintained throughout. At f=1 with one node
down that is D ≥ 2: the two surviving fragments suffice, because losing
another node would be a *second* simultaneous failure, outside the configured
model. Such a generation is marked **degraded** in its publication entry and
queued for repair; when capacity returns, the missing fragments are
re-striped and the degraded mark cleared (§5.15 rebuild scheduling). Degraded
publication is never used just because a target is slow — only because it is
unavailable.

So a `write()` that has returned success **is** durable and visible — stronger
than POSIX, and deliberately so (POSIX requires durability only at `fsync`).
This is a deliberate **latency-for-durability trade**, and it is not free:
the fast path is a small distributed synchronous commit per write (EC encode
+ k+f durable fragment writes + a durable quorum publication), so
single-threaded small-write latency and IOPS pay it. Peak *throughput* is
recovered through asynchronous parallelism, queue depth, and the batching of
§5.4/§5.7 — not by weakening the guarantee. The stronger semantic buys
simpler failure reasoning (no ambiguous gap between "write succeeded" and
"metadata committed") and it is benchmarked brutally against the
strict-POSIX alternative (`write()` = visible, `fsync()` = durable) before
the decision is considered final. `fsync()` then only has to cover whatever
client-side batching efs intentionally permits plus namespace ordering.

**Partial-chunk and concurrent writers (same file).** This is the hot-file
problem (review P0.14) and it gets a real answer. The governing rule:
**never serialize work that the semantics and the hardware allow to run in
parallel** — the filesystem must not invent a conflict merely because two
writes share an inode.

- **Chunk metadata is itself sharded — into a bounded number of per-file
  write lanes.** This is the load-bearing detail, and it has two halves. If
  every chunk-map key for a file lived on `inode_shard(ino)`, then a million
  writers to disjoint chunks would still funnel through **one Raft group /
  one leader**, because Raft serializes the group's log — independent *keys*
  do not help when they share one *log*. But spreading one file's chunk
  metadata across all 4096 shards overshoots in the other direction: it would
  give a large file up to 4096 authoritative size lanes, and a linearizable
  `stat()` would need up to 4096 remote reads. The design point is the
  middle: **distribute enough to remove the bottleneck, not more** (P1+P2).

  ```text
  lane(ino, chunk_index)      = hash(ino, chunk_index) % L
  chunk_meta_shard(ino, ci)   = hash(ino, lane(ino, ci)) & 0xFFF
  ```

  Each file gets **L write lanes**; each lane lives on one metadata shard
  (different files' lanes still scatter across all 4096 shards, so global
  balance is preserved). A lane's shard carries, for every chunk assigned to
  it, the chunk-map entry **plus the lane's size high-water mark and write
  mtime** — so chunk publication, size update, and mtime update for an
  extending write are **one Raft entry on one shard** (P2; this is what makes
  I21 hold with no cross-shard protocol):

  ```text
  { publish (ino, epoch, chunk_index, gen)
  ; MAX(lane.max_end,  end_offset)
  ; MAX(lane.max_mtime, now) }
  ```

  **L is derived, not arbitrary:**

  ```text
  L = required hot-file publication rate / per-shard commit rate (with
      publication batching, below) — and capped at tens of lanes.
  ```

  The target is 64 nodes; if 32–64 lane leaders already drive every node's
  data bandwidth, spreading one file wider buys nothing and only makes
  `stat()` expensive. A disjoint full-chunk write publishes its
  `(ino, chunk_index, gen)` entry on its lane's shard — not on the inode's
  shard. The data is distributed *and* the metadata publication is
  distributed, up to L leaders per file. This is what makes the hot-file
  claim structurally true rather than a slogan.
- **stat() is a distributed snapshot, not a bare MAX.** Reading `MAX` over
  several independently-linearizable shards is not automatically an atomic
  snapshot — and the content-epoch double-check alone only closes
  *truncate-vs-stat*, not *concurrent extending writes across lanes*: with
  lanes A=B=100, a stat that reads A=100, then sees writers commit A=1000
  and B=500, then reads B=500 would return 500 — a size the file never had.
  So each lane carries a monotonically increasing `lane_seq` (bumped by
  every lane mutation), and stat() is a **double collect**:

  ```text
  1. read inode row -> content_epoch E, mtime_gen M
  2. collect all L lanes:  (lane_seq, max_end, max_mtime)   [entries tagged E]
  3. collect all L lane_seq values again
  4. re-read inode row -> E, M unchanged?
  5. if every lane_seq is unchanged AND E/M unchanged:
         the lane vector existed simultaneously (all values held between the
         end of collect 1 and the start of collect 2) -> MAX is linearizable
     else: retry
  ```

  The common path stays lock-free: 2L+2 point reads, L bounded at tens. To
  keep a continuously-written file from starving the retry loop, the retry
  count is bounded (a few); on exhaustion stat() falls back to a **read-only
  multi-shard transaction** over the L lanes (§5.6 machinery: read intents
  on the lane set, one consistent read) — the correctness escape hatch, rare
  in practice. The same double collect validates the distributed mtime
  below.
- **mtime has exact semantics — and does not reintroduce the inode
  bottleneck.** Write-generated mtime lives **in the lanes**, not the inode
  row: the lane leader stamps `max_mtime` on the publication entry, so a
  million writers never touch `inode_shard(ino)`. Explicit timestamp changes
  (`utimens`, truncate, chmod) still update the inode row's `base_mtime`,
  and bump an `mtime_gen` counter on the inode row; lanes stamp their
  `max_mtime` updates with the `mtime_gen` current at publication time.
  `stat()` computes, under the same double-collect validation:

  ```text
  size  = MAX over current-epoch lanes of max_end
  mtime = MAX( inode.base_mtime,
               lanes' max_mtime where lane.mtime_gen == inode.mtime_gen )
  ```

  (The `mtime_gen` guard is what prevents a pre-`utimens` write's lane mtime
  from resurrecting over an explicit timestamp set.) So a `stat()` after a
  returned `write()` — from any client — sees an mtime at least as new as
  that write, and an explicit `utimens` is honored exactly. This is a defined
  efs behavior, not "where POSIX permits."
- **Chunk publications are batched at the Raft layer.** Semantics stay
  per-chunk; physical persistence does not. At 128 KiB chunks, 100 GB/s of
  logical write bandwidth is ~800k chunk publications/s — one synchronous
  metadata transaction per chunk would make the metadata plane the ceiling
  long before the NICs or NVMe are. So the client partitions publication
  records by lane shard and the shard leader commits **many publications per
  Raft proposal**, with one durable group commit covering the batch (this
  composes with the multi-Raft runtime's group-committed WAL, §5.4). The
  per-chunk publication record, its CAS, and its lane updates are unchanged
  — only the physical commit is amortized.
- **One write syscall publishes atomically; the data movement does not
  serialize.** A `write()` spanning several chunks uses the §5.6 transaction
  machinery for the *visibility decision only*:

  ```text
  chunk candidates (immutable generations)
       ↓  all written in parallel, client-direct RDMA
  per-lane publication INTENTS (prepared, not yet visible)
       ↓
  ONE durable write-txid commit decision (coordinator shard)
       ↓
  all intents become visible together
  ```

  Physical work — EC encode, fragment writes, even the per-lane intent
  prepares — stays parallel (P1); only the final visibility decision is
  atomic, which is exactly what POSIX read/write atomicity requires. A
  reader that meets an uncommitted intent resolves it against the durable
  decision record (committed → treat as visible; aborted/unknown → treat as
  absent), the standard intent-resolution pattern. Many writers to disjoint
  chunks of one file still run fully in parallel: their transactions touch
  disjoint keys and do not contend. The **relaxed mode** (§3) skips the
  transaction and publishes per-chunk.
  **The FUSE boundary question is stated honestly.** With
  `FUSE_CAP_ASYNC_DIO` the kernel may split one large write syscall into
  several concurrent FUSE write requests, and FUSE does not tag requests
  with their originating syscall. EFS therefore defines the atomicity unit
  as **one FUSE write request**, and sizes `max_write`/`max_pages` so the
  write sizes that matter arrive as a single request; a syscall larger than
  `max_write` is split by the kernel into `max_write`-sized atomic units —
  the same effective granularity Linux O_DIRECT exhibits on local
  filesystems, and it is documented as such in the POSIX contract. Whether
  the full syscall boundary can be recovered under async-DIO splitting
  (request coalescing by `(fh, offset)` contiguity) is an explicit open
  investigation before this semantic is considered final.
- **Sub-chunk read-modify-write** uses generation CAS: a writer builds
  candidate generation `G+1` from committed base `G`, publishes with
  `CAS(expected_generation = G)`; on conflict it refetches the committed
  generation, reapplies its byte-range patch, and retries. Concurrent
  *disjoint* byte-range writes to the same chunk thus both land (I12), with
  no whole-file lock. The invariant is "every returned chunk is a valid
  serialization of committed byte-range writes," not "old whole chunk or one
  writer's whole chunk."
- **The small-write envelope is declared honestly.** A 4 KiB write inside a
  128 KiB chunk pays the immutable-generation RMW: read 128 KiB, construct a
  new 128 KiB generation, write k+f fragments — roughly **80× data-path
  amplification** for the logical 4 KiB, and two disjoint 4 KiB writes in the
  same chunk contend on one generation CAS (an invented conflict at *chunk*
  granularity — P1 is violated there by construction). This is a deliberate
  scoping decision: **efs is optimized for HPC-sized, aligned I/O; sub-chunk
  random updates intentionally pay RMW amplification**, and the scaling
  claim in §1 does not cover random 4K mutation. If benchmarks later show
  the workload needs it, the designed escape hatch is **immutable delta
  objects** (a small write appends a delta object to the chunk's publication
  rather than rebuilding the chunk; a background consolidation folds deltas
  into a new base generation) — that makes disjoint sub-chunk writes
  independent, at real complexity cost. It is not built until measured.
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
- **O_APPEND is a serialized reservation sequence feeding a parallel data
  path.** EOF allocation is the one same-file serialization the semantics
  truly require, and it lives on `inode_shard(ino)`:

  ```text
  append authority = inode_shard(ino)
  reserve_append(len):  old = append_eof; append_eof += len
                        record reservation (old, len, client session)
                        return old
  ```

  `reserve_append` is one Raft mutation on the inode shard (the accepted
  hotspot). The client then writes its reserved `[old, old+len)` range
  through the **ordinary distributed chunk path** — chunk publications and
  size lanes spread across the file's lanes, so concurrent appenders
  serialize only on the *reservation*, never on the data movement:

  ```text
  reserve 10    reserve 11    reserve 12      (serial, inode shard)
  data 10  ─┐
  data 11  ─┼─ all run concurrently           (parallel, lane shards)
  data 12  ─┘
  visibility frontier advances over contiguous resolved reservations
  ```

  **The visible commit frontier is serialized — and must be.** O_APPEND is
  `(determine EOF; write)` as one atomic operation, so B's reserved offset
  only *means* anything because A's reservation was ordered before it. If A
  reserves `[0,100)` and B reserves `[100,200)`, B's data cannot become
  visible while A is unresolved: exposing B's bytes with A's range
  meaningless would expose a state no legal serialization produced. So the
  visible EOF is a **frontier that advances in reservation order** over the
  longest contiguous prefix of *resolved* reservations — B waits for A's
  resolution, not because of an implementation lock, but because append
  semantics order them. **Data movement is parallel; allocation order and
  the visible commit frontier are serialized.** That is serializing only
  what O_APPEND inherently requires.

  **Crash semantics, stated precisely.** The reservation watermark
  (`append_eof`) is internal allocation state — it is *not* the visible EOF;
  the visible size advances only as the frontier crosses resolved
  reservations. Every reservation must eventually *resolve*: it completes
  (its publication commits) or, once the owning client session is fenced
  (§5.12a), recovery **commits the reserved range as a zero hole** — a
  deliberate committed state, not an emergent one — which unblocks the
  frontier for everyone behind it. Only failure recovery can leave the
  frontier briefly blocked behind an unresolved reservation. The precise
  POSIX statement: a hole arises only from a write that never returned
  success, it reads as zeros (indistinguishable from a sparse region), and
  no returned write is ever lost. Reservations are never rolled back,
  because later appenders already received offsets past them.

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

**Atomicity of one multi-chunk write call.** A single `pwrite(fd, 4 MiB, …)`
spans 32 chunks on up to 32 lane shards, and the independent per-chunk
publication model above makes the chunks visible **as they commit** — a
concurrent reader can observe a mix (first 12 chunks new, remaining 20 old)
while the call is in flight. This is a stated semantic decision, not an
oversight:

- **Default: per-chunk atomicity.** The atomicity unit of the data plane is
  one chunk publication. A multi-chunk `write()` becomes visible chunk by
  chunk; once `write()` has returned, *every* subsequent read from any client
  sees all of its chunks. Only a reader *concurrent with the in-progress
  call* can observe a mix — the same observable behavior as Linux O_DIRECT
  (which can tear a large direct write), and consistent with the direct-I/O
  decision in §5.13. The POSIX contract documents this explicitly, and the
  HPC target workloads (MPI-IO with explicit synchronization between
  producers and consumers) do not rely on cross-chunk call atomicity.
- **Strong path: transactional publication.** When atomic multi-chunk
  visibility is required, the write executes as a cross-shard transaction
  (§5.6): a write `txid`, one publication *intent* per lane shard, and a
  single durable decision; readers encountering an intent of an undecided
  txid treat the chunk as its previous committed generation. The vocabulary
  already exists — this is exactly what the transaction machinery is for.
  The cost is a 32-shard transaction per large write, so it is used where
  the semantics demand it, never on the default hot path (P1: the default
  path must not pay for a guarantee the workload does not use).

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

**Read authority is amortized, not per-request.** With no client-side TTL
caches, a metadata-intensive workload would otherwise pay one quorum round
per lookup. The leader batches: requests that arrive together are served by
**one** read-authority round, and every request whose required applied index
is covered by the established read index is answered from it. This weakens
nothing — the read is still quorum-backed and applied-index-gated — it just
removes redundant network synchronization, the read-side analog of §5.7's
publication batching.

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
hot path. Open references are tracked under the client's **session**
(§5.12a): a fenced (dead) client's references are safely retired by
recovery, and a stale session's reference can neither keep an inode alive
nor be reused (I19). Reclaim is L6.

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
- **Owner identity and death.** A lock owner is `(client_uuid,
  session_epoch, process_id)`. Locks are released on close/process exit as
  usual; a *dead client* is handled by the session protocol (§5.12a): once
  its session is fenced, the lock authority reclaims its locks. Blocked
  waiters are woken in order.
- **flock vs fcntl** are kept as separate namespaces (POSIX semantics), both
  on the same per-inode authority.

### 5.12a Client sessions and fencing

Several mechanisms — distributed locks, open-unlinked inode lifetime, append
reservation recovery, duplicate suppression — all depend on one magic
transition: "the client incarnation becomes stale." That transition is now a
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
  Shard leaders and data targets **fence on the epoch**: a request naming a
  session epoch older than the committed one is rejected as stale (P3 —
  stale work is harmless, never accepted as authoritative). Leaders cache
  the committed session table and re-validate against the session authority
  on doubt.
- **The failure detector and the authority are separate roles:** heartbeat
  loss / connection death only decides *when* a session replacement may be
  **attempted**; **consensus decides *which* session is authoritative** —
  the epoch bump is a Raft commit on the session shard. A timeout alone
  never changes anything.
- Once `session_epoch + 1` is committed, the old session is dead everywhere:
  its **POSIX locks** are reclaimed by the lock authorities, its
  **open-unlinked references** are dropped (the orphan inode becomes
  reclaimable, §5.9), its pending **append reservations** are resolved as
  committed zero holes (§5.7), and its **dedup records are retained** so its
  in-flight retries are still recognized as duplicates rather than
  re-executed (I9).
- A fenced client learns on its next operation (`FENCED`) and must
  re-establish a session (new epoch, re-open state) before doing further
  work.

**The honest trade-off:** a partitioned-but-alive client can be fenced —
that is an availability sacrifice, never a safety violation, and it is the
standard fencing-token pattern (the session epoch *is* the fencing token,
checked at every authority). The alternative — never fencing on suspicion —
leaves dead clients' locks and reservations held forever.

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
- **The FUSE layer must not re-serialize what efs just parallelized.**
  Direct-I/O alone is not enough: without `parallel_direct_writes`, Linux
  serializes direct writes on the same file *above* the efs client, and P1
  is violated before a request even reaches efs. These are architectural
  requirements, not tuning suggestions: `FOPEN_DIRECT_IO`,
  `FOPEN_PARALLEL_DIRECT_WRITES`, `FUSE_CAP_ASYNC_DIO`,
  `FUSE_CAP_PARALLEL_DIROPS`, large `max_write`/`max_pages`, and sufficient
  `max_background`. (`FUSE_CAP_ASYNC_DIO` is in deliberate tension with the
  atomic-publication unit — splitting can blur the syscall boundary; §5.7
  defines the atomic unit and the `max_write` sizing rule that resolves it.)
  FUSE-over-io_uring is evaluated as the interface matures.
- **Direct-I/O disables kernel readahead — so the client owns prefetch.**
  FUSE direct-I/O bypasses the page cache *and performs no read-ahead*, so a
  naive client turns sequential reads into a lockstep `read → wait → read`
  that can never drive enough queue depth to saturate the NIC/RDMA/NVMe. The
  efs client therefore runs an **asynchronous prefetch pipeline**: a read of
  chunk N issues N and prefetches N+1, N+2, … with adaptive queue depth. This
  is a performance design requirement of the direct-I/O decision, not an
  optional extra — it is what makes §7's "large sequential reads scale"
  row true.
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

**The bar, stated plainly: a *less advanced* AI model must be able to
contribute a correct change.** Not "the best available model, with the whole
tree in context, on a good day" — a modest one. That is a much stronger
requirement than "the code is organized," and it has concrete consequences:

- **Local correctness must be locally decidable.** Whether a change to module
  X is right must be answerable from X's source + X's interface header + X's
  tests — never from global reasoning about the whole system. If correctness
  requires holding the whole tree in your head, only the strongest
  contributors can play, and the project scales with *model quality* instead
  of with *contributor count*.
- **The blast radius of a mistake is one module.** A weak contributor's error
  must fail *fast and locally* — a unit test, a simulator assertion, an
  interface-contract check — not subtly, three subsystems away, in
  production. The safety net is executable (tests, invariants, the
  simulator), so the quality bar is enforced by the *boundaries*, not by the
  sophistication of whoever is editing.
- **Interfaces carry the contract.** Each module's header states what it
  guarantees and what it requires (invariants, ownership, threading rules) —
  so a contributor doesn't have to *derive* the contract from the rest of the
  tree before touching anything.

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
- **Conventions are machine-checkable.** Formatting, naming, and the
  interface/ownership rules are enforced by tooling (lint, the build, the
  test gate), not by reviewer vigilance — a less advanced contributor
  (human or model) cannot silently violate a rule the tooling would have
  caught.

**Why it is in the architecture doc and not a style guide:** the migration
(§10) lands Raft, KV, and the transaction protocol as *new* components. If
they are built to these boundaries from the start, the simulator and the unit
harness get them for free, and the dev cycle (§9a) stays fast as the system
grows. If they are built as another 6000-line `metadata.c`, no amount of
testing infrastructure will save the cycle time.

### 5.15 The hot-path implementation contract

The topology in §5.0–§5.14 is what makes linear scaling *possible*. This
section is the contract that makes it *actual* — the difference between "the
architecture scales" and "the implementation scales." Each item is a
requirement, traceable to P1/P4, not a tuning suggestion.

- **NUMA-local, queue-depth-driven execution (P4).** The data target and the
  metadata replica both run as asynchronous engines: a NIC RX queue is
  affinitized to a reactor core on its NUMA node, which drives a local NVMe
  queue pair; registered buffers are NUMA-local; ordinary PUT/GET and
  ordinary metadata commits take **no cross-core lock** and make no
  thread-to-thread handoff (no network-thread → queue → worker-thread →
  queue → storage-thread relay). Whether the engine is literally SPDK or an
  equivalent `io_uring`/O_DIRECT design is an implementation decision; the
  architectural requirement is the absence of cross-core coordination on the
  common path.
- **QoS isolation between planes.** "Run at hardware saturation" and
  "metadata stays responsive" are mutually hostile without isolation. Raft,
  membership, transaction decisions, and small metadata RPCs get their own
  RDMA queues / traffic class / credits, separate from bulk data — not
  static bandwidth reservation, but *latency* isolation, so a saturated
  400G NIC cannot queue a Raft packet behind gigabytes of EC fragments. On
  NVMe: metadata WAL, KV reads/writes, bulk fragment I/O, and rebuild I/O
  have separate queues and backpressure policies.
- **Rebuild is distributed and rate-limited (P1 applies to repair too).** A
  lost object's repair is deterministically assigned to an owner, reads its
  k surviving fragments from distributed peers, and writes the replacement —
  spread across the whole cluster, never centralized. Scheduling priority is
  **foreground I/O > metadata > rebuild**: rebuild consumes *unused*
  hardware bandwidth, so a node failure must not collapse foreground
  throughput. (This is also where degraded generations from §5.7 are
  re-striped.)
- **FUSE capabilities and client prefetch** are part of this contract —
  specified in §5.13 because they are also correctness-relevant.
- **Read-side metadata is fetched in windows, not per chunk.** A sequential
  read at 100 GB/s touches ~800k chunks/s; discovering each chunk's
  committed generation with an independent metadata RPC would make the
  metadata plane the ceiling even with a perfect data path. The lane
  structure makes the fix cheap: the client issues **batched range
  chunk-map fetches per lane** (lane i holds chunks i, i+L, i+2L, …, so one
  range request per lane covers a contiguous file window), and the prefetch
  pipeline runs a *metadata window* ahead of the *data window* — chunk maps
  arrive before the data they describe is needed. Per-chunk lookups remain
  only for true random I/O.
- **Batching** of Raft messages, WAL group commit, KV applies, chunk
  publications, and read-authority rounds is specified where it lives
  (§5.4, §5.7, §5.9) and is binding here: no physical NVMe sync may be
  paid per logical operation on any hot path.

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
| survive f nodes down, read *and* write | a write must commit without the dead nodes | single-coordinator 2PC, primary-backup without failover |
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
  failure that leaves a majority. With RF = 2f+1 that is exactly the
  configured f nodes. This is why "survive f nodes down" is a theorem here.
  The old design's root/snapshot
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
  + k+f EC immutable chunk generations
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
- **Configurable failure tolerance:** metadata RF = min(N, 2f+1) per shard +
  k+f EC on data survive the configured f with no data loss (§2 for the
  quorum arithmetic and the valid N/f combinations, §3 for the precise
  availability claim).
- **Immediate visibility:** ops are applied on the leader in log order and
  acknowledged only when committed; kernel caches are actively invalidated
  and file data bypasses the page cache (§5.13). There is no cache to go
  stale.
- **Scale with raw hardware — stated as a measurable bar, not a slogan.**
  "Raw hardware" must be **protection-adjusted**: protection consumes
  hardware, so for k+f EC a logical chunk of size C writes `(k+f)/k × C`
  physically (1.5× at 2+1). The ideal ceiling for logical write bandwidth is
  therefore approximately

  ```text
  min( aggregate client egress        × k/(k+f),
       aggregate storage-node ingress × k/(k+f),
       aggregate NVMe write BW        × k/(k+f),
       EC encode CPU / memory bandwidth,
       metadata publication rate × chunk size )
  ```

  plus smaller metadata/network overheads. The bar is a **high fraction of
  that protection-adjusted ceiling** (85–95% would be extraordinary), and the
  hot-path contract of §5.15 exists precisely to remove the software terms
  from that min(). The data path is client-direct RDMA + EC; the hot-file
  metadata path publishes on bounded per-file lanes rather than serializing
  on an inode row (§5.7). Metadata op cost is one leader RTT + a majority
  replication — the floor for any consistent system — and it no longer
  degrades with table size or with same-file concurrency.

**"Near-linear" is defined per workload class** — several POSIX operations
are serial *by semantics*, and the claim excludes them explicitly instead of
silently failing them:

| Workload | Expected scaling |
|---|---|
| Many files, large aligned I/O | **near-linear with nodes** |
| One file, disjoint full chunks (many writers) | **near-linear up to the file's lane count / cluster size** |
| One giant `write()` syscall | data movement parallel; one atomic visibility decision per syscall (§5.7) |
| Many independent directories | **near-linear** — every directory scatters at birth (§5.3) |
| One spread hot directory | **near-linear across its hash shards** |
| One normal small directory | single-shard until size/pressure spreads it |
| Large sequential reads | near-linear **if userspace prefetch keeps queue depth high** (§5.13) and chunk maps are fetched in per-lane windows (§5.15) |
| `O_APPEND` on one file | reservation order + visible commit frontier serialized by semantics; data movement parallel (§5.7) |
| Overlapping same-range writes | serialized by semantics (CAS retry) |
| `truncate` on one file | content-epoch serialization (§5.7) |
| Cross-directory rename storm | transaction-throughput limited (§5.6) |
| Same-file `fcntl` lock storm | inode lock-authority limited (§5.12) |
| 4K random updates in 128K chunks | RMW limited — declared envelope (§5.7) |

A scaling benchmark is then a scientific question — *where did it stop: NIC,
NVMe, memory bandwidth, EC compute, fabric bisection?* — and any answer that
is a mutex, one leader, one thread, FUSE serialization, one WAL, or one
coordinator is by definition an EFS bug (§1).

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
- **Hard-disk support.** EFS is flash/NVMe-only (§1 hardware envelope). No
  seek-aware layouts, no rotational-latency hiding, no SMR handling — the
  design spends the assumptions flash makes true (cheap random I/O, deep
  hardware queues, µs-scale device latency) instead of defending against
  disk.

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
   the bounded write-lane placement (§5.7).
1. Simulator interfaces + the CURRENT state machine.
2. Define RPC operation IDs + the idempotency model.
3. Ordered KV applied state, incl. atomic batch semantics.
4. Single-shard Raft: persistence · election · replication · apply ·
   ReadIndex/linearizable reads · snapshots.
5. Simulator proves the single-shard invariants.
6. Safe Raft-group reconfiguration + control-plane desired placement.
7. Cross-shard transaction protocol, incl. concurrency control (§5.6).
8. Client sessions + fencing (§5.12a), then the open-unlinked inode
   lifecycle (§5.11) on top of them.
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
- **Raft group** — the RF replicas + log for one shard (RF = 2f+1 at the
  configured failure target).
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
