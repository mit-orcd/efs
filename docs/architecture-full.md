# efs architecture — full one-file rendition

**GENERATED FILE — do not edit.** Built by `docs/gen-architecture-full.py`
from `architecture.md` plus every satellite under `arch/`. Regenerate after
any doc edit:

```bash
python3 docs/gen-architecture-full.py
```

This is the **complete** architecture content in one paste: the normative
index first (it wins any disagreement), then each satellite verbatim as an
appendix. Relative links in the index (e.g. `arch/protocols/data.md`) refer
to the appendices below.

---

# Architecture

**New here, or looking for the next task? → [START-HERE](arch/START-HERE.md)**
(what to work on now, which pages govern a given change, what "done" means).

[Plain-language rendition](architecture.html) ·
[One-file full version](architecture-full.md) ·
[Design rationale](arch/design.md) ·
[Failure tolerance](arch/failure-tolerance.md) ·
[Protocols: transactions](arch/protocols/transactions.md) ·
[data](arch/protocols/data.md) ·
[directories](arch/protocols/directory.md) ·
[sessions](arch/protocols/sessions.md) ·
[Performance](arch/performance.md) ·
[Development](arch/development.md) ·
[Verification](arch/verification.md) ·
[Design history](arch/design-history.md) ·
[Naming](arch/naming.md) ·
[Scaling roadmap](scaling-roadmap.md)

This is the **normative specification** — the index of architectural truth.
It states the goal, the failure model, the consistency contract, the
invariants, where every piece of state lives, which protocol governs each
operation, and the performance contract. Rationale, derivations, and full
protocol detail live in the linked satellites; **if this file and a
satellite disagree, this file wins.** The [scaling
roadmap](scaling-roadmap.md) is the *increment plan* for the current
implementation; this is the *specification*. When a design question comes
up, the answer is decided here first, then reflected in the roadmap.

**Status: ratified Sep 1 2026; revised Sep 1 2026 after external protocol
review (seven rounds — see [design history](arch/design-history.md)).** The
metadata layer described here replaces the current whole-table snapshot +
2PC design. The data path is unchanged in mechanism (client-direct RDMA,
k+f EC) but its commit semantics are specified precisely (§7.3). The bar
for the design is that it earns the *idea* of an "extreme filesystem": it
scales as close as possible to the raw hardware — a component is wrong if
it serializes work the hardware could have done in parallel
([naming](arch/naming.md)).

---

## 0. Governing principles

Four principles decide every hard case below. When a design question has no
obvious answer, the answer is whichever option these four point to.

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
  extending-write problem (§7.3): chunk publication and its size lane share a
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
  (§8).

---

## 1. Goal

A high-performance parallel POSIX file system.

| Property | Target |
|---|---|
| Objects (files + dirs) | ≥ 2³² (4.29 billion) **live** |
| Cluster size | 3 nodes (smallest) to 64 nodes |
| Reference size | 2³² objects on 4 nodes |
| Failure tolerance | **configurable f = 1…3** simultaneous node losses (§2); no data loss at the configured f; requires N ≥ max(2f+1, k+f) |
| Consistency | immediate cross-client visibility (no TTL caches) |
| Data path | client-direct, RDMA, k+f EC (k=2 default) |
| **Scaling bar** | throughput scales with the **protection-adjusted** aggregate hardware ceiling (§9); no software serialization point may become the limiter before a physical resource does |
| **Hardware envelope** | **modern flash only** — NVMe SSDs + RDMA-capable NICs. Hard disks are explicitly out of scope (below) |

**Hardware envelope: flash/NVMe-only, by decision.** EFS does not support
spinning disks, and no design effort is spent on them. This is not an
omission — it is a scoping decision that the architecture actively spends:

- **Random I/O is first-class.** There is no seek penalty to hide, so no
  seek-aware layouts, no write-sequencing-for-disks, no elevator thinking.
  The KV pager's random page faults and the read path's random chunk reads
  are *cheap* by assumption.
- **Deep hardware queues are assumed.** The P4 execution model (§8) —
  asynchronous, queue-depth-driven, one NVMe queue pair per reactor — only
  exists because the devices do. A HDD cannot run that model at all.
- **Microsecond device latency is what the durability choice costs against.**
  `write()` = durable (§7.3) pays a synchronous round trip to *device*
  per write; that is affordable because the device is µs-scale flash, not
  ms-scale disk.
- **No SMR/shingled, no rotational-latency hiding, no track alignment** —
  entire problem classes deleted, not engineered around.

If a deployment needs HDDs, that is a different filesystem; efs's scaling
claims (§9) are made against flash and do not transfer.

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
workload class in §9 — several POSIX operations are serial *by semantics* and
are excluded from the claim rather than silently failing it.

## 2. Failure guarantees

**Assumptions.**

- Nodes fail by crashing (stop, restart, rejoin). No Byzantine behavior.
  **"Losing a node" means its storage is gone**, not just the process — the
  guarantees below are for permanent loss; a crash-with-intact-disk is a
  strictly easier case covered by the same configuration.
- The network can drop, delay, reorder, duplicate, and partition messages.
  Partitions heal.
- Clocks are loosely synchronized (NTP). Used for expiry/TTL and cache hints
  only — **never** for correctness of replication, fencing, or reads. In
  particular there are **no clock-based leader leases** on the authoritative
  read path.
- A node that loses its local storage rejoins empty and rebuilds from peers.
- Clients are **fail-stop / non-Byzantine**: they may crash, disconnect,
  retry, duplicate, or hold stale state, but they are not adversarial.
  Authorization/fencing at storage targets is therefore about *stale*
  clients, not malicious ones (§7.3). If efs is ever exposed to hostile
  clients, capability-based data authorization becomes a requirement, not an
  option.

**The rule.** The cluster is configured with a failure target **f** — the
maximum number of simultaneous permanent node losses it must survive
**without data loss** — capped at **f = 3** and bounded by cluster size.
(The cap is deliberate: metadata quorum size and EC storage overhead both
grow with f, and f=3 already covers the realistic correlated-failure
envelope for a single rack/cluster.) The two planes scale differently with
f: a `k+f` EC stripe survives any f data losses and needs N ≥ k+f distinct
failure domains; a Raft group stays authoritative through f permanent
losses only if the survivors still form a quorum, which needs
**N ≥ 2f+1**. So the configured-f guarantee is one line:

```text
N ≥ max(2f+1, k+f)        RF = 2f+1 voting replicas per shard
```

Lose any f nodes: a metadata majority survives, ≥k data fragments survive,
and the system remains authoritative **automatically** — no quorum override,
no disaster mode, no distinction between "bits survive" and "we can prove
which bits were committed."

| N nodes | max f | metadata RF | data EC | guarantee |
|---|---|---|---|---|
| 3 | 1 | 3 | 2+1 | lose 1: no loss, stays authoritative |
| 4 | 1 | 3 | 2+1 | lose 1: no loss, stays authoritative |
| 5 | 2 | 5 | 2+2 | lose 2: no loss, stays authoritative |
| 6 | 2 | 5 | 2+2 | lose 2: no loss, stays authoritative |
| ≥ 7 | 3 | 7 | 2+3 (or wider k, e.g. 4+3 = 1.75×) | lose 3: no loss, stays authoritative |

**f=3 needs 7 nodes, not 5 or 6.** On 5 nodes the best metadata
configuration is RF=5, Q=3, and 3 ≯ 3 — the 3 dead nodes could be exactly
the quorum that acknowledged the most recent writes. The config validator
rejects any configuration that violates N ≥ max(2f+1, k+f).

**There is no N = 2f mode.** After a permanent majority loss, no local rule
over surviving logs can reconstruct the committed prefix — commitment
information died with the majority, and splicing survivor logs can
manufacture a command sequence no leader ever authorized. Permanent majority
loss is disaster territory (an operator procedure over surviving state), not
an efs operating mode. (Full argument:
[failure-tolerance.md](arch/failure-tolerance.md).)

**Changing f or k is an online control-plane operation** with an explicit
`effective_f` → `target_f` transition state: add shard replicas (joint
consensus, §7.8), re-stripe every protected data generation to the new k+f
width as **new immutable generations under the new coding profile** (§7.3),
verify, and only then commit the new guarantee. Derivation and transition
detail: [failure-tolerance.md](arch/failure-tolerance.md).

## 3. Consistency contract

- **Single-shard metadata operations are linearizable** within the
  authoritative shard's Raft group.
- **Multi-shard metadata operations are atomic** across their participant
  shards according to the cross-shard transaction protocol (§7.2). The
  intended property for those is strict serializability of transactions.
- There is **no global ordering** between two independent operations on
  unrelated shards, and none is needed.
- **Data:** a `write()` that has returned success is durable and visible (see
  the precise commit state machine in §7.3). Un-`fsync`ed data can be lost on
  client crash only where POSIX permits it.
- **One `write()`/`pwrite()` publishes atomically.** POSIX makes regular-file
  `read()`/`write()` effects atomic with respect to one another, so a
  concurrent reader never observes a mix of old and new chunks from a single
  in-flight write: publication is one atomic decision covering every chunk
  the call touched (§7.3), while the *data movement* stays fully parallel.
  The atomicity unit is **one FUSE write request**; `max_write` is sized so
  the writes that matter arrive as one request. Full syscall-level atomicity
  *above* that boundary is an open kernel-interface problem — FUSE
  async-DIO can split a syscall without identifying it — and EFS **does not
  claim** it until solved (§7.3 states the unresolved contract and the
  investigation paths). Once a call has returned, every subsequent read
  sees all of it. An explicit **relaxed mode** (mount option, for
  applications that synchronize themselves — MPI-IO-style) publishes chunk
  by chunk for maximum publication throughput; it is documented as weaker
  than POSIX and is never the default.

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
feature needs snapshot readdir, that is an MVCC read (rejected for now —
[design.md](arch/design.md)) introduced deliberately, not an accident of the
scan.

**CAP-accurate availability.** At the configured failure target f (§2,
which requires N ≥ 2f+1): any f nodes may be permanently lost with **no
data loss**, and every shard retains a quorum after the losses, so
operations remain available to clients that can reach it — no operator
action, ever. A minority partition never serves authoritative writes or
stale authoritative reads. Brief unavailability during leader election is
expected. "Survive f nodes" means safety always, plus continued operation
whenever a quorum survives — not literally zero interruption for every
client.

## 4. Invariants

These are the test oracle for the simulator
([verification.md](arch/verification.md)) and for `fsck`. A bug is a
violation of one of these. Invariants are stated over the **committed,
externally-visible projection** of state; cross-shard transaction *intents*
(§7.2) are internal and are governed by I17/I18, not by the naïve forms of
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
  that is genuinely absent; every resource, availability, or integrity
  failure surfaces as a distinct retryable error instead. **This is a safety
  invariant, not an error-reporting nicety:** a false "absent" answer to a
  LOOKUP lets the next CREATE mint a *second* inode for an object that is
  still live, which violates I5, I6, and I7 and leaves no evidence that it
  happened. A read path that can fail is therefore required to fail
  *loudly*, never by answering "no".
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
  repaired when capacity returns (§7.3).
- **I12 · chunk consistency.** Every returned chunk corresponds to one valid
  committed generation and a legal serialization of the committed byte-range
  mutations.
- **I13 · generation coherence.** A reconstruction never mixes fragments from
  different generations. Generation identifiers are globally unique candidate
  identities scoped to `FileID = (ino, inode_generation)`, so no two writers
  and no two incarnations of an ino can ever produce different bytes under
  one identity (§7.3).
- **I14 · publish-after-durability.** Metadata cannot publish generation G
  before G satisfies the EC durability requirement.
- **I15 · orphan-generation safety.** Fragments from a generation that was
  never published may be reclaimed without changing any committed state.
- **I20 · stale-writer fencing.** A stale client/node cannot overwrite or
  publish data over a newer committed chunk generation.
- **I21 · publish+size atomicity.** An extending write never commits its
  chunk publication without the corresponding size-lane update, nor the
  reverse — they are one Raft entry on one shard (§7.3). A reader never sees
  a size that covers unpublished bytes, nor published bytes that no committed
  size exposes.
- **I22 · epoch fencing.** A publication naming a superseded content epoch is
  never committed, and size/time lane state from a superseded epoch is never
  read. The epoch fences *in-flight work and lane state* — it does **not**
  invalidate committed chunk data, and it is not part of a fragment object's
  identity: a `truncate()` to a smaller non-zero size preserves the surviving
  prefix, as POSIX requires (§7.3).
- **I25 · end-to-end integrity.** Every durable fragment is protected by a
  checksum over its immutable identity plus its payload. A fragment that
  fails verification is never used as reconstruction input; it is treated as
  unavailable and repaired (§7.3).
- **I23 · session fencing.** A fenced (superseded) client session cannot
  perform any **authoritative mutation**: it cannot commit a metadata
  operation, publish a data generation, acquire or retain a lock, an open
  reference, or an append reservation, or make stale work visible. Metadata
  leaders enforce the epoch; data targets need not. Fencing is a **barrier,
  not an announcement**: no consequence of the old session's death (lock
  reclaim, lease drop, reservation resolution) may take effect until every
  shard that could act on the old epoch has durably acknowledged the fence
  (§7.5).
- **I24 · atomic write publication.** Within the atomicity unit (one FUSE
  write request, ≤ `max_write`; the syscall-boundary problem above that
  size is explicitly open, §7.3): a returned `write()`/`pwrite()` is
  visible in its entirety to every subsequent read, and a concurrent read
  during the call sees either all or none of that call's chunks (default
  mode; §7.3).

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

## 5. State placement

**Three planes, not two.**

- **Data plane** — chunk PUT/GET, k+f EC (§2, §7.3), client-direct over RDMA.
- **Metadata plane** — virtual shards, one Raft group per shard at
  RF = 2f+1 (§2, §7.1), ordered applied KV on NVMe.
- **Control plane** — cluster membership, desired placement, shard
  reconfiguration, node incarnation, drain/rebuild. Its own Raft group
  (§7.8), and *not* an implicit authority for operations it does not itself
  make safe.

**Identity and addressing.**

