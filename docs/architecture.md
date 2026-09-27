# Architecture

**New here, or looking for the next task? → [START-HERE](arch/START-HERE.md)**
(what to work on now, which pages govern a given change, what "done" means).

[Browser view (generated)](architecture.html) ·
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
review (nine rounds — see [design history](arch/design-history.md)).** The
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
| Exports | **one per cluster** — hardcoded name `efs` (below) |

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
  Publication (`fsync`/`close`/`O_SYNC`, §7.3) pays a synchronous round
  trip to *device*; that is affordable because the device is µs-scale
  flash, not ms-scale disk. A plain `write()` does not.
- **No SMR/shingled, no rotational-latency hiding, no track alignment** —
  entire problem classes deleted, not engineered around.

If a deployment needs HDDs, that is a different filesystem; efs's scaling
claims (§9) are made against flash and do not transfer.

**One export per cluster, by decision (Sep 4 2026).** A cluster hosts
exactly one filesystem tree, named `efs`; the mount target is
`cluster:port:efs` and there is no create-export operation. This is a
scoping decision in the same sense as the hardware envelope: the old
system's multiple named exports were a testing convenience, never a
production need. The escape hatch is preserved by construction: if a
second filesystem is ever required, it is **one engine (KV + Raft groups)
per export, side by side — never an `export_id` in keys** — so the
single-export key format is not a retrofit trap. The mkfs-chosen `salt`
(§7.4) survives as a per-cluster value.

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

**How to describe efs, precisely.** It is a *high-performance parallel
filesystem targeting Linux/POSIX semantics, with explicitly documented
deviations* — not "a POSIX filesystem" full stop. Three deviations are known
and deliberate, and all are stated in this section rather than discovered
by a user: strictly-conforming per-read `atime` is **not offered** (§7.3);
full syscall-level write atomicity **above the FUSE request boundary**
is an unresolved kernel-interface problem that efs does not claim (below);
and a returned `write()` is **not** durable or cross-client visible until
`fsync`, `close`, or `O_SYNC`/`-o sync` (below). Everything else in this
section is a promise. If a deviation is ever added, it belongs here, in
this list, before it ships.

- **Single-shard metadata operations are linearizable** within the
  authoritative shard's Raft group.
- **Multi-shard metadata operations are atomic** across their participant
  shards according to the cross-shard transaction protocol (§7.2). The
  intended property for those is strict serializability of transactions.
- There is **no global ordering** between two independent operations on
  unrelated shards, and none is needed.
- **Data:** a returned `write()` is buffered in the client. It is
  durable and visible to every client only after `fsync`, last `close`,
  or an `O_SYNC`/`O_DSYNC`/`-o sync` write-through (the §7.3 publication
  machine). Same-client read-your-writes hold via the dcache. This is
  POSIX and every production PFS; the stronger "every `write()` publishes"
  alternative was measured and rejected (START-HERE W2:
  peer sees 0/10 un-`fsync`ed bytes; `kill -9` of `efs-fuse` loses a
  64 MiB acknowledged `write()`). `O_SYNC` is specified; it is not
  wired yet.