- **`efs_ino_t` is 64-bit.** The target is ≥ 2³² *live* objects, so the ID
  space must exceed that to cover deleted/recreated objects, avoid dangerous
  immediate reuse, tolerate stale client handles, and leave headroom.
- **Inode generation / incarnation.** Each inode carries a generation that
  changes on reuse, so a stale handle for a deleted `ino` never silently
  becomes a handle for a new object (ABA protection). A handle is
  `(ino, generation)`.
- **`FileID = (ino, inode_generation)` scopes every persistent data-plane
  identity** — fragment objects, chunk-map entries, write lanes, append
  reservations, publication intents. A bare `ino` is *not* sufficient there:
  a fragment PUT issued by a delayed writer for the previous occupant of
  ino 100 would otherwise land inside the new object's key space. ABA
  protection has to cover the data plane, not just handles (§7.3).
- **Allocation.** Inos are allocated in per-shard (or per-allocation-domain)
  ranges so allocation is shard-local and globally unique by construction.
  The exact scheme (range handout vs. `[domain | counter]`) is an
  implementation detail; the invariant is global uniqueness + ABA protection.
  **Allocation is also the mechanism behind CREATE co-location (§7.4).**
  Placing a new file's inode row on its parent directory's shard is not a
  second, competing placement rule — it is a constraint on *which range the
  ino is drawn from*: the allocator picks an ino whose `ino & 0xFFF` already
  equals the parent's shard. `inode_shard()` therefore stays a pure function
  of the ino, and no operation ever needs an exception to it.

**Sharding.** **4096 metadata shards**: `shard = ino & 0xFFF`
(`shard_bits = 12`). Low-bit masking yields **interleaved buckets**, not
contiguous ranges — sequentially-allocated inos then balance across shards
naturally. A shard is ≈ 1M inodes at the 2³² target; shards are cheap and a
node hosts many.

**Where every piece of state lives (authoritative).**

| State | Authority | Key | Replication |
|---|---|---|---|
| inode row | `inode_shard(ino) = ino & 0xFFF` | `(ino)` | shard Raft group (RF = 2f+1) |
| dentry, LOCAL layout | parent's shard | `(parent_ino, name)` → `(ino, gen, type)` | shard Raft |
| dentry, HASHED layout | `hash(parent_ino, name) & 0xFFF` | `(parent_ino, name)` → `(ino, gen, type)` | shard Raft |
| chunk map | lane shard (§7.3 permutation) | `(FileID, lane, chunk_index)` | shard Raft |
| size / write mtime / write ctime | write lane, co-located with the chunk's lane shard | `(FileID, lane)` | shard Raft |
| active-lane bitmap, `base_size`/`base_mtime`/`base_ctime`, `content_epoch` | `inode_shard(ino)` (inode row) | `(ino)` | shard Raft |
| directory mtime / ctime, HASHED layout | each of the directory's dentry hash shards | `(dir_ino, shard)` | shard Raft |
| client session, touched-shard set | `hash(client_uuid) & 0xFFF` | `(client_uuid)` | shard Raft |
| open lease | `inode_shard(ino)` | `(ino, session_id)` | shard Raft |
| POSIX locks | `inode_shard(ino)` | `(ino, start, end, owner)` | shard Raft |
| transaction decision | `participant[hash(txid) % participant_count]` | `(txid)` | shard Raft |
| membership / desired placement | control-plane group | — | control Raft (RF = 2f+1) |
| file data | k+f distinct failure domains by placement | `(FileID, chunk_index, candidate_generation, fragment_index, coding_profile_id)` | k+f EC fragments |

**Applied state: a logical ordered KV per shard.** Each replica applies its
log to a **local, embedded, ordered key-value store** on NVMe. RAM is a
bounded cache, not the store. Each shard has an isolated ordered
applied-state *namespace*; the implementation multiplexes many shard
namespaces into one local storage engine via a shard-prefix key (at 64 nodes
a node hosts ~192 shard replicas at RF=3, ~448 at RF=7 — while *leaders*
stay ~64/node at any RF, which is what matters for write scheduling).
Ordered keys make readdir a range scan. A log + KV is the durability
mechanism; snapshots exist only for **log truncation**, never as the
durability mechanism. **Capacity model (honest):** real persistent
consumption is `(N_inodes × inode_value_size + N_dentries ×
dentry_value_size + names) × KV_amplification × RF`, plus Raft log,
snapshots, tombstones, transaction and dedup state, and compaction headroom
— with `N_dentries ≥ N_inodes` (hardlinks). NVMe capacity is designed
around this explicitly; it is not a "2³² × 128 B fits" claim.

**A cache miss is never absence.** Because RAM is a bounded cache over the
KV rather than the store itself, every miss must be resolvable from local
applied state. When it cannot be — I/O error, corruption, or an evicted
entry whose backing is gone — that is a **resource** failure and must
surface as a distinct retryable error (I9). This is a real failure mode
rather than a hypothetical one: a bounded cache whose miss path is allowed
to fail silently is precisely how a live row comes to read as a missing
object, and the read path is the one place where that mistake is
unrecoverable (see I9 for why).

## 6. Operation → participant matrix (authoritative)

Derived from the placement rules (§5) and the CREATE co-location rule
(§7.4). This matrix is the authority on which shards an operation touches.

| Op | State touched | Shard(s) | Protocol |
|---|---|---|---|
| CREATE (file) | dentry + inode row | **1** (co-located) | single Raft entry |
| MKDIR | dentry + parent `nlink` on parent shard + dir inode on its home shard | 2 | transaction (§7.2) |
| RMDIR (LOCAL dir) | dentry + parent `nlink` + dir inode row; emptiness is one range check | 2 | transaction (§7.2) |
| RMDIR (HASHED dir) | as above **+ emptiness across the directory's dentry shards** | 2 + used hash shards | transaction with the emptiness check in its read set (§7.4) |
| UNLINK (last link) | dentry + inode row | **1** if `dentry_shard == inode_shard` (and nlink==1), else 2 | single Raft entry / transaction |
| UNLINK (nlink>1) | dentry on parent shard, nlink on inode shard | 2 | transaction (§7.2) |
| LOOKUP (name only) | dentry | 1 | single read |
| GETATTR (stat) | inode row **+ the file's active write lanes** (size/mtime/ctime merge, §7.3) | 1 + active lanes (1 for a small file, ≤64) | double-collect (§7.3) |
| GETATTR (stat) on a HASHED directory | dir inode row + the directory's used dentry shards (mtime/ctime merge, §7.4) | 1 + used hash shards | double-collect (§7.4) |
| SETATTR | inode row | 1 | single Raft entry |
| LINK (hardlink) | new dentry on its parent shard, nlink on inode shard | 2 | transaction (§7.2) |
| RENAME same-dir (LOCAL dir) | two dentries on parent shard | **1** in the simple no-replacement case; replacing a destination whose inode lives on another shard widens the participant set | single Raft entry / transaction (§7.2) |
| RENAME same-dir (HASHED dir) | two dentries on `hash(parent, old)` and `hash(parent, new)` — independent shards | ≥2 | transaction (§7.2) |
| RENAME cross-dir (file) | src dentry, dst dentry, ino row, parent nlinks | ≥2 | transaction (§7.2) |
| RENAME cross-dir (directory) | as above **+ the destination's whole ancestry chain as a versioned read set** (cycle prevention, §7.4) | ≥2 + O(depth) | transaction (§7.2) |
| WRITE non-extending | chunk-map entry | 1 (lane shard) | single Raft entry |
| WRITE extending | chunk-map entry + that lane's size/mtime/ctime marks | **1** (co-located, §7.3) | single Raft entry |
| WRITE first use of a lane | as above + `active_lanes` bit on the inode row | 2 | transaction (§7.2); ≤64 times per file, ever |
| O_APPEND | EOF reserve + chunk + size | inode shard + lane shards | reserve (§7.3) then write |
| TRUNCATE | `content_epoch` + `base_size` + times on inode row | 1 | single Raft entry (§7.3) |
| READDIR | dentries | 1 (normal) / many (spread) | range scan / scatter-merge |
| CHMOD / CHOWN | inode row (ctime, **not** mtime) | 1 | single Raft entry |

(The UNLINK-last-link row is conditional because of a hardlink corner case:
after `create /a/foo; link /a/foo /b/foo; unlink /a/foo`, the surviving dentry
is on `/b`'s shard while the inode row is on `/a`'s — the final unlink of
`/b/foo` is then a two-shard transaction.)

## 7. Protocol summaries

Each protocol is stated here in binding summary; the linked document is its
full specification.

### 7.1 Sharded Raft and linearizable reads

Each shard is an independent Raft group at **RF = 2f+1** (§2), replicas on
distinct failure domains. Each group elects its own leader; leaders spread
across nodes, so metadata write throughput scales with node count — there is
no single metadata primary. Every mutation is a log entry `(term, index)`,
committed when a majority of the group's replicas hold it durably; this is
what makes I1–I4 and I10 true. The 4096 logical groups run on a **multi-Raft
runtime** (reactors per NUMA domain, messages batched by destination,
heartbeats coalesced, WAL group-committed across groups, KV applies batched)
— an architectural requirement, not an optimization
([performance.md](arch/performance.md)).

Authoritative reads (LOOKUP/GETATTR/readdir) go to the **shard leader**,
which establishes quorum-backed read authority (ReadIndex-style) and waits
until its applied index covers the read before reading the KV. There are
**no clock-based leader leases** (clocks are never correctness inputs, §2).
Read authority is **amortized**: one quorum round serves a whole batch of
requests — the read-side analog of publication batching (§7.3).

Invariants: I1–I4, I9, I10, I18.

### 7.2 Cross-shard transactions

EFS uses conditional prepared intents plus one durable distributed decision.
Intents are never externally visible until the decision commits (I17).
Prepares are **conditional** (`prepare(T, key, expected_version)`) and
**no-wait**: a conflicting prepare fails immediately, the loser aborts its
intents and retries with randomized backoff — nobody ever waits while
holding an intent, so deadlock is impossible by construction. **Independent
prepares run in parallel**; write-publication transactions (hot path, §7.3)
always prepare in parallel, while rare namespace transactions prepare in
canonical key order to minimize retry churn. The coordinator is dispersed —
`participant[hash(txid) % participant_count]` — so same-file write
transactions never funnel into one shard.

**Only exclusive keys take intents.** A transaction's effects are either
**exclusive/CAS keys** (chunk-map entry, dentry, inode row — one holder,
version-checked) or **commutative reductions** (`MAX` on a lane's size and
times, or a hashed directory's times). Reductions are transaction *payload*,
applied by the reducer at resolve time, and never block a prepare —
otherwise two writers publishing different chunks that share a lane would
conflict on nothing, and the per-file hotspot the lanes exist to remove
would reappear one level down (P1).

A transaction is visible **at its decision**, not when cleanup runs; a
reader meeting an intent resolves COMMIT → new value, ABORT → old value,
NO-DECISION → old value (the read linearizes before the eventual decision),
and **cannot-establish-authority → a retryable error, never "absent"** (I9).
Recovery drives every prepared transaction to COMMIT or ABORT (L5); resolved
intents are GC'd, while decision records are retained until every
participant has acknowledged them.

Invariants: I16, I17; the namespace transactions it carries (MKDIR/RMDIR,
LINK, cross-shard UNLINK and RENAME — §6) additionally preserve I5–I7.
Full protocol: [protocols/transactions.md](arch/protocols/transactions.md)

### 7.3 Data publication

A write commits through: allocate chunk generation → encode + store k+f
fragments (client-direct RDMA) → **ALL k+f durable fragment ACKs** →
Raft-commit the publication (chunk map + lane marks) → apply → return.
**"Durable ACK" = the target completed the persistent-NVMe operation**
(flush/FUA or PLP media), not an RDMA completion. A returned `write()` is
durable and visible — stronger than POSIX, a deliberate latency-for-
durability trade. Degraded publication (with `u` domains already
unavailable) needs ≥ `k+(f−u)` fragments, is marked degraded, and is
re-striped on repair — never for a merely *slow* target (I11).

**Identity.** Every data-plane key is scoped by
`FileID = (ino, inode_generation)`, so a delayed PUT from a previous occupant
of an ino cannot enter the new object's key space. A generation identifier is
a **globally unique candidate identity, never `G+1`**: successor numbering
would let two concurrent writers PUT different bytes under one object name.
Ordering comes from the publication `CAS(expected = committed base)`; the
loser's object is an orphan it retries over (P3, I13).

**Lanes.** Chunk metadata is sharded into **64 fixed per-file write lanes**,
activated on use rather than sized in advance — a runtime-derived lane count
would relocate every chunk-map key when it changed.

```text
lane(ci)                 = chunk_index % 64
lane_shard(FileID, lane) = (inode_shard(ino) + lane * stride(ino)) & 0xFFF
stride(ino)              = 2 * (hash(ino) & 0x7FF) + 1        (odd)
```

An odd stride over a power-of-two shard count is a permutation, so the lanes
are **64 distinct shards** (independent hashing would collide); **lane 0 is
the inode's own shard**, so a small file has no fan-out. A monotonic 64-bit
`active_lanes` bitmap on the inode row is set on a lane's first use — ≤64
inode interactions per file *lifetime* — and `stat()` collects only active
lanes, as a **double collect** over per-lane sequence numbers (bounded
retries, read-only transaction fallback).

An extending write is **one Raft entry on the lane shard**:
`{publish chunk (CAS); MAX(lane.max_end); MAX(lane.max_mtime);
MAX(lane.max_ctime)}` (I21).

**Times.** Write-generated mtime *and ctime* both live in the lanes; routing
write ctime to the inode row would send every hot-file writer back to the
inode leader. Explicit changes update the inode row's `base_*` under
`mtime_gen`, keeping POSIX's distinction exactly (`chmod` sets ctime, not
mtime). **atime is `noatime` by default**, `relatime` a coalesced mount
option; strict per-read atime is deliberately not offered.

**Truncate** bumps the content epoch and stamps the inode's `base_size`: it
invalidates lane state and rejects in-flight publications of the old epoch,
but **does not invalidate committed chunk data** — the surviving prefix stays
readable, as POSIX requires (I22). **O_APPEND** serializes only the EOF
reservation and the visible frontier (which advances over contiguous
*resolved* reservations; a fenced appender's range resolves as a committed
zero hole); data movement is parallel. **Sub-chunk writes** use generation
CAS; disjoint ranges both land (I12), and pay RMW amplification by design.

**One write syscall publishes atomically** via a §7.2 transaction (I24). The
atomicity unit is **one FUSE write request**, and **EFS MUST NOT claim full
POSIX write atomicity above the `max_write` boundary** until FUSE async-DIO
syscall splitting is solved — an explicit unresolved contract. A documented
relaxed mount mode publishes per-chunk (§3).

**Data targets are dumb — so publication is the validator.** A fragment PUT
is immutable and idempotent; the target verifies only placement epoch, target
incarnation and object identity, and does not check sessions (§7.5). Since
nothing on the data path enforces the coding/placement contract, the
publication entry carries its **durability evidence** (`coding_profile_id`,
`placement_epoch`, per-fragment ACK set with target incarnations and
checksums) and the lane leader **rejects evidence that does not match the
current profile, the current placement, all k+f fragment roles, and the
required distinct failure domains** — a stale client can durably write a full
stripe to an obsolete placement and still not publish it. The metadata
authority alone decides which generation is visible (I15, I20).

**Every durable fragment carries a checksum over its identity plus payload**
(I25). Media corruption is not Byzantine, and EC without integrity checking
reconstructs confidently from a corrupt fragment; a fragment failing
verification counts as unavailable and is repaired, never decoded.

Invariants: I11–I15, I20–I22, I24, I25.
Full protocol: [protocols/data.md](arch/protocols/data.md)

### 7.4 Directory placement and spreading

A dentry lives on its **parent's shard** as a small projection
`(ino, gen, type)`; the inode row is the only mutable copy of anything else.
**CREATE co-location:** a file's inode row lands on its directory's shard
(file CREATE = dentry + inode row in **one Raft entry**); **every MKDIR
scatters** — `inode_shard(new_dir) = hash(parent, name, export_salt) &
0xFFF` — paying one 2-shard transaction to buy an independently scalable
subtree for the directory's lifetime. `EFS_DIR_SPREAD_MIN` is a scalability
bound on the co-located file set, not just a space threshold.

A directory spreads (one-way) on **entry count OR op pressure**, through a
layout-epoch protocol `LOCAL → SPLITTING(e) → HASHED(e)`: during SPLITTING,
writes go only to the hashed location, reads check hashed-then-local, the
migrator moves idempotently, and **mutations of unmigrated entries write a
dominating hashed tombstone** — the hashed side always wins, so nothing
resurrects (I8).

**Directory timestamps spread with the directory.** POSIX updates a
directory's mtime/ctime on every entry create/remove/rename; sending those to
its home shard would funnel every create in a spread directory through one
leader and silently defeat the spread. A HASHED directory therefore keeps a
`dir_lane` (`max_mtime`, `max_ctime`) **on each of its dentry shards** — one
Raft entry per mutation, parent uninvolved — reduced by `stat()` under the
same double-collect protocol as file stat.

**Two operations are not uniformly cheap once a directory is HASHED.**
`rmdir` must prove emptiness, which is distributed: the emptiness check over
the dentry shards is part of the transaction's **read set**, so a concurrent
create invalidates it (an exact distributed entry counter is rejected — it
rebuilds the hotspot the spread removed). Same-directory `rename` is a
two-shard transaction, since `hash(parent, old)` and `hash(parent, new)` are
independent.

**Directory rename carries an ancestry read set.** POSIX forbids moving a
directory beneath itself, and checking that by walking the tree is unsound:
two concurrent renames can each validate a legal tree and together create an
unreachable cycle. Every directory row carries `parent_dir` +
`parent_version`; a directory rename puts the destination's whole ancestry
chain at those versions **into its conditional PREPARE**, so any concurrent
reparent in the chain aborts it. O(depth) and rare.

Invariants: I5–I8.
Full protocol: [protocols/directory.md](arch/protocols/directory.md)

### 7.5 Client sessions and fencing

Every client operates under a **session** `{client_uuid, session_epoch,
state}`, committed via Raft on the session shard `hash(client_uuid) &
0xFFF`. Every mutating request carries `(client_uuid, session_epoch,
op_id)`; **shard leaders reject a superseded epoch** (I23). Heartbeat loss
only decides *when* a replacement may be attempted; **consensus decides
which session is authoritative**.

**Revocation is a barrier, not an announcement.** Committing `epoch+1` fences
nothing by itself: a shard that has not heard of it, acting on its own view of
E, can still commit for the dead client. The session record therefore carries
the **set of shards registered to act on it** (a 4096-bit bitmap; registration
is once per shard per session, never on the I/O path), and the transition is
three-phase:

```text
ACTIVE(E) -> FENCING(E+1)  [freeze the touched set; no new registrations]
          -> FENCE(E+1) to every touched shard; each durably rejects E,
             resolves its undecided work, and ACKs
          -> ACTIVE(E+1)   [only after every ACK]
```

Nothing that depends on the old session being dead — lock reclaim, open-lease
drop, append-reservation resolution — happens before that last commit. An
unreachable shard makes the *new* session wait: the correct availability-for-
safety trade, and not a real loss, since a shard that cannot establish
authority cannot commit for the old client either. Dedup records survive the
bump (I16), reclaimed on a bounded window rather than kept per operation.

**Data targets do not fence sessions** — an orphan fragment PUT is harmless
because publication is exclusively metadata-controlled (P3). Fencing a
partitioned-but-alive client is an availability sacrifice, never a safety
one; the session epoch is the fencing token.

Invariants: I23, I16.
Full protocol: [protocols/sessions.md](arch/protocols/sessions.md)

### 7.6 Open-unlinked files and POSIX locking

Open lifetime is **one session-scoped open lease per (session, inode)** on
the inode shard: committed at the session's first open of the inode, removed
at its last close, lazily dropped when the session is fenced. Reclaim
requires `nlink == 0 AND open_sessions == empty` (I19, L6). Cost: one Raft
entry at each end of a session's use of a file — not per descriptor, not
per I/O.

Locks: **one lock authority per inode** on `inode_shard(ino)`, records
`(ino, start, end, owner)` mutated via Raft — a semantic serialization (P1),
accepted because a locking application asked for coordination. Owners are
`(client_uuid, session_epoch, process_id)`; fenced sessions' locks are
reclaimed. efs reproduces the **Linux** contract: classic `fcntl`,
**OFD `fcntl`** (POSIX.1-2024, included), and `flock` are three
non-interacting namespaces on the same authority (`flock` is not POSIX; the
non-interaction is Linux behavior). Advisory locks never fence the data
path.

Full protocol: [protocols/sessions.md](arch/protocols/sessions.md)

### 7.7 FUSE / kernel interface contract

Kernel metadata caches: `entry_timeout = negative_timeout = attr_timeout =
0` plus explicit invalidation through the low-level (inode-based) FUSE API.
**File data: direct-I/O by decision** — the kernel page cache is bypassed,
so cross-client data visibility is exactly as strong as metadata visibility,
with no coherence protocol to get wrong. Coherent caching may return later
only if it materially pays and only after zero/stale-cache semantics are
demonstrably correct without it. **Cross-client coherent `MAP_SHARED` is
unsupported initially** (documented limitation; `MAP_PRIVATE` unaffected).

The FUSE layer must not re-serialize what efs parallelized — requirements,
not tuning: `FOPEN_DIRECT_IO`, `FOPEN_PARALLEL_DIRECT_WRITES`,
`FUSE_CAP_ASYNC_DIO`, `FUSE_CAP_PARALLEL_DIROPS`, large
`max_write`/`max_pages`, sufficient `max_background`. `FUSE_CAP_ASYNC_DIO`
is in deliberate tension with write atomicity — see the unresolved contract
in §7.3. Direct-I/O disables kernel readahead, so **the client owns an
adaptive asynchronous prefetch pipeline** — a performance requirement of
the direct-I/O decision (§9). FUSE-over-io_uring is evaluated as the
interface matures.

### 7.8 Control plane, membership, and reconfiguration

The control plane is its own Raft group (RF = 2f+1): cluster membership,
desired shard→node placement, node incarnation, drain/rebuild. **Desired
placement ≠ authoritative configuration** — each shard's own group
transitions its actual membership through an overlapping-quorum
(joint-consensus) path, e.g. `{A,B,C} → joint{A,B,C,D,E} → {B,D,E}`, and a
replacement replica catches up before becoming a voting member (I18). Every
process/storage incarnation carries `(node_uuid, storage_incarnation,
process_boot_id)`; delayed packets from an old incarnation are rejected.

### 7.9 Request idempotency

Every mutating request carries a stable logical identity —
`(client/session UUID, request sequence)`. The replicated state machine
records enough completed-request state to make ambiguous retries idempotent
(I16) — critical for O_APPEND, create, link, unlink, rename, truncate,
mkdir. One `txid` threads through all participants of a cross-shard
transaction (§7.2).

**That state is a bounded window, not a history.** At this scale "one durable
record per operation, kept forever" is an unbounded table on every shard, so
what is retained per `(client_uuid, session_epoch)` is a
`highest_contiguous_seq` watermark, a small completion bitmap for the ragged
out-of-order edge above it, and a bounded reply cache for operations that
could still be retried. Records are reclaimable once the session is fenced,
its outstanding ambiguity is resolved (§7.5), and the retention window has
passed; transaction decision records have the matching participant-ACK
condition (§7.2).

## 8. The hot-path performance contract

Binding requirements, each traceable to P1/P4 — the difference between "the
architecture scales" and "the implementation scales." Full contract with
rationale: [performance.md](arch/performance.md).

- **NUMA-local, asynchronous, queue-depth-driven execution.** Ordinary
  PUT/GET and ordinary metadata commits take no cross-core lock and make no
  thread-to-thread handoff; NIC RX queues affinitize to reactor cores that
  drive NUMA-local NVMe queue pairs.
- **QoS isolation between planes.** Raft, membership, transaction decisions,
  and small metadata RPCs get their own RDMA queues / traffic class /
  credits, separate from bulk data; on NVMe, WAL, KV, bulk fragment I/O, and
  rebuild I/O have separate queues and backpressure.
- **Rebuild is distributed and rate-limited.** Deterministic repair owner,
  reads from distributed peers, never centralized; priority is foreground
  I/O > metadata > rebuild.
- **Read-side metadata is fetched in windows.** Per-lane batched range
  chunk-map fetches (lane i holds chunks i, i+64, i+128, …), with the
  metadata window running ahead of the data window; per-chunk lookups only
  for true random I/O.
- **Batching everywhere.** Raft messages, WAL group commit, KV applies,
  chunk publications, read-authority rounds: **no persistence boundary is
  paid per chunk, per metadata record or per Raft group when several
  operations can safely share one.** Durability boundaries are amortized to
  the largest batch the externally visible semantics allow — they are not
  eliminated, because a returned `write()` is durable (§7.3) and therefore
  crosses one.
- **FUSE capabilities and client prefetch** per §7.7.

## 9. The scaling envelope

"Raw hardware" must be **protection-adjusted**: protection consumes
hardware, so for k+f EC a logical chunk of size C writes `(k+f)/k × C`
physically (1.5× at 2+1). The ideal ceiling for logical write bandwidth is
approximately

```text
min( aggregate client egress        × k/(k+f),
     aggregate storage-node ingress × k/(k+f),
     aggregate NVMe write BW        × k/(k+f),
     EC encode CPU / memory bandwidth,
     metadata publication rate × chunk size )
```

plus smaller metadata/network overheads. The bar is a **high fraction of
that protection-adjusted ceiling** (85–95% would be extraordinary), and the
hot-path contract of §8 exists precisely to remove the software terms from
that min(). The data path is client-direct RDMA + EC; the hot-file metadata
path publishes on bounded per-file lanes rather than serializing on an inode
row (§7.3). Metadata op cost is one leader RTT + a majority replication —
the floor for any consistent system — and it no longer degrades with table
size or with same-file concurrency.

**"Near-linear" is defined per workload class** — several POSIX operations
are serial *by semantics*, and the claim excludes them explicitly instead of
silently failing them:

| Workload | Expected scaling |
|---|---|
| Many files, large aligned I/O | **near-linear with nodes** |
| One file, disjoint full chunks (many writers) | **near-linear up to the file's lane count / cluster size** |
| One giant `write()` syscall | data movement parallel; one atomic visibility decision per syscall (§7.3) |
| Many independent directories | **near-linear** — every directory scatters at birth (§7.4) |
| One spread hot directory | **near-linear across its hash shards** |
| One normal small directory | single-shard until size/pressure spreads it |
| Large sequential reads | near-linear **if userspace prefetch keeps queue depth high** (§7.7) and chunk maps are fetched in per-lane windows (§8) |
| `O_APPEND` on one file | reservation order + visible commit frontier serialized by semantics; data movement parallel (§7.3) |
| Overlapping same-range writes | serialized by semantics (CAS retry) |
| `truncate` on one file | content-epoch serialization (§7.3) |
| Cross-directory rename storm | transaction-throughput limited (§7.2) |
| Same-file `fcntl` lock storm | inode lock-authority limited (§7.6) |
| 4K random updates in 128K chunks | RMW limited — declared envelope (§7.3) |

A scaling benchmark is then a scientific question — *where did it stop: NIC,
NVMe, memory bandwidth, EC compute, fabric bisection?* — and any answer that
is a mutex, one leader, one thread, FUSE serialization, one WAL, or one
coordinator is by definition an EFS bug (§1).

## 10. Implementation order

Not a big-bang rewrite. Order chosen so each step is gated and the system
stays runnable. **Do not** go straight `KV → Raft → done`.