- **One `write()`/`pwrite()` publishes atomically.** POSIX makes regular-file
  `read()`/`write()` effects atomic with respect to one another, so a
  concurrent reader never observes a mix of old and new chunks from a single
  in-flight write: publication is one atomic decision covering every chunk
  the call touched (§7.3), while the *data movement* stays fully parallel.
  The requirement is symmetric — POSIX makes those effects atomic with
  respect to *each other* — so a multi-chunk **read** validates the
  chunk-map versions it used and retries if they moved under it, rather than
  returning a splice of old and new (§7.3).
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
  failure surfaces as a **distinct non-`ENOENT` error** instead — usually
  retryable, though some (a fragment that fails its integrity check with no
  repair source) are legitimately terminal `EIO`. What is forbidden is the
  substitution, not the finality. **This is a safety invariant, not an
  error-reporting nicety:** a false "absent" answer to a
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
- **I24 · atomic write publication *and* atomic observation.** Within the
  atomicity unit (one FUSE request, ≤ `max_write`; the syscall-boundary
  problem above that size is explicitly open, §7.3): once publication
  returns (`fsync`/`close`/`O_SYNC`), that write is visible in its entirety
  to every subsequent read, and a concurrent read sees either all or none
  of that call's chunks (default mode; §7.3). **The obligation is symmetric**
  — a multi-chunk read must also return a state some serialization of the
  concurrent writes actually produced, so it validates the chunk-map
  versions it read; an atomic publication decision alone does not prevent a
  slow reader from splicing old and new chunks.

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
- **L7.** EC generations that no chunk map references eventually reclaim —
  not only the never-published ones (a losing CAS candidate, a fenced
  client's orphan), but also generations that *were* published and are no
  longer reachable: everything a `truncate` range-deleted, and every
  old-profile generation superseded by a re-stripe (§7.3, failure
  tolerance).
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
| dentry, HASHED layout | the directory's 64-shard permutation, `dir_lane = hash(name) % 64` (§7.4) | `(parent_ino, name)` → `(ino, gen, type)` | shard Raft |
| chunk map | lane shard (§7.3 permutation) | `(FileID, lane, chunk_index)` | shard Raft |
| size / write mtime / write ctime | write lane, co-located with the chunk's lane shard | `(FileID, lane)` | shard Raft |
| active-lane bitmap, `base_size`/`base_mtime`/`base_ctime`, `content_epoch`, `mtime_gen` | `inode_shard(ino)` (inode row) | `(ino)` | shard Raft |
| directory mtime / ctime, HASHED layout | each of the directory's dentry shards | `(dir_ino, dir_lane)` | shard Raft |
| directory used-shard bitmap, `dir_mtime_gen`, LOCAL `nents` | `inode_shard(dir_ino)` (dir inode row) | `(dir_ino)` | shard Raft |
| `dentry_seq` (per directory, per dentry shard — the predicate guard key for emptiness, §7.2) | that dentry shard | `(dir_ino, dir_lane)` | shard Raft |
| current coding profile + `profile_epoch`, per-node availability state | control-plane group, **installed into every publication authority** (§7.3) | — | control Raft + per-shard installed config |
| client session, touched-shard set | `hash(client_uuid) & 0xFFF` | `(client_uuid)` | shard Raft |
| open lease | `inode_shard(ino)` | `(FileID, session_id)` | shard Raft |
| POSIX locks | `inode_shard(ino)` | `(FileID, start, end, owner)` | shard Raft |
| append reservations, `reservation_watermark` | `inode_shard(ino)` | `(FileID)` | shard Raft |
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

**Log truncation is a chunked InstallSnapshot of a snapshot file.** The
snapshot point is a memtable flush plus `save_snap` at the applied index.
The bytes are that group's export of the segment view pinned in the same
cycle, written off the apply path to one file per group
(`mdraft/snap-<group>-<index>.kvx`). The Raft message carries
`lastIncludedIndex`, `lastIncludedTerm`, `offset`, `data[]`, and `done`.
A follower stages `mdraft/snap-<group>-<index>.part` and imports it only
when `done` is set. One chunk is in flight per peer. A snapshot never
advances past the KV's durable point, and the leader does not hold the
image in a RAM blob.

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
| RMDIR (HASHED dir) | as above **+ emptiness across the directory's dentry shards** | 2 + used dir lanes (≤64) | transaction with the emptiness check in its read set (§7.4) |
| UNLINK (last link) | dentry + inode row | **1** if `dentry_shard == inode_shard` (and nlink==1), else 2 | single Raft entry / transaction |
| UNLINK (nlink>1) | dentry on parent shard, nlink on inode shard | 2 | transaction (§7.2) |
| LOOKUP (name only) | dentry | 1 | single read |
| GETATTR (stat) | inode row **+ the file's active write lanes** (size/mtime/ctime merge, §7.3) | 1 + active lanes (1 for a small file, ≤64) | double-collect (§7.3) |
| GETATTR (stat) on a HASHED directory | dir inode row + the directory's used dentry shards (mtime/ctime merge, §7.4) | 1 + used dir lanes (≤64) | double-collect (§7.4) |
| SETATTR (mode/owner) | inode row (ctime) | 1 | single Raft entry |
| SETATTR (`utimens`) | inode row `base_*`/`mtime_gen` + the file's active lanes | 1 + active lanes (≤65) | **inode fence** (§7.3) — it can set mtime *backwards* |
| SETATTR (size) | — | — | this is TRUNCATE; see that row |
| LINK (hardlink) | new dentry on its parent shard, nlink on inode shard | 2 | transaction (§7.2) |
| RENAME same-dir (LOCAL dir) | two dentries on parent shard | **1** in the simple no-replacement case; replacing a destination whose inode lives on another shard widens the participant set | single Raft entry / transaction (§7.2) |
| RENAME same-dir (HASHED dir) | two dentries on `hash(parent, old)` and `hash(parent, new)` — independent shards | ≥2 | transaction (§7.2) |
| RENAME cross-dir (file) | src dentry, dst dentry, ino row, parent nlinks | ≥2 | transaction (§7.2) |
| RENAME cross-dir (directory) | as above **+ the destination's whole ancestry chain as a versioned read set** (cycle prevention, §7.4) | ≥2 + O(depth) | transaction (§7.2) |
| WRITE non-extending | chunk-map entry | 1 (lane shard) | single Raft entry |
| WRITE extending | chunk-map entry + that lane's size/mtime/ctime marks | **1** (co-located, §7.3) | single Raft entry |
| WRITE first use of a lane | as above + `active_lanes` bit on the inode row | 2 | transaction (§7.2); ≤64 times per file, ever |
| O_APPEND | EOF reserve (validated against the active-lane EOF vector, and holding an **EOF barrier** on those lanes until the reservation resolves) + chunk + size | inode shard + active lanes, then lane shards | reserve (§7.3) then ordinary write |
| TRUNCATE | `content_epoch` + `base_size` + times on inode row, **+ each active lane's fence and range delete, + the tail chunk's publication when `size` is not chunk-aligned** | 1 + active lanes (≤65) | **inode fence**: one transaction (§7.2, §7.3) |
| READ spanning several chunks | chunk maps on the covering lanes | lanes covering the range | validated collect (§7.3) — I24 has a read side |
| READDIR | dentries | 1 (normal) / used dir lanes (spread, ≤64) | range scan / scatter-merge |
| CHMOD / CHOWN | inode row (ctime, **not** mtime) | 1 | single Raft entry |
| LOCK (`fcntl` / `flock` / `F_GETLK`) | lock records | 1 (inode shard) | single Raft entry per grant/release; blocking waits held at the authority (§7.6) |

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
times, or a hashed directory's times). Reductions are transaction *payload*
and never block a prepare — otherwise two writers publishing different
chunks that share a lane would conflict on nothing, and the per-file hotspot
the lanes exist to remove would reappear one level down (P1). But **"not a
lock" must not mean "not visible":** a reduction is effective at its
transaction's *decision*, which can precede the reducer materializing it, so
pending reduction intents are stored discoverably under the lane's own key
prefix and an authoritative lane read is `MAX(materialized marks, committed
pending reductions)`. Materialization is background compaction of
already-visible state.

**Read/predicate guards are the third primitive**, and several protocols are
unsound without them: an observation a transaction depends on must stay true
*through its decision*, not merely until its last check. A guard is durable,
**shared** (guards coexist), and conflicts with any mutation that would
change the guarded state. `O_APPEND` guards the active lanes' `lane_seq`;
directory rename guards each ancestor's `parent_version`; `stat()`/read
fallbacks guard the versions they collected. `RMDIR` is the case that proves
guards cannot be replaced by version checks: emptiness is a statement about
keys that **do not exist**, so an insert into an observed-empty shard is a
**phantom** no version check can see. Each directory therefore carries a
`dentry_seq` per dentry shard, bumped by every dentry mutation including
inserts, and `RMDIR` guards those — predicate isolation without MVCC.

**The parent directory row is a reduction target, not an exclusive key.**
Every namespace op touches its parent's `nlink`/`nents`/`mtime`/`ctime` and
the dentry shard's `dentry_seq`; if those were CAS'd as a full row image,
every op in one directory would serialize on that one row (the hotspot P1
forbids), and — since the same-group log path applies without a version
CAS — a transaction that read the row before a log-path apply would
overwrite it (Sep 20 2026: `mdtest` left a parent at `nlink=2` with three
live children, every further `rmdir` an error forever). So a transaction
carries a **signed inode delta** (`nlink ±`, `nents ±`, times `MAX`,
`used_shards OR`, `parent SET`) and a **`+1` on the dentry_seq witness**,
both folded into whatever the row/witness holds at RESOLVE, commuting with
each other and with the log path. What keeps it sound: a pending reduction
and an exclusive intent on the same key are mutually **BUSY** (an `RMDIR`'s
DEL of the child row cannot pass a pending `LINK`'s `nlink+1`, nor the
reverse), the log path probes for pending intents/reductions before an
unversioned DEL and returns BUSY, every fold bumps the key's version so a
stale EXCL lands STALE, and `dentry_seq` guards compare the **value**
observed at read time (a log-path bump is unversioned but changes the
value), which makes `RMDIR`'s emptiness guard STALE exactly when a child
appeared. Directory ops thus retry only on real conflicts (same name, same
child, emptiness violated), never on "someone else touched the parent".

A transaction is visible **at its decision**, not when cleanup runs; a
reader meeting an intent resolves COMMIT → new value, ABORT → old value,
NO-DECISION → old value (the read linearizes before the eventual decision),
and **cannot-establish-authority → a retryable error, never "absent"** (I9).
**A reader that resolved an intent as NO-DECISION must re-check that
decision before returning** — intents exist from PREPARE, so a read spanning
a commit can mix two states of one transaction while every key version it
checks is unchanged. Decisions are final, so only the undecided ones need
re-checking (§7.3).
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
(flush/FUA or PLP media), not an RDMA completion. That machine runs at
`fsync`, last `close`, or `O_SYNC`/`O_DSYNC`/`-o sync` — not at a plain
`write()`. A returned `write()` is POSIX-buffered (client dcache);
cross-client visibility and crash durability begin at publication
(§3). Degraded publication (with `u` domains already
unavailable) needs ≥ `k+(f−u)` fragments, is marked degraded, and is
re-striped on repair — never for a merely *slow* target, and only against
domains the **control plane has committed as unavailable**, since a client
cannot distinguish slow from failed (I11). `u` is **protection debt, not a
headcount**: a degraded generation consumes budget until its missing
fragments are actually reconstructed, because a returning node restores
capacity, not the fragments it never held.

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
inode leader. Explicit changes update the inode row's `base_*`, keeping
POSIX's distinction exactly (`chmod` sets ctime, not mtime). Only `utimens`
can move a timestamp **backwards**, so only `utimens` bumps `mtime_gen` and
only lane *mtimes* are generation-guarded; every other time source is a
monotone "now" and plain `MAX` is correct for it — guarding ctime would
create a backwards-moving ctime rather than prevent one. That monotonicity
is **enforced, not assumed**: `CLOCK_REALTIME` can step backwards, so
implicit time updates are clamped by `MAX` — a documented HPC-suited choice,
not bit-exact wall-clock POSIX. **atime is
`noatime` by default** (reads do not update atime at all), `relatime` a
coalesced mount option; strict per-read atime is deliberately not offered.

**Truncate** bumps the content epoch and stamps `base_size` — and because
lane leaders must reject stale-epoch publications while ordinary writes
never consult the inode shard, the epoch is **pushed by a bounded fence over
the inode row plus the active lanes** (≤65 authorities; rare ops pay, P2).
The same fence carries a **per-lane range delete of the chunk-map entries
beyond the new size**: retaining them would let a later sub-chunk RMW take a
pre-truncate generation as its base and resurrect truncated bytes. Because
the KV is ordered and a lane holds `i, i+64, i+128, …`, that is one range
delete per lane, not one per chunk. It **does not invalidate committed chunk
data below the new size** — the surviving prefix stays readable, as POSIX
requires (I22). When the new size falls **inside** a chunk, that tail chunk's
zero-filled rewrite is prepared first and **CAS-published inside the same
transaction**: a size without its matching tail would leave the file
nominally shorter while the old bytes past the new end remain readable, and
re-extending must read as zeros. The same fence distributes `mtime_gen`.

**O_APPEND** reserves EOF on the inode shard, but the reservation is
validated against the file's **real** EOF — the active-lane vector under the
`stat()` read-set — never against a private counter, which an ordinary
extending `pwrite` would never advance. Validating once is not enough
either: POSIX makes "determine EOF" and "write" one atomic step, so while
reservations are unresolved an **append barrier** on the active lanes (the
same bounded fence, taken once per append *burst*) requires any publication
that would push EOF past the reservation watermark to order against them.
Non-extending and within-watermark publications — including the appenders'
own data — are untouched, so ordinary writers pay nothing. The reserved
length is the length the client will attempt (it holds the whole request
buffer before reserving), so a short append arises only from failure. Every
reservation resolves: COMPLETED, ABORTED_HOLE (a live client's failed
append, which it commits itself), or FENCED_HOLE (session fencing, the
backstop). The visible frontier advances over contiguous *resolved*
reservations; data movement is parallel. **Sub-chunk writes** use generation
CAS; disjoint ranges both land (I12), and pay RMW amplification by design.

**One write syscall publishes atomically** via a §7.2 transaction (I24) —
and I24 has a **read side too**: an atomic write decision does not stop a
reader that fetches chunk A, waits, then fetches chunk B from splicing old
and new. A multi-chunk read is a **validated collect** (collect chunk-map
versions → fetch in parallel → revalidate versions **and any undecided
transaction decisions it resolved** → retry, bounded, then a read-only
transaction), the same shape as `stat()`. Read prefetch is therefore
prefetch and not a cache: a prefetched chunk map is usable without
revalidation only inside the read whose linearization interval covers it.
The atomicity unit is **one FUSE write request**, and **EFS MUST NOT claim
full POSIX write atomicity above the `max_write` boundary** until FUSE
async-DIO syscall splitting is solved — an explicit unresolved contract. A
documented relaxed mount mode publishes per-chunk (§3).

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
same double-collect protocol as file stat. For that reduction to be bounded,
**a spread directory uses a fixed 64-shard permutation**, the same
construction as file lanes (`dir_lane = hash(name) % 64`, odd stride from
the home shard ⇒ 64 distinct shards): hashing over all 4096 would make
`stat(dir)` a 4096-way collect. First use of a lane registers it in the
directory's 64-bit used-shard bitmap — the one time the parent shard *is*
involved, ≤64 times per directory ever. Directory `utimens` carries
`dir_mtime_gen` through the same bounded fence a file's does, for the same
backwards-time reason.

**Two operations are not uniformly cheap once a directory is HASHED.**
`rmdir` must prove emptiness, which is distributed *and* is a statement
about entries that do not exist — so it takes **read/predicate guards**
(§7.2) on each dentry shard's `dentry_seq`, not version checks on the keys
it read: an insert into an observed-empty shard creates a key that was never
there to check (the phantom), and only a witness that every mutation bumps
can conflict with it. An exact distributed entry counter is rejected — it
rebuilds the hotspot the spread removed. Same-directory `rename` is a
two-shard transaction, since the old and new names hash to independent
lanes.

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

Open lifetime is **one session-scoped open lease per (FileID, session)** on
the inode shard: committed at the session's first open of the inode, removed
at its last close, lazily dropped when the session is fenced. Reclaim
requires `nlink == 0 AND open_sessions == empty` (I19, L6). Cost: one Raft
entry at each end of a session's use of a file — not per descriptor, not
per I/O.

Locks: **one lock authority per inode** on `inode_shard(ino)`, records
`(FileID, start, end, owner)` mutated via Raft — a semantic serialization
(P1), accepted because a locking application asked for coordination. efs
reproduces the **Linux** contract, which is **two conflict domains, not
three lock namespaces**: classic `fcntl` and **OFD `fcntl`**
(POSIX.1-2024, included) share one byte-range record-lock domain and
**do conflict with each other**, even within one process on one descriptor;
`flock` is a separate domain (not POSIX; its independence is Linux
behavior). Ownership is the orthogonal axis: classic `fcntl` locks are owned
by the process (`client_uuid, session_epoch, process_id`), while OFD `fcntl`
*and* `flock` locks are owned by the **open file description**
(`client_uuid, session_epoch, open_description_id`) and are therefore shared
by duplicated and inherited descriptors. Fenced sessions' locks are
reclaimed in both domains. Advisory locks never fence the data path.

**Blocking waits are held at the authority, granted in FIFO order.** A
blocking request (`F_SETLKW`, blocking `flock`) is a long-lived RPC the
leader answers only on grant or failure — the grant is the reply; no
polling, no timers, and the wait queue is leader memory, not Raft state (a
wait has no durability value; the client can always re-ask). Leader
failover fails the held request and the client re-issues it under the same
op-id, which idempotency (I16) resolves against a grant the old leader may
have committed first; a session fenced while waiting is dequeued by the
revocation barrier and never granted. The authority implements the full
POSIX range algebra (partial-unlock split, adjacent merge, in-place type
conversion, `F_GETLK` as a leader read) and caps records per inode
(`ENOLCK`). **Deadlock detection is same-inode only:** the authority holds
the complete wait-for graph for its inode, so those cycles fail `EDEADLK`;
cross-inode cycles are not detected — POSIX makes `EDEADLK` a *may*, Linux
checks only classic `fcntl` even locally, and no blocked wait is ever
stuck, because signals interrupt it and fencing tears it down.

**Inode-scoped ephemeral state is keyed by `FileID`, not `ino`** — leases,
locks and append reservations alike — so a delayed CLOSE or UNLOCK naming a
previous occupant of a reused ino cannot release a live file's state. Any
operation acting through an inode handle validates the inode generation
before mutating; a mismatch is a stale-handle error, never a silent no-op.

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

**Second unresolved kernel-interface item: upstream Linux serializes some
operations above efs, per mount.** These are not efs serialization points
and they are not cluster-wide — 64 clients on 64 nodes are unaffected — but
64 ranks *on one node sharing one mount* can serialize in their own kernel
before a single request reaches efs:

| Kernel behavior | Consequence for one mount |
|---|---|
| A direct write that **extends `i_size`** still takes the inode lock exclusively, even with `FOPEN_PARALLEL_DIRECT_WRITES` | concurrent extending writers to one file serialize |
| `IOCB_APPEND` forces the exclusive inode lock | `O_APPEND` writers to one file serialize |
| `O_CREAT` takes the parent directory inode exclusively; `FUSE_CAP_PARALLEL_DIROPS` covers **lookup/readdir only** | same-directory creates serialize |

Honesty about this belongs in the scaling envelope, not in a footnote: a
shared-file workload that **pre-sizes** the file turns extending writes into
non-extending ones and avoids the first row entirely, which is already the
recommended HPC pattern — but that is a workaround, and the other two rows
have none today. Either efs eventually carries a kernel/FUSE change, or §9
states plainly that current upstream FUSE bounds *intra-mount* scaling for
these three cases. It is tracked as an open interface problem alongside the
syscall-splitting one, not silently absorbed into "efs scales".

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
could still be retried. **Reclamation is driven by acknowledgement, not by
elapsed time** — the failure model allows unbounded delay, so no timer can
prove a straggler will not arrive; the client reports the **highest
*contiguous* acknowledged reply** and records at or below it may be
released, with later duplicates answered as already-completed. Contiguity is
required because replies arrive out of order — "highest reply received"
would let reply 100 authorize discarding 99's result while 99 is still being
retried. The retained *reply* matters, not just a completion bit: a retried
`O_APPEND` reservation must recover **the same offset**, or a lost reply
becomes a permanent hole. A **live** session's records are reclaimed at or
below its watermark; a **fenced** session's are dropped wholesale once the
revocation barrier completes (§7.5) — a dead client can never advance a
watermark, and after the barrier its old-epoch requests are rejected by
epoch alone, so no per-request state is needed to reject them. Transaction
decision records have the matching participant-ACK condition (§7.2).

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
  eliminated, because a publication (`fsync`/`close`/`O_SYNC`, §7.3)
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

Every row above scales **per client node**. Three of them are additionally
bounded *within a single mount* by upstream Linux, not by efs: concurrent
extending direct writes to one file, `O_APPEND` on one file, and
same-directory `O_CREAT` all take an exclusive kernel lock before efs is
called (§7.7). Many ranks on many nodes are unaffected; many ranks on **one**
node sharing one mount are not. Pre-sizing a shared file removes the first
case. This is a stated limit of the current kernel interface, not a claim
efs makes and then quietly misses.

A scaling benchmark is then a scientific question — *where did it stop: NIC,
NVMe, memory bandwidth, EC compute, fabric bisection?* — and any answer that
is a mutex, one leader, one thread, one WAL, or one coordinator is by
definition an EFS bug (§1). FUSE/VFS serialization is the one exception, and
only for the three cases named above: it is a known kernel-interface limit
with its own open item, so a benchmark that stops there must be reported as
that, never as "efs scaled".

## 10. Implementation order

Not a big-bang rewrite. Order chosen so each step is gated and the system
stays runnable. **Do not** go straight `KV → Raft → done`.

```text
0. Freeze the exact metadata placement model:
   inode_shard() · dentry_shard() · authoritative row ownership ·
   inode IDs + generations · FileID scoping of every data key AND every
   inode-scoped ephemeral record — leases, locks, reservations (§7.3, §7.6) ·
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
10.5 Durable backends, then production adoption: an on-disk ordered KV and
    an on-disk Raft log behind the step 3/4 interfaces, then the applied SM
    gated in-sim, then `efsd` adopts the engine for the cluster's single
    export (`efs`, §1) — reads first behind a flag, writes after (not a
    cutover of the live table until the new path is proven).
11. Delete the old snapshot / root-2PC machinery.
12. FUSE cache-coherence optimization only after zero/stale-cache semantics
    are demonstrably correct (data path starts as direct-I/O, §7.7).
```

**Why 10.5 exists.** Steps 3–5 built the KV and Raft as *interfaces with
in-memory implementations*, which is all the simulator needs. Production
`efsd` still keeps metadata in the in-memory table and makes it durable with
the snapshot / root-2PC flush that step 11 deletes. Deleting that path before
a durable replacement is wired would drop metadata durability, so 10.5 is
ordered ahead of it: durable backends first (gated by re-running the whole
simulator against them, `efsd` untouched), then the applied SM in-sim, then
production adoption for the single export. **Status (Sep 6):** 10.5a/b
and 10.5c-1..8 are gated in-sim; 10.5c-9 (Raft host) and 10.5c-10
(LOOKUP/GETATTR via ReadIndex + KV) and 10.5c-11 (file CREATE as one
Raft entry) and 10.5c-12 (MKDIR as a 2-shard txn) and 10.5c-13
(last-link file UNLINK) and 10.5c-14 (mode/owner SETATTR) and
10.5c-15 (empty LOCAL RMDIR as a 2-shard txn) and 10.5c-16 (LINK as a
2-shard txn) and 10.5c-17 (nlink>1 UNLINK as a 2-shard txn) and
10.5c-18 (utimens inode fence) and 10.5c-19 (same-dir LOCAL file
RENAME as a 2-shard txn) and 10.5c-20 (READDIR/LOOKUP_PATH via
ReadIndex + KV) and 10.5c-21 (SETATTR SIZE / chunk-aligned truncate)
and 10.5c-22 (chunk publish + GETCHUNKS)
and 10.5c-23 (unaligned truncate tail CAS)
and 10.5c-24 (cross-group propose: MKFS submit + inode bounce)
and 10.5c-25 (O_APPEND reserve + resolve-on-report)
and 10.5c-26 (SYMLINK as CREATE S_IFLNK + publish)
and 10.5c-27 (same-dir LOCAL directory rename)
and 10.5c-28 (HASHED dest CREATE)
and 10.5c-29 (HOLD open-unlinked leases)
and 10.5c-30 (non-blocking FLOCK grant/release)
and 10.5c-31 (non-blocking whole-file fcntl)
and 10.5c-32 (non-blocking fcntl byte ranges)
and 10.5c-33 (F_GETLK leader read)
and 10.5c-34 (blocking lock waits: FIFO leader queue)
and 10.5c-35a (session record + register + establish)
and 10.5c-35b (real session uuid/epoch on HOLD/FLOCK)
and 10.5c-35c (revocation barrier: fence + waiter dequeue)
and 10.5c-35d (append-reservation reclaim on fence as FENCED_HOLE)
are gated on a
scratch cluster behind `EFS_MD_RAFT`. **Status (Sep 11): production
adoption has landed** — `efsd` + `efs-fuse` serve the single export from
the Raft+KV engine behind `EFS_MD_RAFT`, reads AND writes (every FUSE
metadata op is a Raft proposal; chunk publish/GETCHUNKS included), gated
at 198/201 posix solo in 34 s and 193/201 under jobs=16 (commits 3299e89,
807327a, 495444d; the last is the event-driven pump + eager commit
broadcast + name-ordered readdir cursor). Remaining: three known-debt
posix failures (cross-dir rename EINVAL, `.find` unimplemented, the
report-poisoning EIO flake), then step 11 — cutover of the live table is
not this work. The KV engine is
a WAL plus immutable sorted segments with compaction, and there is **one
engine and one group-committed WAL per node** — the shard prefix in every key
multiplexes all groups into it, which is the same "logical groups, not
physical WALs" rule as [performance.md](arch/performance.md) §5.4. The Raft
log follows the same shape for the same reason — **one multiplexed record log
per node**, every group appending to it, concurrent appends sharing one fsync.

**The Raft log is the durability boundary; the applied KV is a replayable
view.** A committed entry is one a majority holds in its log, and the KV is
rebuilt by replaying forward from the snapshot point, so a metadata write pays
one persistence boundary rather than two and the KV owes nothing on the
critical path. This is not a new decision — it is what the step 4 state
machine already assumes, since it starts at `last_applied = snap_idx` and
re-applies everything above it (which is also why apply must stay idempotent,
I16). It costs exactly one ordering rule: a snapshot drops the log prefix, so
**no snapshot may advance past what the applied KV has durably stored**. The
KV's own sync mode is therefore a performance choice, not a correctness one.

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
  never blocks a prepare, but it *is* visible from its transaction's
  decision, not from its materialization.
- **Content epoch** — the inode's content generation; bumped by truncate to
  invalidate lane state and reject in-flight stale publications. It is a
  fence, not part of any object's identity.
- **Inode fence** — the bounded transaction over the inode row plus a file's
  active lanes (≤65 authorities) that distributes a new `content_epoch` or
  `mtime_gen` and carries truncate's per-lane range delete. Used only by rare
  operations; the write path never touches the inode shard.
- **Validated collect** — the read pattern shared by `stat()` and multi-chunk
  reads: collect versioned state, do the expensive work, re-read the
  versions, retry if they moved, with a read-only transaction as the bounded
  escape hatch.
- **Session** — a client's `{client_uuid, session_epoch, state, touched
  shards}` record on its session shard; the epoch is the fencing token, and
  the touched-shard set is what the revocation barrier fences.
- **Intent** — a durable prepared record written by a participant in a
  cross-shard transaction; never externally visible as a committed effect.