```text
0. Freeze the exact metadata placement model:
   inode_shard() · dentry_shard() · authoritative row ownership ·
   inode IDs + generations · FileID scoping of every data key (§7.3) ·
   the CREATE co-location rule (§7.4) ·
   the 64-lane permutation + active-lane bitmap (§7.3).
1. Simulator interfaces + the CURRENT state machine.
2. Define RPC operation IDs + the idempotency model.
3. Ordered KV applied state, incl. atomic batch semantics.
4. Single-shard Raft: persistence · election · replication · apply ·
   ReadIndex/linearizable reads · snapshots.
5. Simulator proves the single-shard invariants.
6. Safe Raft-group reconfiguration + control-plane desired placement.
7. Cross-shard transaction protocol, incl. concurrency control (§7.2).
8. Client sessions + fencing (§7.5), then the open-unlinked inode
   lifecycle (§7.6) on top of them.
9. Data-generation publication / fencing integration (§7.3), with the
   simulator checking the logical data protocol (arch/verification.md).
10. Directory layout-epoch spread (§7.4) + distributed locking (§7.6).
11. Delete the old snapshot / root-2PC machinery.
12. FUSE cache-coherence optimization only after zero/stale-cache semantics
    are demonstrably correct (data path starts as direct-I/O, §7.7).
```

**Step 1 has a hard prerequisite: the carve-up.** A pure state machine behind
transport/storage interfaces does not exist today — four files hold ~45% of
the tree and the state machine is fused with its I/O. So the *concrete* first
move is **Phase M** in the roadmap: carve the monolith into `raft/ kv/ meta/
wire/ data/ client/` behind interfaces
([development.md](arch/development.md)), as behavior-preserving refactor
gated by the existing suites. It is the dev-cycle lever in its own right
*and* the thing that makes step 1 possible — the simulator can only reuse a
state machine that is already pure. Build nothing in steps 2–11 as new
monolith code; every new component lands inside the carved boundaries.

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
- **FileID** — `(ino, inode_generation)`; the incarnation-scoped identity that
  keys every persistent data-plane object, so a reused ino cannot inherit a
  predecessor's in-flight writes.
- **Chunk generation** — the immutable version of a logical chunk's fragment
  set, named by a **globally unique candidate identity** (never a successor
  number); publication is atomic via metadata commit, and ordering comes from
  the publication CAS.
- **Coding profile** — the `{k, f, stripe/coding epoch, placement epoch}`
  identity of a generation's erasure coding; changes online, never mutated
  in place.
- **Write lane** — one of a file's 64 fixed metadata lanes; a lane shard holds
  the chunk-map entries plus the size/mtime/ctime high-water marks for its
  chunks. Lanes map to distinct shards by an odd-stride permutation, and lane
  0 is the inode's own shard.
- **Active-lane bitmap** — the inode row's monotonic 64-bit record of which
  lanes a file has ever used; it bounds what `stat()` must collect.
- **Commutative reduction** — transaction payload applied by a reducer
  (`MAX` on a lane's size/times), as opposed to an exclusive CAS key; it
  never blocks a prepare.
- **Content epoch** — the inode's content generation; bumped by truncate to
  invalidate lane state and reject in-flight stale publications. It is a
  fence, not part of any object's identity.
- **Session** — a client's `{client_uuid, session_epoch, state, touched
  shards}` record on its session shard; the epoch is the fencing token, and
  the touched-shard set is what the revocation barrier fences.
- **Intent** — a durable prepared record written by a participant in a
  cross-shard transaction; never externally visible as a committed effect.


---

# Appendices — satellite documents, verbatim


## Appendix 1 — Start here — task routing for contributors

*Source: `arch/START-HERE.md` (headers demoted, nav stripped).*

This page exists because of the bar in [development.md](development.md): **a
less advanced model must be able to contribute a correct change.** That is
only true if finding *the next task* and *the exact pages that govern it* is
mechanical. Read this page, then read at most the two or three files it
sends you to — not the whole spec.

---

### 1. The task right now

> **Phase M — carve the monolith**, step 1: `wire/`.
> [roadmap Phase M](../scaling-roadmap.md#phase-m--carve-the-monolith-first-the-dev-cycle-lever)

Nothing in the architecture migration ([architecture.md](#architecture)
§10) may start before this, because migration step 1 needs a pure state
machine behind transport/storage interfaces and today there isn't one. Phase M
is behavior-preserving refactor gated by the *existing* suites — no new design
and no new test infrastructure required.

**Rule for picking the next one after that:** Phase M steps are ordered and
each is gated; do them in order. When Phase M is done, the order is
[architecture.md](#architecture) §10, step by step. If a step looks like
it needs a design decision that is not already in the spec, that is a signal
to stop and ask — not to invent one.

---

### 2. Routing: "I am changing X"

Read the row's **Read** column and nothing else first. The **Governs** column
is what your change must not break; the **Gate** column is what proves it.

| You are changing | Read | Governs | Gate |
| --- | --- | --- | --- |
| Any wire message | [architecture.md §7](#architecture) op matrix, `include/efs/protocol.h` | build-ID compat; restart all servers together | `make test`, solo posix |
| Path lookup / dentries | [protocols/directory.md](protocols/directory.md) | I5–I8 | posix, posix2 |
| create / unlink / rename / link | [protocols/directory.md](protocols/directory.md), [protocols/transactions.md](protocols/transactions.md) | I5–I9, I16, I17 | posix, posix2, posixstress |
| Anything about file size, mtime, ctime | [protocols/data.md](protocols/data.md) "lanes"/"times" | I21, I22 | posix (size-visibility tests), posix2 |
| Chunk write / publish / truncate / append | [protocols/data.md](protocols/data.md) | I11–I15, I20–I22, I24, I25 | posix, posixpersist, fio honest matrix |
| Client reconnect, leases, locks, open-unlinked | [protocols/sessions.md](protocols/sessions.md) | I19, I23, I16 | posix2, posixstress |
| Cross-shard anything | [protocols/transactions.md](protocols/transactions.md) | I16, I17, I9 | posix2, posixstress |
| Raft, KV, replication, membership | [architecture.md §7.1/§7.8](#architecture), [failure-tolerance.md](failure-tolerance.md) | I1–I4, I10, I18 | simulator (once it exists), leaks |
| A hot path, for speed | [performance.md](performance.md) | P1–P4, §8 contract | fio honest matrix — **never** the stock `perf` write column |
| FUSE client behavior | [architecture.md §7.7](#architecture) | I24, kernel-cache rules | posix, posix2 |
| Module structure / file layout | [development.md](development.md) | ~1000-line file cap; header-only deps | `make test` + the suite for whatever moved |
| The spec itself | [development.md](development.md) "machine gate" | one home per normative table | regenerate `architecture-full.md`; links + `I1..I25` resolve |

Invariant texts live in [architecture.md](#architecture) §4. Where state
lives and which shards an operation touches live in §5 and §6 — those two
tables are the single source of truth; satellites explain them and never
restate them.

---

### 3. Done means

A change is finished when all of these hold. Do not stop early and do not
substitute one for another.

1. **It builds on a node** — never in the NFS home
   (see the fcstor deploy rule; a local `make` produces AVX-512 objects that
   SIGILL on the AMD test nodes).
2. **Unit tests pass:** `make test`, plus `test_meta_v6`, `test_dir_stats`,
   `test_ino_path`, `test_meta_slot` where metadata is involved.
3. **The gate from your routing row passes**, run with
   `tests/run_tests.sh <suite>`, and the result directory is recorded.
4. **A failure is a failure.** A timeout is not a skip; an empty TSV is not a
   pass; a suite that ran against a dead mount (`findmnt` not
   `fuse.efs-fuse`) did not run at all.
5. **The measurement is honest.** If you claim a speedup, it came from the
   documented method in [performance.md](performance.md), not from a cache.

---

### 4. Never, without asking

- Invent a design decision the spec does not contain (see §1).
- Restate a normative table in a satellite — link to its one home instead.
- Add a component as new monolith code; it lands inside the carved
  boundaries ([development.md](development.md)).
- Weaken an invariant to make a test pass.
- Widen a timeout instead of removing the work that made it slow.


## Appendix 2 — Naming

*Source: `arch/naming.md` (headers demoted, nav stripped).*

**Naming intent.** The bar for this design is that it earns the *idea* of an
"extreme filesystem": it scales as close as possible to the raw hardware.
Every design decision is checked against that bar — a component is wrong if
it serializes work the hardware could have done in parallel.

**On the name itself.** "extremfs" collides phonetically with **XtreemFS**
(an existing open-source distributed FS; the XtreemFS® trademark is
registered by Quobyte), and "EFS" is overwhelmingly associated with Amazon
Elastic File System. Neither is a good public identity for a project meant
to be found and attributed. The *idea* — never serialize what the hardware
allows in parallel — is the identity; the public **name should be chosen to
be distinctive and searchable**, and is deliberately left open here pending
a proper trademark/search check. The project uses "efs" as the working name
only.


## Appendix 3 — Design rationale

*Source: `arch/design.md` (headers demoted, nav stripped).*

This is the rationale document: why the architecture in
[../architecture.md](#architecture) has the shape it has, what is
claimed and what is not, and what was deliberately rejected. The spec itself
is normative; this file argues for it.

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
- **EFS-specific composition is *not* covered by Raft's proof.** RPC dedup,
  KV persistence ordering, cross-shard transaction logic, FUSE cache
  coherence, EC generation publication, chunk RMW, reconfiguration, and the
  orphan lifecycle remain **explicit proof / model / simulation
  obligations**. That is exactly what the
  [simulator](verification.md) is for.

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
them, are the actual contribution
([§0 of the spec](#architecture)):

> **P1 · Never serialize work that the semantics and the hardware allow to
> happen in parallel.**
> **P2 · Co-locate what must commit atomically on the common path; distribute
> what can evolve independently.**
> **P3 · Make stale work harmless instead of trying to prevent it.**

Traditional PFS architectures handle `1000 clients → 1000 different files`
well. The hard problem is `1000 clients → ONE file → different byte/chunk
ranges`. efs's answer is that if writes do not conflict, the filesystem must
not invent a conflict merely because they share an inode: disjoint chunks
publish to different metadata shards (see
[the data protocol](protocols/data.md)), size is a sharded high-water mark
co-located with the chunk it extends (P2), sub-chunk RMW is generation CAS,
stale publications are fenced by content epochs rather than prevented (P3),
and only the operations whose *semantics* require serialization (O_APPEND
EOF allocation, overlapping byte ranges, truncate, rename) are serialized —
because the semantics demand it, not because the filesystem happens to have
one inode lock.

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

### What is deliberately rejected

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
  invalidated — see the FUSE contract in
  [../architecture.md](#architecture)).
- **Kernel module.** Stay on FUSE; the low-level (inode-based) FUSE API
  migration is a separate, orthogonal client rewrite.
- **Clock-based leader leases on the authoritative read path.** Clocks are
  not correctness inputs.
- **Hard-disk support.** EFS is flash/NVMe-only (§1 hardware envelope of the
  spec). No seek-aware layouts, no rotational-latency hiding, no SMR
  handling — the design spends the assumptions flash makes true (cheap
  random I/O, deep hardware queues, µs-scale device latency) instead of
  defending against disk.
- **An N = 2f durability-only mode.** Permanent majority loss is disaster
  territory, not an operating mode — see
  [failure-tolerance.md](failure-tolerance.md) for why a survivor-log merge
  cannot be made safe by prose.


## Appendix 4 — Failure tolerance — derivation

*Source: `arch/failure-tolerance.md` (headers demoted, nav stripped).*

The normative rule lives in
[§2 of the spec](#architecture): **N ≥ max(2f+1, k+f), RF = 2f+1**.
This document derives it, explains why there is no weaker mode, and
specifies how f and k change online.

### The derivation

The two planes scale differently with the configured failure target f
(max simultaneous *permanent* node losses with no data loss, capped at 3):

- **Data (EC):** a `k+f` stripe survives any f losses (any k of k+f
  fragments reconstruct). Needs **N ≥ k+f** distinct failure domains.
- **Metadata (Raft):** a committed entry lives on a quorum
  Q = ⌊RF/2⌋+1. Zero data loss through f permanent losses needs every
  committed entry to keep at least one surviving copy (**Q > f**), and the
  system *staying authoritative with no human in the loop* needs the
  survivors to still form a quorum: **N − f ≥ Q**. With RF = N (every node
  carries every shard's replica in the small-cluster limit) that is
  N ≥ 2f+1. More generally RF = 2f+1 replicas per shard, placed on distinct
  failure domains, gives Q = f+1 and survival of any f replica losses with
  both properties.

**f=3 needs 7 nodes, not 5 or 6.** On 5 nodes the best metadata
configuration is RF=5, Q=3, and 3 ≯ 3 — the 3 dead nodes could be exactly
the quorum that acknowledged the most recent writes. The config validator
rejects any configuration that violates N ≥ max(2f+1, k+f).

### Why there is no N = 2f mode

At N = 2f exactly (e.g. 4 nodes wanting f=2), f permanent losses leave a
minority of survivors. Every committed entry still has ≥1 surviving copy
(Q + f > N), so the *bits* survive — but plain Raft cannot re-establish
authority without a majority, and **no local merge rule over the surviving
logs can reconstruct the committed prefix.** Commitment information died
with the majority.

The tempting merge — "per log index, the highest-term entry wins" — is
unsound. Raft's log-prefix properties hold only *within logs produced
through Raft*; they do not license constructing a new log by independently
selecting entries per index. A legal situation:

```text
survivor A:   index 5  term 10  X        (uncommitted branch)
survivor B:   index 5  term 9   Y        (committed earlier)
              index 6  term 11  Z        (appended by a term-11 leader
                                          whose log contained Y at index 5,
                                          elected by a majority excluding A)
```

The merge produces `X@5, Z@6` — a log that **never existed**. `Z` was
created on a prefix containing `Y`, not `X`. Worse, the merge *drops* `Y`,
which was committed and possibly acknowledged to a client. Operation-ID
idempotency does not fix this: the problem is not duplicate application but
a synthesized state-machine history no leader authorized.

Raft is deliberately majority-based; the paper provides availability only
while a majority remains, and production systems (e.g. etcd) treat
permanent quorum loss as disaster recovery, not ordinary log
reconciliation. EFS therefore does not offer the mode: permanent majority
loss is **disaster territory** (an operator procedure over surviving state),
not an efs operating mode. If a 4-node/f=2-style disaster-survivability
guarantee is ever genuinely needed, it is designed as a separate protocol
with its own proof — not bolted onto Raft by prose.

### Changing f or k online

The cluster carries `effective_f` (the guarantee currently in force) and,
during a change, `target_f`. Raising f:

1. add shard replicas via joint consensus (safe reconfiguration, §7.1 of
   the spec);
2. re-stripe **every protected data generation** to the new k+f width —
   each re-striped chunk is a *new immutable generation* written under the
   new **coding profile** (`coding_profile_id`, see
   [the data protocol](protocols/data.md)), never an in-place
   reinterpretation of an existing generation;
3. verify;
4. only then commit `effective_f = target_f`.

The guarantee changes at that commit, not when the operator asks. Lowering
f runs the reverse. Both are placement changes, not reformats. EC stripe
width k is independently configurable (wider k on larger clusters trades
encode CPU for storage efficiency, e.g. 4+3 = 1.75× overhead instead of
2+3 = 2.5×); the binding constraint is always N ≥ max(2f+1, k+f).

### Degraded operation

While the cluster is already operating with `u` unavailable failure domains
(`u ≤ f` failures already consumed), a data generation may publish with
`D ≥ k + (f − u)` durable fragments, marked degraded and queued for repair
— the invariant "survives the remaining f−u further losses" is maintained
throughout. Degraded publication is never used because a target is *slow*,
only because it is *unavailable*. The full rule is in
[the data protocol](protocols/data.md); rebuild scheduling is in
[performance.md](performance.md).


## Appendix 5 — Protocol — cross-shard transactions

*Source: `arch/protocols/transactions.md` (headers demoted, nav stripped).*

The operations that genuinely touch more than one shard (see the
operation→participant matrix in
[§6 of the spec](#architecture): mkdir/rmdir, cross-dir rename,
hardlink, unlink with nlink>1 or with the dentry on a different shard than
the inode, and every multi-lane atomic `write()`) need **atomic visibility**,
which a reconcile rule alone does not provide. efs uses a **Raft-backed
distributed transaction** — decentralized, no global transaction server:

```text
txid (stable across all participants)
coordinator = participant[hash(txid) % participant_count]
participant set

PREPARE  -> each participant writes a durable intent
DECISION -> coordinator Raft-commits COMMIT | ABORT (durable)
RESOLVE  -> participants apply/abandon per the decision
```

The coordinator is **dispersed by txid**, not pinned to a fixed participant:
this protocol is on the hot write path (every multi-lane `write()` uses it —
see [the data protocol](data.md)), and a large file's writes repeatedly touch
the same lane set — a deterministic-per-file coordinator would re-create a
same-file serializer. Hashing the txid spreads decision records across the
file's own lane leaders, so independent write transactions never funnel into
one shard.

Atomic *commitment* is only half the problem. The other half is
**concurrency control** — without it, two concurrent transactions over the
same keys (`rename(a,b)` vs `unlink(a)`; `rename(a,b)` vs `rename(a,c)`) have
no serializable history even though each commits atomically.

- **Conflict detection at PREPARE.** A prepare is conditional:
  `prepare(T, key, expected_version)`. It fails if another live transaction
  holds a conflicting intent on `key`, or if `key`'s current version no
  longer equals `expected_version` (the key changed since the transaction
  read it). An intent on an **exclusive** key is that key's lock: at most one
  live transaction holds it.
- **Not all transaction state is an exclusive key — and getting this wrong
  silently re-creates the hotspot one level down.** A transaction's effects
  split into two kinds, and only the first kind takes a lock:

  ```text
  exclusive / CAS keys        the chunk-map entry, a dentry, an inode row
                              -> conflict, version check, one holder

  commutative reductions      MAX(lane.max_end,   ...)
                              MAX(lane.max_mtime, ...)
                              MAX(lane.max_ctime, ...)
                              MAX(dir_lane.mtime, ...)
                              -> transaction PAYLOAD, not a lock
  ```

  Consider two writers publishing chunk 10 and chunk 74 of one file, which
  by construction share a write lane ([data protocol](data.md)). They
  conflict on nothing: different chunk-map keys, and `MAX` commutes. If the
  transaction machinery treated `(FileID, lane)` as an exclusive intent key
  because both transactions update the lane's high-water marks, they would
  conflict anyway — and the per-file serialization the lanes were introduced
  to remove would simply have moved from the inode to the lane. So
  **reductions are carried in the intent and applied by the reducer at
  resolve time; they never block a prepare.** Several live transactions may
  hold reduction payloads on the same lane concurrently, provided their
  exclusive keys are disjoint. This is P1 applied to the transaction layer
  itself.
- **No-wait, and what that buys.** A prepare that finds a conflicting
  intent **fails immediately** — nobody ever waits while holding an intent,
  so no wait-for cycle can form and deadlock is impossible by construction.
  The loser aborts its acquired intents and retries with randomized backoff
  (livelock is bounded by backoff/priority tuning, a liveness knob, not a
  correctness mechanism). Because no-wait already gives deadlock freedom,
  prepares do **not** need sequential acquisition:
  - **Write-publication transactions (hot path): all PREPAREs are sent in
    parallel** (P1 — the transaction must not serialize what the hardware
    can do concurrently).
  - **Namespace transactions (rare):** participants and keys are prepared
    in a canonical global order (by shard id, then key) — a simple,
    deterministic schedule that minimizes retry churn for the
    rename/unlink/link family.
- **Visibility rule.** An intent is never externally visible as a committed
  effect; readers that encounter an intent resolve it against the
  coordinator's decision, so clients never observe an illegal intermediate
  (I17). **A transaction becomes visible at its decision, not when background
  cleanup happens to run** — a reader that meets a decided-but-unresolved
  intent must reflect the decision, or a client could fail to see its own
  just-returned write. Resolution has exactly four outcomes, and the last one
  is a safety rule, not an error path:

  ```text
  authoritative COMMIT        -> the new value
  authoritative ABORT         -> the old value
  authoritative NO-DECISION   -> the old value; this read linearizes before
                                 whatever the decision turns out to be
  cannot establish authority  -> retryable error (EIO/EBUSY). Never guess,
                                 and never report the effect as absent.
  ```

  The fourth line is I9 again, in transaction clothing: *unknown because
  unreachable* must never be answered as *not there*.
- **Idempotency / retry.** The whole transaction carries one `txid`;
  re-prepare and re-resolve are idempotent (I16).
- **Crash recovery.** A recovering participant or a reader that finds an
  intent queries the coordinator's durable decision and drives the
  transaction to completion (L5).
- **GC.** Resolved intents are reclaimed (L7-class). A **decision record**
  outlives its intents — a recovering participant must still be able to ask
  what happened — so it has its own explicit condition: reclaimable once
  every participant has acknowledged the decision durably, and no participant
  that could still be recovering remains. Until then it is retained,
  bounded by the transaction rate rather than by the operation history.
  Duplicate-suppression records have the matching bound in
  [sessions.md](sessions.md).

With conditional prepare + no-wait, the protocol yields strict
serializability for multi-shard ops: conflicting transactions are ordered by
intent acquisition, non-conflicting ones run concurrently.

**Terminology.** What the old design deleted is the **global root-snapshot
2PC durability mechanism** — the one whose commit point was an
insufficiently-replicated coordinator/root-generation. A per-operation
Raft-backed 2PC-like protocol for cross-shard transactions is a different
thing and is exactly what's needed here. Raft (replicated ordering within a
shard) and 2PC (atomic commitment across shards) solve different problems;
efs uses each where it fits.


## Appendix 6 — Protocol — data plane

*Source: `arch/protocols/data.md` (headers demoted, nav stripped).*

This is where "scales with raw hardware" is won or lost. The mechanism
(client-direct RDMA, EC) is unchanged from the current implementation; what
this protocol adds is a precise commit and concurrency protocol.

**Erasure coding is k+f, matched to the failure target.** The stripe is k
data + f parity fragments on k+f distinct nodes, where f is the configured
failure target ([§2 of the spec](#architecture)): 2+1 at f=1, 2+2 at
f=2, 2+3 at f=3 (storage overhead 1.5×/2×/2.5×). k=2 is the minimal-width
default; larger clusters may widen k (e.g. 4+3 = 1.75×) to trade encode CPU
for capacity. Any k fragments reconstruct the chunk, so reads survive any f
losses by construction. Writes place all k+f fragments; while nodes are
down, a write that cannot place its full stripe either blocks on repair or
commits a degraded stripe that is explicitly queued for re-striping — never
silently under-protected.

**Every persistent data identity is scoped by `FileID`, not by `ino`.** An
ino is reused after deletion, and the inode generation is what makes a
handle ABA-safe ([§5 of the spec](#architecture)) — so the data
plane must carry the same incarnation, or a delayed fragment PUT from the
*previous* occupant of ino 100 can collide with the new one:

```text
FileID = (ino, inode_generation)
```

`FileID` — never bare `ino` — keys fragment objects, chunk-map entries,
write lanes, append reservations, and publication intents.

**Immutable chunk generations.** Each logical chunk's fragments are keyed by

```text
(FileID, chunk_index, chunk_generation, fragment_index, coding_profile_id)
```

A generation is immutable once written. Reconstruction never mixes
generations (I13). Metadata publishes a generation only after it satisfies
EC durability (I14); an unpublished generation's fragments are orphans,
reclaimable without touching committed state (I15). This is what makes
stale-client writes safe: a stale client may write an orphan generation, but
it cannot make it visible without winning the metadata publication protocol
(I20).

**`chunk_generation` is a unique candidate identity, never `G+1`.** A
successor-numbering scheme is unsound with concurrent writers: two clients
that both read committed generation `G` would both mint "`G+1`" for
*different* bytes, and would then PUT **different content under the same
object identity** — destroying the immutability the whole design rests on.
So a candidate generation is globally unique by construction, and ordering
is supplied by the publication CAS rather than by the identifier:

```text
candidate_generation = H(client_uuid, session_epoch, op_id,
                         chunk_index, retry_attempt)      (or a 128-bit UUID)

publish:  CAS(expected = committed base generation,
              publish  = candidate_generation)
```

Two concurrent writers therefore build two *distinct* immutable objects;
exactly one wins the CAS, and the loser's object is an orphan it can reclaim
and retry over (P3). Successor notation (`G`, `G+1`) appears in this
document only as pseudocode for logical succession — it is never an
identifier.

**`content_epoch` is a fence, not part of the object identity.** It tags
*publication requests* and *lane state* so a truncate can invalidate both in
one single-shard commit (below), but it must **not** appear in a fragment
object's key. If it did, a `truncate()` to a smaller non-zero size — which
POSIX requires to preserve the surviving prefix — would strand every
surviving chunk under an epoch no reader consults, and re-tagging them
instead would turn one truncate into an O(file-size) rewrite (8M chunk-map
entries for a 1 TiB file). Uniqueness across epochs is already guaranteed by
`FileID` + the unique candidate generation, so the epoch has no identity
work left to do.

**`coding_profile_id` identifies the coding interpretation** — `{k, f,
stripe/coding epoch, placement epoch}` — because f and k change online
(see [failure-tolerance.md](../failure-tolerance.md)): the same
`(FileID, chunk_index, candidate_generation)` under profile 2+1 and under 2+2
would be ambiguous without it. Re-striping to a new profile is itself
generation-based (P3): reconstruct the chunk, write a **new immutable
generation** under the new profile, satisfy the new durability requirement,
publish it atomically, then GC the old generation. An existing generation's
coding interpretation is never mutated in place.

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
not a performance trade-off. Write latency therefore tracks the slowest
fragment target, not the fastest k; that is the honest price of the
guarantee, and it is hidden by queue depth and pipelining
([performance.md](../performance.md)), never by weakening the durability
definition.

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
re-striped and the degraded mark cleared (rebuild scheduling in
[performance.md](../performance.md)). Degraded publication is never used
just because a target is slow — only because it is unavailable.

So a `write()` that has returned success **is** durable and visible — stronger
than POSIX, and deliberately so (POSIX requires durability only at `fsync`).
This is a deliberate **latency-for-durability trade**, and it is not free:
the fast path is a small distributed synchronous commit per write (EC encode
+ k+f durable fragment writes + a durable quorum publication), so
single-threaded small-write latency and IOPS pay it. Peak *throughput* is
recovered through asynchronous parallelism, queue depth, and batching — not
by weakening the guarantee. The stronger semantic buys simpler failure
reasoning (no ambiguous gap between "write succeeded" and "metadata
committed") and it is benchmarked brutally against the strict-POSIX
alternative (`write()` = visible, `fsync()` = durable) before the decision
is considered final. `fsync()` then only has to cover whatever client-side
batching efs intentionally permits plus namespace ordering.

### Partial-chunk and concurrent writers (same file)

This is the hot-file problem and it gets a real answer. The governing rule:
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

  **The lane count is fixed, and lanes are activated, not resized.** A
  tempting formulation — "derive L from the required publication rate" —
  does not survive contact with the key space: if `lane = f(chunk_index) % L`
  then changing L **relocates essentially every chunk-map key of the file**,
  so L could never adapt at runtime without a full file-layout migration
  protocol. Instead the lane space is a fixed maximum, and a file's *use* of
  it grows:

  ```text
  LMAX      = 64                                   (fixed, for every file)
  lane(ci)  = chunk_index % LMAX

  lane_shard(FileID, lane) = (inode_shard(ino) + lane * stride(ino)) & 0xFFF
  stride(ino)              = 2 * (hash(ino) & 0x7FF) + 1        (always odd)
  ```

  Two properties fall out of that formula, and both are load-bearing:

  - **The 64 lanes are guaranteed to land on 64 *distinct* shards.** Because
    the shard count is a power of two and `stride` is odd, `lane ↦ (base +
    lane·stride) mod 4096` is a permutation — so the mapping is injective
    over the 64 lanes. Independent hashing (`hash(ino, lane) & 0xFFF`) would
    not be: birthday collisions would silently give a hot file fewer
    effective leaders than it asked for.
  - **Lane 0 is the inode's own shard.** An ordinary small file's chunk map,
    size, times and inode row are therefore all on one shard: no fan-out at
    all for the overwhelmingly common case.

  Different files get different strides *and* different bases, so lane sets
  scatter across all 4096 shards and global balance is preserved.

  **The active-lane bitmap.** The inode row carries a 64-bit
  `active_lanes`. A lane counts only once it has been used:

  ```text
  first publication into a not-yet-active lane
      -> the publication transaction also sets that bit on the inode row
      -> monotonic; bits are never cleared
  stat() collects only the ACTIVE lanes
  ```

  So a one-chunk file has exactly one active lane and `stat()` is one read;
  a huge hot file ends up with 64 active lanes and 64 independent leaders.
  Activation costs at most **64 inode-row interactions over the entire
  lifetime of the file** — not one per write — and a multi-chunk write sets
  every bit it needs in the transaction it was already running, so a
  sequential writer activates the full set within its first few writes
  rather than in 64 separate round trips.

  A lane's shard carries, for every chunk assigned to it, the chunk-map
  entry **plus that lane's size, mtime and ctime high-water marks** — so
  chunk publication, size update and time updates for an extending write are
  **one Raft entry on one shard** (P2; this is what makes I21 hold with no
  cross-shard protocol):

  ```text
  { publish (FileID, epoch, chunk_index, candidate_generation)   <- CAS key
  ; MAX(lane.max_end,   end_offset)                              <- reduction
  ; MAX(lane.max_mtime, now)                                     <- reduction
  ; MAX(lane.max_ctime, now) }                                   <- reduction
  ```

  The distinction annotated above is not decoration — it is what keeps the
  lane from becoming the next bottleneck, and it is specified in
  [transactions.md](transactions.md): the chunk-map entry is an **exclusive
  CAS key**, while the three `MAX`es are **commutative reductions**. Two
  writers publishing *different* chunks that happen to share a lane must not
  conflict; if lane state were an exclusive intent key they would, and the
  inode hotspot would simply have moved down one level.

  Because lane i holds chunks i, i+64, i+128, …, a sequential window of a
  file is one contiguous range request per lane (read windows in
  [performance.md](../performance.md)), and the collect set for `stat()` is
  known from the bitmap without reading anything else. A disjoint full-chunk
  write publishes on its lane's shard — never on the inode's shard. The data
  is distributed *and* the metadata publication is distributed, up to 64
  leaders per file. This is what makes the hot-file claim structurally true
  rather than a slogan.
- **stat() is a distributed snapshot, not a bare MAX.** Reading `MAX` over
  several independently-linearizable shards is not automatically an atomic
  snapshot — and the content-epoch double-check alone only closes
  *truncate-vs-stat*, not *concurrent extending writes across lanes*: with
  lanes A=B=100, a stat that reads A=100, then sees writers commit A=1000
  and B=500, then reads B=500 would return 500 — a size the file never had.
  So each lane carries a monotonically increasing `lane_seq` (bumped by
  every lane mutation), and stat() is a **double collect**:

  ```text
  1. read inode row -> content_epoch E, mtime_gen M, active_lanes A,
                       base_size, base_mtime, base_ctime
  2. collect the lanes in A:  (lane_seq, max_end, max_mtime, max_ctime)
                              [entries tagged E]
  3. collect those lane_seq values again
  4. re-read inode row -> E, M, A unchanged?
  5. if every lane_seq is unchanged AND E/M/A unchanged:
         the lane vector existed simultaneously (all values held between the
         end of collect 1 and the start of collect 2) -> MAX is linearizable
     else: retry
  ```

  The common path stays lock-free: `2·|A|+2` point reads, and `|A|` is 1 for
  an ordinary small file and at most 64 for the hottest. To keep a
  continuously-written file from starving the retry loop, the retry count is
  bounded (a few); on exhaustion stat() falls back to a **read-only
  multi-shard transaction** over the active lane set (the
  [transaction machinery](transactions.md): read intents on the lane set,
  one consistent read) — the correctness escape hatch, rare in practice.
  The same double collect validates the distributed times below. A collect
  that meets a prepared publication intent resolves it against the durable
  decision (below); it never treats an unreachable authority as "absent"
  (I9).
- **Timestamps have exact semantics — and none of them reintroduce the inode
  bottleneck.** POSIX has three, and a write touches two of them: it updates
  `mtime` *and* `ctime`. Routing write-generated `ctime` to the inode row
  would send every hot-file writer straight back to the inode leader and
  undo the lanes entirely, so **write-generated mtime and ctime both live in
  the lanes**; the lane leader stamps `max_mtime`/`max_ctime` on the
  publication entry and a million writers never touch `inode_shard(ino)`.

  Explicit status changes remain inode-row events, with the POSIX
  distinction respected exactly — in particular **`chmod` updates ctime, not
  mtime**:

  ```text
  chmod / chown / link / unlink   -> ctime
  utimens                         -> the given times, plus ctime
  truncate                        -> mtime and ctime, plus content_epoch
  write                           -> mtime and ctime, in the lanes
  ```

  Each explicit change updates the inode row's `base_mtime`/`base_ctime`
  (and `base_size` for truncate) and bumps `mtime_gen`; lanes stamp their
  time updates with the `mtime_gen` current at publication. `stat()`
  computes, under the same double-collect validation:

  ```text
  size  = MAX( inode.base_size,
               active current-epoch lanes' max_end )
  mtime = MAX( inode.base_mtime,
               lanes' max_mtime where lane.mtime_gen == inode.mtime_gen )
  ctime = MAX( inode.base_ctime,
               lanes' max_ctime where lane.mtime_gen == inode.mtime_gen )
  ```

  `base_size` is what makes truncate correct without touching the lanes:
  after a shrink the lanes are epoch-invalidated and contribute nothing, and
  the inode's own value is the answer until new writes exceed it. The
  `mtime_gen` guard is what prevents a pre-`utimens` write's lane timestamp
  from resurrecting over an explicit timestamp set. So a `stat()` after a
  returned `write()` — from any client — sees mtime and ctime at least as new
  as that write, and an explicit `utimens` is honored exactly. This is
  defined efs behavior, not "where POSIX permits."
- **atime is an explicit policy choice, not an omission.** A strict POSIX
  access timestamp would turn every read into a metadata mutation — the
  precise opposite of the read path this architecture is built for, and the
  reason `relatime` is the Linux default and `noatime` is universal in HPC.
  efs therefore defines: **`noatime` semantics are the default** (atime
  tracks the inode's mtime/ctime and is not advanced by reads), and
  `relatime` semantics — advance atime only when it precedes mtime/ctime, or
  is older than a coarse interval — are available as a mount option, paid for
  by a coalesced, best-effort inode update that is never on the read
  completion path. Strictly-conforming per-read atime is **not offered**; it
  is a scaling contradiction, and the deviation is documented in the POSIX
  contract rather than hidden.
- **Chunk publications are batched at the Raft layer.** Semantics stay
  per-chunk; physical persistence does not. At 128 KiB chunks, 100 GB/s of
  logical write bandwidth is ~800k chunk publications/s — one synchronous
  metadata transaction per chunk would make the metadata plane the ceiling
  long before the NICs or NVMe are. So the client partitions publication
  records by lane shard and the shard leader commits **many publications per
  Raft proposal**, with one durable group commit covering the batch (this
  composes with the multi-Raft runtime's group-committed WAL —
  [performance.md](../performance.md)). The per-chunk publication record,
  its CAS, and its lane updates are unchanged — only the physical commit is
  amortized.
- **One write syscall publishes atomically; the data movement does not
  serialize.** A `write()` spanning several chunks uses the
  [transaction machinery](transactions.md) for the *visibility decision
  only*:

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
  disjoint keys and do not contend. The **relaxed mode** (§3 of the spec)
  skips the transaction and publishes per-chunk.

  **The FUSE syscall boundary is an explicit unresolved contract.** POSIX
  requires syscall-level read/write atomicity, but with
  `FUSE_CAP_ASYNC_DIO` the kernel may split one large direct-I/O syscall
  into several smaller concurrent FUSE write requests, and FUSE does not
  tag requests with their originating syscall. So "POSIX-atomic at `write()`
  granularity" and "atomicity unit = one FUSE write request" cannot both be
  final claims. This is a **kernel-interface limitation, not an EFS protocol
  problem** — the machinery above publishes whatever unit the client hands
  it atomically. The contract:

  - The atomicity unit is **one FUSE write request**; `max_write`/
    `max_pages` are sized so the writes that matter arrive as one request.
  - **EFS MUST NOT claim full POSIX write atomicity above the FUSE
    request-size boundary until this is solved.**
  - Investigation paths, in order of preference: (1) kernel/FUSE support
    for operation grouping; (2) intentionally short-write at the supported
    atomic boundary so the kernel never splits a syscall; (3) a
    client/kernel extension carrying a syscall transaction ID;
    (4) disabling the relevant parallel FUSE behavior in a strict mode;
    (5) a clearly documented Linux/FUSE compatibility mode vs. strict
    POSIX mode.
- **Sub-chunk read-modify-write** uses generation CAS: a writer reads
  committed base generation `B`, builds a **unique candidate generation**
  from it (above — not `B+1`), and publishes with
  `CAS(expected_generation = B)`; on conflict it refetches the committed
  generation, reapplies its byte-range patch, mints a fresh candidate, and
  retries. The loser's candidate is a distinct immutable orphan, never a
  competing object under a shared name. Concurrent
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
  claim in §1 of the spec does not cover random 4K mutation. If benchmarks
  later show the workload needs it, the designed escape hatch is **immutable
  delta objects** (a small write appends a delta object to the chunk's
  publication rather than rebuilding the chunk; a background consolidation
  folds deltas into a new base generation) — that makes disjoint sub-chunk
  writes independent, at real complexity cost. It is not built until
  measured.
- **Truncate is a content-epoch bump, with a stated linearization rule.**
  Truncate (or any wholesale content replacement) advances the inode's
  `content_epoch` on the inode row — a single-shard Raft entry that also
  stamps the new authoritative `base_size`. The bump does three things and
  no more: it invalidates every write lane's size/time high-water marks at
  once, it causes any in-flight publication naming the old epoch to be
  **rejected at commit**, and it fixes the file's size until subsequent
  writes push lanes above it.

  It does **not** invalidate committed chunk data. Chunk-map entries for
  surviving chunks stay valid across the bump (they are keyed by `FileID`
  and candidate generation, not by epoch — above), so shrinking a 1 TiB
  file is O(1) metadata work, not a rewrite of 8M entries. Chunks lying
  wholly beyond the new size become unreferenced and are reclaimed lazily
  (L7); the single chunk straddling the new end is rewritten as one new
  generation. The rule:

  ```text
  a write publishing under epoch E linearizes BEFORE the operation that
  advances the file from E to E+1;

  a write that begins after truncate has returned uses epoch E+1.
  ```

  A writer that read epoch 17 and publishes after the file moved to epoch 18
  has its publication rejected by the lane leader's epoch check, so no reader
  ever considers it — P3: the stale write is harmless, not prevented. Its
  candidate fragments are orphans (L7); old-epoch *lane* state is discarded
  with the bump.
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
  ([sessions.md](sessions.md)), recovery **commits the reserved range as a
  zero hole** — a deliberate committed state, not an emergent one — which
  unblocks the frontier for everyone behind it. Only failure recovery can
  leave the frontier briefly blocked behind an unresolved reservation. The
  precise POSIX statement: a hole arises only from a write that never
  returned success, it reads as zeros (indistinguishable from a sparse
  region), and no returned write is ever lost. Reservations are never
  rolled back, because later appenders already received offsets past them.

### Stale-target fencing — targets are dumb, metadata decides

Because clients write directly to data targets, Raft terms on the metadata
path do not fence the data path. But the fencing is *not* "target rejects an
older generation" — generations are immutable candidate objects, and two
concurrent clients may legitimately produce different candidate generations
for the same chunk with no global ordering between them. The correct model:

- A fragment object `(FileID, chunk_index, candidate_generation,
  fragment_index, coding_profile_id)` is **immutable and its PUT is
  idempotent**. A target stores it; storing the same object twice is a
  no-op. A target may hold several candidate generations of one chunk at
  once — that is fine, because they are distinct immutable objects.
- The target verifies only **placement epoch, target incarnation, and object
  identity** — i.e. "am I the current, legitimate home for this object?" It
  does **not** decide which candidate generation is logically newer, and it
  does **not** check client sessions (a fenced client's PUT just stores an
  orphan; publication is rejected on the metadata path — see
  [sessions.md](sessions.md)).
- **The metadata authority alone decides which generation is committed**
  (via the publication CAS). Unpublished candidates are orphans, reclaimed
  without touching committed state (I15).

This keeps the data nodes dumb — which fits the minimalist design — and it is
what makes I20 precise: a stale writer cannot make its generation *visible*,
because visibility is granted only by the metadata publication protocol, not
by anything a data target does.

#### Publication validates the durability evidence

Dumb targets have a consequence that must be stated, or the model has a hole:
if nobody on the data path checks the coding and placement contract, then
**the metadata publication authority is the only place it can be checked**,
and it must actually do so. A stale client can hold an old placement view and
successfully write a full, durable stripe *to the wrong set of targets*.
Those PUTs are harmless in themselves (P3) — but the publication naming them
must be rejected, not accepted because "k+f ACKs arrived."

A publication therefore carries its evidence, and the lane leader validates
it before the entry is committed:

```text
publication = { FileID, content_epoch, chunk_index,
                candidate_generation,
                coding_profile_id, placement_epoch,
                durable_ack_set = [ { target incarnation,
                                      fragment role/index,
                                      object identity,
                                      fragment checksum } ... ] }

accept only if:
    coding_profile_id  == the file's current profile
    placement_epoch    == the current placement for those fragments
    the ack set covers exactly the k+f expected fragment roles
    those targets sit in the required number of distinct failure domains
    every ACK names THIS candidate object, not some other generation
    (degraded publication: the relaxed k+(f-u) rule above, and only then)
```

This is what lets targets stay dumb without weakening I11 or I14: the
contract is enforced once, by the authority that decides visibility, instead
of by every target on the fastest path.

#### End-to-end integrity (I25)

The failure model excludes Byzantine *participants*, but silent media
corruption is not Byzantine behavior — it is ordinary SSD/DRAM/wire reality,
and erasure coding is specifically bad at it: EC can reconstruct from a
corrupt fragment and produce confidently wrong data, because the code has no
way to tell which input lied.

So every durable fragment carries a checksum over **its immutable identity
plus its payload** (identity included, so a correct fragment stored under the
wrong name cannot pass). The checksum is verified on read and before any use
as reconstruction input. A fragment that fails verification is treated as
**unavailable** — it is never fed to the decoder, the read is served by
reconstructing from other fragments, and the bad fragment is queued for
repair like any lost one. The immutable-generation design makes this cheap:
the checksum is computed once, at creation, and never has to be maintained.

This also gives the simulator ([verification.md](../verification.md)) a
natural corruption fault to inject, which is the only way the property is
ever actually tested.


## Appendix 7 — Protocol — directory placement & spreading

*Source: `arch/protocols/directory.md` (headers demoted, nav stripped).*

This is the placement decision everything else hangs off. The authoritative
operation→participant matrix derived from these rules is
[§6 of the spec](#architecture); this document is the placement
protocol itself.

### Placement rules

- **`inode_shard(ino) = ino & 0xFFF`.** The authoritative inode row lives
  here.
- **A dentry lives on its parent's shard**
  (`dentry_shard = inode_shard(parent_ino)`), with a **threshold-based
  spread** for huge/hot directories (below).

**Why parent-shard dentries.** It makes the two dominant namespace ops
single-shard: `create`/`unlink` inside a directory touch only
`inode_shard(parent)` for the dentry. readdir is one ordered range scan on
one shard. The cost — a hot directory funnels to one leader — is real but
bounded, and it is the *right* trade because directory locality is the common
case.

**The two-index problem, resolved.** The full mutable inode row is **not**
stored in both indexes. The dentry index stores a **projection**:

```text
(parent_ino, name) -> (ino, generation, type)        // dentry: small, lookup hint
(ino)              -> authoritative inode_row        // inode: single source of truth
```

The dentry value carries only what LOOKUP needs to route and to validate a
handle (`ino`, `generation`, `type`). All mutable attributes (mode, uid, gid,
nlink, …) live **only** in the inode row — with two deliberate exceptions
defined in [the data protocol](data.md): *write-generated* size and mtime
live in the file's bounded write lanes (otherwise every write would serialize
on the inode shard), and `stat()` merges them under the double-collect
snapshot protocol. Explicit attribute changes (`utimens`, `chmod`, `chown`,
truncate) always update the inode row. This eliminates any
chmod/chown/mtime/size/nlink divergence between the two indexes, and it makes
hardlinks clean: many `(parent,name)` keys point at one authoritative inode.

**Cost we accept:** a LOOKUP that needs attributes does a dentry get (parent
shard) then an inode get (inode shard) — two point-gets, possibly two shards.
That is the price of not duplicating mutable state, and it is paid only on
the attribute path, not the name-exists path.

### The CREATE co-location rule (load-bearing) — files local, directories scatter

Parent-shard dentries only make CREATE single-shard if the new inode lands
on the *same* shard as the dentry. So inode allocation is constrained by
placement (P2). But a naive "child inherits the parent's shard" rule has a
fatal consequence: a directory's children inherit its shard, *their* children
inherit it again, and an entire subtree funnels into one Raft leader until
each directory individually grows hot enough to spread — "many independent
directories → near-linear" would be false for exactly the common case. So
the rule distinguishes files from directories:

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
shard, via [transactions](transactions.md)) and in return buys **an
independently scalable subtree for the directory's entire lifetime** —
`/projectA/*` and `/projectB/*` live on different shards from birth, not
after a spread event. Directory creation is rare next to file creation, so
the transaction cost is amortized to nothing; the parallelism it buys is
permanent. The per-export `salt` (chosen at mkfs) prevents correlated names
from clustering across exports.

The allocator picks an ino whose low 12 bits equal the required shard (inos
are allocated per-shard — §5 of the spec — so this is free: the shard's
allocator simply hands out its own inos).

**The stranded-set bound still holds, now for files.** A normal directory's
*files* keep their inode rows on the directory's shard forever — their ino
identity encodes the shard (`ino & 0xFFF`) and cannot migrate.
`EFS_DIR_SPREAD_MIN` is therefore a **scalability bound, not just a space
threshold**: it caps how many file rows can be permanently co-located on one
shard before the directory spreads. It is kept low enough that the stranded
set is irrelevant at scale (thousands of rows, never millions).

### Hot-directory spread is a layout-epoch protocol, not a flag

A *normal* directory deliberately serializes its creates on one shard leader
— a locality choice, and the right one (directory locality is the common
case) — which means P1 is satisfied by the *spread mechanism*, not by
pretending the conflict doesn't exist. The spread trigger therefore responds
to **actual serialization pressure, not just namespace size**:

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

**Mutations of not-yet-migrated entries use a dominating tombstone** — this
is what enforces I8 (no resurrection) during the split. "Writes go to the
hashed location" is not enough on its own: if `unlink(foo)` only wrote the
hashed side while the old local `foo` still exists, the migrator could later
copy the stale local `foo` into hashed storage and resurrect it. So a
mutation of an entry that has not migrated yet writes an authoritative
hashed record first:

```text
unlink(foo) while foo is still LOCAL-only:
    write HASHED(foo) = TOMBSTONE(layout_epoch = e)

lookup(foo):
    hashed record or tombstone exists  ->  it decides (tombstone => ENOENT);
                                           LOCAL is never consulted
migrator:
    sees TOMBSTONE(epoch >= e) for foo ->  does not copy LOCAL foo
eventually:
    old LOCAL foo is GC'd with the rest of the local range
```

A rename/move out of a not-yet-migrated entry likewise writes the
authoritative hashed overlay record (new name, or tombstone for the old
name) *before* the old local copy is allowed to disappear. Rule: during
`SPLITTING`, **the hashed side always wins over the local side**, for
records and tombstones alike.

### Directory timestamps must spread with the directory

Spreading the dentries is not enough on its own, and this is easy to miss:
POSIX says creating, removing or renaming an entry updates the **containing
directory's** mtime and ctime. If a create that lands on hash shard 77 then
has to bump the directory inode's mtime on shard 12, **every create in the
spread directory still funnels through shard 12** — the spread has been
defeated by a timestamp, and the hot-directory scaling claim is false. This
is precisely the failure mode P1 exists to catch.

The answer is the same shape as file write lanes ([data protocol](data.md)):

```text
LOCAL directory:
    dentry mutation and dir mtime/ctime are on the same shard already
    -> one Raft entry, nothing to do

HASHED directory:
    each dentry shard carries a dir_lane:
        { lane_seq, max_mtime, max_ctime }

    create/unlink/rename on hash shard S:
        { dentry mutation                     <- exclusive key
        ; MAX(dir_lane[S].max_mtime, now)     <- commutative reduction
        ; MAX(dir_lane[S].max_ctime, now) }   <- commutative reduction
        ONE Raft entry on S. The parent shard is not involved.

    stat(directory):
        mtime = MAX( inode.base_mtime, dir_lanes' max_mtime )
        ctime = MAX( inode.base_ctime, dir_lanes' max_ctime )
```

The lane set of a hashed directory is bounded the same way a file's is: the
directory's inode row carries the set of hash shards that have actually been
used, so `stat()` on a directory is a bounded collect and not a 4096-way
fanout. The reduction is validated by the same double-collect protocol as
file `stat()`, and the reductions are transaction payload rather than
exclusive keys ([transactions.md](transactions.md)) — two creates on
different hash shards must not conflict merely because both touch the
directory's time.

Explicit changes (`utimens`, `chmod`, `chown` on the directory itself) go to
the inode row's `base_*` values under `mtime_gen`, exactly as for files.

### RMDIR and RENAME are not uniformly cheap once a directory is HASHED

The operation→participant matrix in the spec gives the common case; the
spread layout changes two entries, and the difference is worth stating
explicitly because both are easy to get wrong.

**RMDIR must prove emptiness, and on a HASHED directory emptiness is
distributed.** For a LOCAL directory, "no entries" is a single range check on
one shard, so RMDIR is the ordinary two-shard transaction (parent dentry +
directory inode row). For a HASHED directory the entries are spread over the
directory's hash shards, and a naive per-shard check races: shard A can be
observed empty, then get a create, while shard B is being checked. So:

```text
RMDIR on LOCAL dir:   2 shards (parent dentry + inode row)
RMDIR on HASHED dir:  parent dentry + inode row
                      + a transactional read over the directory's dentry
                        shards, whose emptiness is part of the transaction's
                        read set (conditional PREPARE; any concurrent create
                        invalidates it and the RMDIR retries or fails
                        ENOTEMPTY)
```

The alternative — maintaining an exact distributed entry count — would
re-create a per-directory counter hotspot on the very directory that was
spread to avoid one. RMDIR is rare; paying an O(hash-shard-count)
transaction for it is the right trade, and the operation is bounded because
the used-shard set is recorded on the directory inode.

**Same-directory RENAME is not always single-shard.** In a LOCAL directory,
old and new name share the parent's shard, so a simple rename is one entry.
In a HASHED directory `hash(parent, old)` and `hash(parent, new)` are
independent, so the ordinary case is a **two-shard transaction**, widening
further if the destination exists and its inode row lives elsewhere.

### Directory rename must not create a cycle

POSIX forbids moving a directory beneath itself: the source may not be an
ancestor of the destination. Checking this by walking the tree at the moment
of the rename is **not sufficient under concurrency** — two concurrent
renames can each validate against a tree that was legal when they read it,
and together produce a detached cycle that no single operation ever
authorized:

```text
initially:   /a  and  /b  are siblings
rename(/a -> /b/a)   validates: b is not under a     OK
rename(/b -> /a/b)   validates: a is not under b     OK
both commit  ->  a and b are now each other's ancestor, unreachable
```

The fix is to make the **entire ancestry predicate part of the transaction's
read set**, so the two renames above are forced to conflict. Every directory
inode row carries an authoritative `parent_dir` pointer with a
`parent_version` that changes on every reparent:

```text
directory rename(src, dst):
  1. walk dst's ancestors to the root, collecting
     (dir_id, parent_version) for every link in the chain
  2. fail EINVAL if src appears anywhere in that chain
  3. include the whole collected chain in the conditional PREPARE as
     read-set entries at those versions
  4. any concurrent rename that reparents a directory in the chain bumps
     its parent_version -> our prepare fails its version check
  5. abort and retry
```

That makes the check a real concurrency-controlled predicate rather than an
advisory glance. The cost is an O(depth) multi-shard transaction — acceptable
precisely because directory rename is rare, and unavoidable if the invariant
"the namespace is a tree" (I8's structural half) is to hold under concurrent
renames. Renaming a *file* is unaffected: files have no children, so no
ancestry predicate exists.


## Appendix 8 — Protocol — sessions, open-unlinked, locking

*Source: `arch/protocols/sessions.md` (headers demoted, nav stripped).*

These four mechanisms share one foundation: a real client-session protocol.
It is specified here together with everything built on it.

### Client sessions and fencing

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

#### Revocation is a barrier, not an announcement

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

#### Duplicate suppression must be bounded state

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

### Open-unlinked inode lifetime

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

### Distributed POSIX locking

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


## Appendix 9 — Performance — multi-Raft runtime & hot-path contract

*Source: `arch/performance.md` (headers demoted, nav stripped).*

The topology in [the spec](#architecture) makes linear scaling
*possible*. This document is the contract that makes it *actual* — the
difference between "the architecture scales" and "the implementation
scales." Each item is a requirement, traceable to P1/P4, not a tuning
suggestion.

### The multi-Raft runtime

**4096 logical groups are not 4096 physical mini-databases.** A node must
never run per-group timers, WALs, fsyncs, sockets, heartbeats, or threads —
on a 3-node cluster that would be ~1365 leaders per node doing bookkeeping
instead of work. The implementation is a **multi-Raft runtime**: a few
reactor threads (per NUMA domain, P4) drive many shard state machines; Raft
messages are batched by destination, heartbeats coalesced, AppendEntries
pipelined, WAL writes group-committed across groups, and KV applies batched —
while each shard keeps its own independent logical ordering, terms, and
commit indices. This is the same logical/physical split as the applied-state
KV (§5 of the spec), and it is an architectural requirement, not an
optimization: without it the bookkeeping cost of 4096 groups consumes the
hardware the groups were meant to exploit.

### The hot-path implementation contract

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
  throughput. (This is also where degraded generations from
  [the data protocol](protocols/data.md) are re-striped.)
- **FUSE capabilities and client prefetch** are part of this contract —
  specified in §7.7 of the spec because they are also correctness-relevant.
- **Read-side metadata is fetched in windows, not per chunk.** A sequential
  read at 100 GB/s touches ~800k chunks/s; discovering each chunk's
  committed generation with an independent metadata RPC would make the
  metadata plane the ceiling even with a perfect data path. The lane
  structure makes the fix cheap: the client issues **batched range
  chunk-map fetches per lane** (lane i holds chunks i, i+64, i+128, …, so one
  range request per lane covers a contiguous file window), and the prefetch
  pipeline runs a *metadata window* ahead of the *data window* — chunk maps
  arrive before the data they describe is needed. Per-chunk lookups remain
  only for true random I/O.
- **Batching** of Raft messages, WAL group commit, KV applies, chunk
  publications, and read-authority rounds is specified where it lives
  (the multi-Raft runtime above, and the
  [data](protocols/data.md) / read protocols in the spec) and is binding:
  **no persistence boundary is paid per chunk, per metadata record or per
  Raft group when several operations can safely share one; durability
  boundaries are amortized to the largest batch the externally visible
  semantics allow.** The stronger-sounding claim "no NVMe sync per logical
  operation" would be a lie: efs defines a returned `write()` as durable, so
  *some* persistence boundary is crossed before it returns — an ordinary
  completed write on PLP media, or an explicit FUA/flush without it
  ([data protocol](protocols/data.md)). What batching removes is paying that
  boundary once per operation when one boundary could have covered
  thousands; it cannot remove the boundary itself.


## Appendix 10 — Development — modularity constraint

*Source: `arch/development.md` (headers demoted, nav stripped).*

> Looking for something to do rather than a principle to follow?
> [START-HERE.md](START-HERE.md) turns this page's bar into a task list: the
> current task, which pages govern a given change, and what "done" means.

Modularity is not a style preference here — it is what makes the two things
this project depends on possible at all: **fast isolated testing** (see
[verification.md](verification.md)) and **bounded-context change** (a human
or a model editing one component without ingesting the whole codebase). A
system this subtle cannot afford either to be slow to test or to require
global knowledge to change safely.

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
  lets the same compiled state machine run under the simulator
  ([verification.md](verification.md)) — purity buys testability and
  simulatability at once.
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

### The specification itself is under a machine gate

The same argument applies to the documents. A normative index plus satellites
is only safer than a monolith if the split cannot silently drift — otherwise
a contradiction between the index and a satellite is just a monolith with
extra steps, and the contributor who reads only one of them is misled. The
documentation gate is therefore part of the build, not an editorial habit:

```text
regenerate docs/architecture-full.md and fail on any diff
    -> the generated review artifact can never be stale

validate every internal link resolves
    -> the index-plus-satellite structure stays navigable

validate every invariant reference (I1..I25) names a defined invariant
    -> "preserves I21" cannot survive renumbering

validate every operation in the §6 matrix names an existing protocol section

reject a normative table defined in more than one place
    -> placement formulas, the op matrix and the invariant list have exactly
       one home; satellites explain them, never restate them
```

The last check is the one that matters most: duplicated normative tables are
how the index and a satellite come to disagree, and the rule "if they
disagree, the index wins" is a fallback, not a substitute for the tables
being single-sourced.

**Why it is in the architecture and not a style guide:** the migration
(§10 of the spec) lands Raft, KV, and the transaction protocol as *new*
components. If they are built to these boundaries from the start, the
simulator and the unit harness get them for free, and the dev cycle stays
fast as the system grows. If they are built as another 6000-line
`metadata.c`, no amount of testing infrastructure will save the cycle time.


## Appendix 11 — Verification — simulator & code→signal cycle

*Source: `arch/verification.md` (headers demoted, nav stripped).*

### The simulator (build first, architecture-independent)

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
you explore millions of interleavings overnight. The invariants of §4 of the
spec are checked after every simulated step, so a bug is a seed + a violated
invariant, not a log line on a live cluster.

**How it is built.** The metadata core is written to be **transport- and
storage-agnostic**: it sends messages and reads/writes disk through
interfaces. Two backends implement those interfaces: the real one (sockets +
NVMe, what ships) and the simulated one (a message queue + a fault-injecting
in-memory disk, what the simulator drives). The same compiled state machine
runs in both, so "passes in simulation" is meaningful. (This is also why
[development.md](development.md) makes state-machine purity an architectural
rule.)

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
session fence at every point of a client's outstanding work
fence ACK lost / touched shard unreachable mid-revocation
ino reuse: delayed PUT from the previous inode incarnation
concurrent directory renames validating overlapping ancestries
create racing an rmdir emptiness check on a hashed directory
silent fragment corruption (payload and identity)
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
rebuild from fragments · fragment silently corrupted
publication with evidence from an obsolete placement/coding profile
```

A "fragment" in the simulator is an abstract durable object with an identity,
a checksum and a home, not bytes on an RNIC. With those events the checker can
verify that no committed read ever reconstructs from mixed generations (I13),
that nothing is published before durability (I14), that orphans never become
visible (I15/I20), that a corrupt fragment is never accepted as
reconstruction input (I25), that a publication whose durability evidence does
not match the current placement is rejected (§7.3), and that f simultaneous
losses never lose a published chunk (I11) — under every crash/interleaving
the generator can produce.

The corruption fault is the reason I25 exists as an invariant rather than an
implementation habit: it is only ever *tested* if the simulator can flip bits
in a durable object, and an EC decoder without integrity checking fails that
test by producing confidently wrong data rather than an error.

### Shortening the code → signal cycle

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
- **Bounded-context change.** Modularity
  ([development.md](development.md)) keeps the unit of work small: a change
  loads one module + its interface header, not the whole tree. This is what
  makes both fast isolated tests and model-assisted editing tractable.
- **Invariants as executable checks** (simulator assertions + `fsck`), not
  prose — so "is this a bug" is decidable without a human reading a log.


## Appendix 12 — Design history (review rounds)

*Source: `arch/design-history.md` (headers demoted, nav stripped).*

The normative specification is [../architecture.md](#architecture). It
states rules without narrating their discovery. This note records how it got
here — the review rounds and the mistakes they caught — so the reasoning
stays available without cluttering the spec.

### What was replaced

The previous metadata layer was a whole-table snapshot with copy-on-write
pages and a root two-phase commit. Its commit point was an
under-replicated coordinator/root generation, and the bug history of the
old system — a dual-writer root generation, peers pinned at an old committed
generation, roots adopted before their pages were local — was the system
repeatedly discovering that the commit point was not quorum-replicated.
That mechanism is deleted, not patched. The data-path mechanism
(client-direct RDMA, k+f EC) is unchanged; its commit semantics are what
the new architecture specifies.

### Sep 1 2026 — six external review rounds

**Round 1 — specification blockers.** `shard_bits = 20` was an arithmetic
error (2³² objects / 4096 shards wants 12 bits). Inode IDs became 64-bit +
generation (ABA-safe handles). The dentry index became a small *projection*
`(ino, gen, type)` with the inode row as the only mutable copy. Cross-shard
rename/hardlink were shown to need a real Raft-backed transaction — the "no
2PC" claim was only ever about the old root-snapshot 2PC. I1–I4 were
restated as Raft's actual safety properties (election uniqueness, leader
completeness, term fencing, quorum acknowledgement) instead of "at most one
node believes it is leader." Reads became leader + ReadIndex with no clock
leases. Reconfiguration became joint consensus. Request idempotency via
op-IDs was added.

**Round 2 — POSIX composition.** Three (later four) governing principles
were written down (P1 never-serialize, P2 co-locate-what-commits-atomically,
P3 make-stale-harmless). The CREATE co-location rule was made explicit
(without it every CREATE is a distributed transaction). Hot-directory spread
became a layout-epoch protocol instead of a flag. Size became sharded
high-water lanes co-located with the chunk shard (I21). stat() got the
content-epoch double-check; truncate became a content-epoch bump (I22).
O_APPEND became reserve-then-distributed-write. Locking got one lock
authority per inode. File data became direct-I/O by decision. The simulator
was scoped to the *logical* data protocol. Data targets were made dumb.

**Round 3 — the hot-path contract.** EC publication was fixed to require
**all** k+f durable fragment ACKs before publication — the earlier "≥ 2 of
3 ACKs" rule had a real durability hole at f=1 (publish with 2 durable
fragments, lose 1 → 1 fragment < k=2 → acknowledged data lost). The FUSE
layer was named as an architectural serializer risk (parallel-direct-writes
et al. became requirements). Direct-I/O's loss of kernel readahead produced
the client prefetch requirement. Size lanes were bounded to L per file (a
4096-way fanout would have made stat() absurd). The multi-Raft runtime,
publication batching, the declared small-write envelope, QoS isolation, and
rate-limited distributed rebuild all entered the spec. "Durable ACK" was
defined as a persistent-NVMe operation, not RDMA completion.

**Round 4 — configurable failure tolerance.** f became a configured target
(1–3). The automatic guarantee was set at N ≥ max(2f+1, k+f). The user's
ratified 4-node/f=2 and 6-node/f=3 configurations survived only as an
explicitly labeled N=2f "durability-only mode" with an operator-gated
survivor-log-merge disaster-recovery protocol.

**Round 5 — structural fixes.** MKDIR was changed to scatter (files
co-locate with the parent directory; every new directory hashes onto its own
shard, buying an independently scalable subtree for one 2-shard transaction).
stat() became a double collect over monotonic `lane_seq` numbers (the epoch
check alone could assemble a size the file never had). Multi-chunk write
atomicity flipped: syscall-level atomic publication became the default
(I24), per-chunk visibility an explicit relaxed mode. O_APPEND gained the
serialized visible commit frontier. Client sessions became a real protocol
(I23).

**Round 6 — this revision.** The N=2f survivor-log merge was shown unsound:
selecting the highest-term entry independently per log index can synthesize
a command sequence no leader ever authorized and drop a *committed* entry
(commitment information dies with the lost majority). The mode was deleted
outright — N ≥ max(2f+1, k+f), RF = 2f+1, full stop. The FUSE
syscall-boundary problem (async-DIO splits a syscall without identifying
it) became an explicit unresolved contract with a MUST-NOT-claim, instead
of a claim that papered over the kernel interface. Write-publication
transactions got hash(txid) coordinator dispersion and parallel no-wait
PREPARE (they are on the hot path now). Directory spreading gained the
dominating hashed tombstone (no resurrection during SPLITTING, I8).
Session fencing was narrowed to metadata authorities — data targets do not
check sessions (orphan fragment PUTs are harmless by P3). Open-unlinked
gained the session-scoped open lease. Chunk identity gained
`coding_profile_id` for online f/k changes. The lane assignment became
round-robin within a file (`(chunk_index + hash(ino)) % L`) so sequential
read windows are one range request per lane. And the specification itself
was split: this file plus the satellites, leaving
[../architecture.md](#architecture) as the normative index.

**Round 7 — protocol closure.** The structure was accepted; this round only
closed remaining protocol holes, and every one of them was a place where an
earlier statement was true-sounding but did not compose.

*Session fencing was not actually a protocol.* "Leaders cache the session
table and re-validate on doubt" fences nothing: a shard that has not heard
about `epoch+1`, acting on its own cached view, can still commit for the
dead client — a direct I23 violation, and nothing ever tells that leader to
be in doubt. Replaced by a real revocation barrier: the session record
carries a touched-shard set, and `ACTIVE(E) → FENCING(E+1) → ACTIVE(E+1)`
only completes after every touched shard has durably acknowledged the fence.
Registration is once per shard per session, so the I/O path is untouched.

*ABA protection did not reach the data plane.* Handles were
`(ino, generation)` but chunk objects were keyed by bare `ino`, so a delayed
fragment PUT from the previous occupant of an ino could land in the new
object's key space. Every data-plane key is now scoped by
`FileID = (ino, inode_generation)`.

*Successor generation numbering was unsound.* Two concurrent writers reading
committed `G` would both mint "`G+1`" for different bytes and PUT different
content under one object identity — destroying the immutability everything
else rests on. Generations are now globally unique candidate identities, with
ordering supplied by the publication CAS instead of by the name.

*But the reviewer's companion suggestion — put `content_epoch` in the chunk
key — was wrong, and the spec already had that bug.* With the epoch in the
object identity, `truncate()` to a smaller non-zero size would strand every
surviving chunk under an epoch no reader consults (and the text said those
were GC'd), while re-tagging them instead turns one truncate into an
O(file-size) rewrite. POSIX requires the prefix to survive. The epoch is now
strictly a fence over lane state and in-flight publications; committed chunk
data survives a bump. That change exposed a second gap: with lanes
epoch-invalidated, `stat()` had nothing to read after a truncate, so the
inode row gained `base_size` alongside `base_mtime`.

*A derived `L` could never change.* `lane = f(chunk_index) % L` means
changing L relocates every chunk-map key of the file. Replaced by a fixed
64-lane space activated on use: an odd-stride permutation guarantees 64
*distinct* shards (independent hashing collides), lane 0 is the inode's own
shard so small files have no fan-out, and a monotonic `active_lanes` bitmap
bounds what `stat()` collects — at most 64 inode interactions over a file's
entire lifetime.

*The lane was about to become the next hotspot.* If a transaction's `MAX` on
lane size/mtime were an exclusive intent key, two writers publishing
*different* chunks that share a lane would conflict on nothing — the
per-file serializer moved down one level rather than removed. Transactions
now distinguish exclusive CAS keys from commutative reductions carried as
payload. Intent resolution also gained its fourth outcome: *cannot establish
authority* is a retryable error, never "absent" (I9 again).

*Timestamps had two hidden serializers.* A write updates ctime as well as
mtime, so routing write-ctime to the inode row would send every hot-file
writer back to the inode leader — write ctime moved into the lanes. Worse,
POSIX updates the **containing directory's** mtime/ctime on every entry
create/remove, so a spread directory would still funnel every create through
its home shard: hashed directories now keep per-dentry-shard `dir_lane`
times. And atime stopped being unspecified — `noatime` is the default,
`relatime` an option, strict per-read atime deliberately not offered.

*Two namespace cases were hiding behind "2 shards."* `rmdir` must prove
emptiness, which is distributed once a directory is hashed (now a
transactional read set, not a counter, which would rebuild the hotspot), and
same-directory `rename` is two-shard in a hashed directory because the two
names hash independently. Separately, directory rename's cycle check is
unsound if it merely walks the tree: two concurrent renames can each validate
a legal tree and together create an unreachable cycle. The destination's
whole ancestry now enters the transaction's versioned read set.

*Three hygiene items at scale.* Duplicate-suppression state became a bounded
window (watermark + completion bitmap + reply cache) instead of a permanent
row per operation, with a stated GC condition — as did transaction decision
records. Because targets are deliberately dumb, publication became the
validator of durability evidence against the *current* coding profile and
placement, so a stale client that durably wrote a full stripe to an obsolete
placement cannot publish it. And end-to-end fragment checksums were added
(I25): media corruption is not Byzantine behavior, and EC without integrity
checking will reconstruct confidently from a corrupt fragment.

*One performance claim was simply too strong.* "No physical NVMe sync per
logical operation on any hot path" contradicts "a returned `write()` is
durable" — some persistence boundary must be crossed. The contract now says
what was actually meant: boundaries are amortized to the largest batch the
externally visible semantics allow, never paid per chunk, per record, or per
Raft group when one could cover many.

