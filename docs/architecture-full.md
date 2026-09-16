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
  `write()` = durable (§7.3) pays a synchronous round trip to *device*
  per write; that is affordable because the device is µs-scale flash, not
  ms-scale disk.
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
deviations* — not "a POSIX filesystem" full stop. Two deviations are known
and deliberate, and both are stated in this section rather than discovered
by a user: strictly-conforming per-read `atime` is **not offered** (§7.3),
and full syscall-level write atomicity **above the FUSE request boundary**
is an unresolved kernel-interface problem that efs does not claim (below).
Everything else in this section is a promise. If a deviation is ever added,
it belongs here, in this list, before it ships.

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
  problem above that size is explicitly open, §7.3): a returned
  `write()`/`pwrite()` is visible in its entirety to every subsequent read,
  and a concurrent read sees either all or none of that call's chunks
  (default mode; §7.3). **The obligation is symmetric** — a multi-chunk read
  must also return a state some serialization of the concurrent writes
  actually produced, so it validates the chunk-map versions it read; an
  atomic publication decision alone does not prevent a slow reader from
  splicing old and new chunks.

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
(flush/FUA or PLP media), not an RDMA completion. A returned `write()` is
durable and visible — stronger than POSIX, a deliberate latency-for-
durability trade. Degraded publication (with `u` domains already
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

> **§10 steps 0–12 are in** (simulator, KV, Raft, txns, sessions, dir
> spread ops including populated leftover migrate as a 2-shard txn,
> delete-2PC, FUSE A–D). LOCAL dirs auto-begin SPLITTING when `nents >
> EFS_DIR_SPREAD_MIN`, and the background migrator drains leftovers to
> HASHED (`src/meta/dir_spread.c`; host hook is one GC-thread pass).
> Remaining specified leftovers, in order:
> (1) **production joint-consensus reconfiguration** (§7.8 / step 6) —
> `efs_raft_change` is in `raft.c` + sim, not wired in `raft_host`;
> (2) **honest fio** on a 19810 NVMe cluster (do not auto `raft-mkfs`);
> pressure-triggered spread has no numeric bound — do not invent one.
> C1 relaxed-coherence is out of scope. Doc machine-gate:
> `make docs-check`. Cutover of a 36T `efs-test` is not this work.
> [architecture.md §10](#architecture)

The 10.5c increment list below is landed history, not the current task.

> **Architecture migration §10, step 10.5c (landed):** applied SM is gated in-sim
> (10.5c-1..8), the production Raft host is gated in `efsd` (10.5c-9),
> and LOOKUP/GETATTR go through ReadIndex + KV behind `EFS_MD_RAFT`
> (10.5c-10), file CREATE is a single Raft entry on the dentry
> shard (10.5c-11), MKDIR is a 2-shard txn (10.5c-12), last-link
> file UNLINK is one Raft entry (10.5c-13), mode/owner SETATTR is
> one Raft entry (10.5c-14), empty LOCAL RMDIR is a 2-shard txn
> (10.5c-15), LINK is a 2-shard txn (10.5c-16), nlink>1 UNLINK
> is a 2-shard txn (10.5c-17), utimens is the inode fence
> (10.5c-18), same-dir LOCAL file RENAME is a txn
> (10.5c-19), READDIR/LOOKUP_PATH are ReadIndex + KV
> (10.5c-20), SETATTR SIZE (chunk-aligned truncate, no tail)
> is one Raft entry (10.5c-21), chunk publish + GETCHUNKS
> (10.5c-22), unaligned SETATTR SIZE (tail CAS in the truncate
> entry, 10.5c-23), cross-group propose (10.5c-24: MKFS
> submit + inode bounce, no new opcode), and O_APPEND reserve
> (10.5c-25: reply size=watermark, getattr stays frontier until
> REPORT resolves), SYMLINK as CREATE S_IFLNK + publish of the
> target bytes (10.5c-26: no SYMLINK opcode, no target column),
> and same-dir LOCAL directory rename (10.5c-27: bounce before
> resolve so a scattered dest is not I9; pver GUARD + exclusive
> pver PUT; LOOKUP_PATH bounces like LOOKUP), and HASHED dest
> CREATE (10.5c-28: DIR begin/migrate/finish on an empty LOCAL
> dir; first use of a hashed dentry shard on another group is a
> 2-shard txn; SPLITTING dest is BUSY; bounce HASHED lanes), and
> HOLD open-unlinked leases (10.5c-29: `EFS_MSG_INODE_HOLD` on the
> inode shard; owner is the session stand-in; last close reclaims),
> and non-blocking FLOCK grant/release (10.5c-30: `EFS_MSG_INODE_FLOCK`
> on the inode shard; whole-file FLOCK domain; conflict is BUSY),
> and non-blocking whole-file fcntl (10.5c-31: same opcode with
> `EFS_FLOCK_FCNTL`; record-lock domain; flock on the same file
> does not conflict), and non-blocking fcntl byte ranges (10.5c-32:
> optional 16-byte start/end suffix; adjacent OK, overlap BUSY;
> flock-domain ranges INVAL), and F_GETLK as a leader read
> (10.5c-33: `EFS_FLOCK_GETLK`; no Raft entry; first conflicting
> record or F_UNLCK), and blocking lock waits (10.5c-34:
> `EFS_FLOCK_WAIT`; FIFO leader queue, grant is the reply; leader
> loss re-issues), the session record (10.5c-35a:
> CREATE/REGISTER/ESTABLISH apply; GET is a ReadIndex), and real
> session identity on HOLD/FLOCK (10.5c-35b: optional
> `(uuid, epoch)` wire suffix, `efs_session_accept` before
> propose), and the revocation barrier (10.5c-35c: coordinator
> `raft-session fence`, waiters of the old epoch never granted),
> and append-reservation reclaim on fence (10.5c-35d: OPEN
> reservations of epoch E resolve as FENCED_HOLE). Cutover of
> `efs-test` is not this work.
> [architecture.md §10](#architecture)
>
> **10.5c-1 is in (gated):** the first single-shard op batch over the
> applied KV — READDIR (LOCAL / SPLITTING / HASHED, including lane-0
> aliasing), SETATTR mode/owner (ctime not mtime, MAX-clamped, stale-handle
> reject), GETATTR as the validated double collect (file write lanes **and**
> HASHED-dir `used_shards` dir lanes; pending committed reductions; epoch /
> `mtime_gen` / `lane_seq` recheck; I9 on an unreachable coordinator),
> LOOKUP_PATH as a batched ancestor walk (resume from the terminal, I9,
> intermediate not-a-directory). The simulator proposes SETATTR through
> Raft, serves GETATTR/READDIR via ReadIndex, and walks LOOKUP_PATH
> hop-by-hop with a ReadIndex per shard; a leader crash mid-read does not
> change the answer. Gate: `test_meta_apply`, `test_sim`
> (`test_single_shard_ops`, `test_hashed_dir_stat`).
>
> **10.5c-2 is in (gated):** the `utimens` inode fence. Only utimens can
> set a time backwards, so it is the only op that bumps `mtime_gen` and
> pushes that generation onto every active write lane (file) or used dir
> lane (HASHED/SPLITTING directory). getattr then ignores older lane
> mtimes; a later write re-stamps at the new generation. atime-only does
> not fence. chmod after utimens does not hide the explicit mtime. The
> simulator proposes the fence through Raft; a leader crash does not
> resurrect a stale lane mtime. Gate: `test_meta_apply`
> (`test_utimens_fence`, `test_stat_fence_and_gen`, `test_stat_dir_hashed`),
> `test_sim` (`test_utimens_fence`, `test_hashed_dir_stat`).
>
> **10.5c-3 is in (gated):** truncate range-delete. SETATTR(size)
> is one apply batch: `content_epoch++`, `base_size = S`, per-active-lane
> epoch fence, prefix scan of each lane's chunk-map with DEL of keys at or
> past S, optional CAS of the straddling tail chunk in the same batch.
> A later publish at the old epoch is STALE (I22); a sub-chunk RMW cannot
> resurrect deleted keys. The simulator proposes truncate through Raft;
> crash/restart keeps size 0 and the deleted chunks gone. Gate:
> `test_meta_apply` (`test_truncate_range_del`), `test_sim`
> (`test_truncate_range_del`).
>
> **10.5c-4 is in (gated):** O_APPEND in-sim. Reserve against
> the real EOF (MAX of `base_size`, active-lane ends, pending reductions,
> watermark) — a private counter is a correctness bug. While unresolved,
> an append barrier rejects publish with `new_size > watermark`; getattr
> reports the contiguous resolved frontier, not lane `max_end`. Resolve is
> COMPLETED / ABORTED_HOLE / FENCED_HOLE; a retried reserve recovers the
> same offset (I16, `opid.extra`). The simulator proposes reserve/resolve
> through Raft; crash/restart keeps the reservation. Gate:
> `test_meta_apply` (`test_append_reserve`), `test_sim`
> (`test_append_reserve`).
>
> **10.5c-5 is in (gated):** in-sim LINK, UNLINK nlink>1, RMDIR, and
> file RENAME. LINK is dest dentry + inode `nlink++`. UNLINK last-link is
> one shard iff `dentry_shard == inode_shard`, else a 2-shard txn (the
> hardlink corner). RMDIR is a txn; LOCAL emptiness is one range check,
> HASHED emptiness is `dentry_seq` guards on used lanes (≤64; never raise
> `EFS_TXN_MAX_PART`). File RENAME is same-dir LOCAL one shard / cross-dir
> a txn; replace is not in this slice. Directory rename is INVAL until
> `parent_version` is on the inode row. Dest uniqueness CASes the current
> txn version (a reused name is not ver 0). Gate: `test_meta_apply`
> (`test_link_nlink`, `test_rmdir_rename`), `test_sim` (`test_link_i17`,
> `test_rmdir_rename`) — mem and `EFS_SIM_KV_DIR`/`EFS_SIM_RAFT_DIR`.
>
> **10.5c-6 is in (gated):** directory rename with `parent_version`. The
> inode row is 128 B and carries `parent` + `parent_version` (bumped on
> reparent). Ancestry of `dst_parent` is a read-set of **pver sidecar**
> guards (`EFS_KV_KIND_PVER`), not exclusive on ancestor inodes — chmod /
> mkdir in an ancestor does not abort. src in the chain is INVAL (cannot
> move a dir under itself). Concurrent `rename(a→b/a)` ‖ `rename(b→a/b)`:
> the second BUSYs on the first's exclusive pver PUT (I17, no half-apply).
> Cross-dir directory rename adjusts parent nlink; file rename does not.
> `EFS_TXN_MAX_PART` stays 8 (deep ancestry that would exceed it is BUSY).
> Replace is still not in this slice. Gate: `test_sim` (`test_dir_rename`,
> `test_rmdir_rename`) — mem and `EFS_SIM_KV_DIR`/`EFS_SIM_RAFT_DIR`.
>
> **10.5c-7 is in (gated):** the in-sim export is created by a Raft mkfs,
> not a local KV seed. Boot elects, proposes `efs_meta_apply_init` with a
> leader-stamped `now`, and apply is idempotent. getattr of ROOT is
> ReadIndex. 100% message drop cannot create the export (there is no
> locally-seeded ROOT to hide behind). Crash/restart keeps ROOT and files
> written after mkfs. Gate: `test_sim` (`test_export_mkfs`) — mem and
> `EFS_SIM_KV_DIR`/`EFS_SIM_RAFT_DIR`.
>
> **10.5c-8 is in (gated):** per-export `salt` is chosen at mkfs and
> stored on the ROOT shard (`EFS_KV_KIND_EXPORT`). MKDIR scatter is
> `hash(parent, name, salt) & 0xFFF`. Idempotent mkfs does not rewrite
> salt. Crash/restart keeps it. Gate: `test_meta_apply`
> (`test_export_salt`), `test_sim` (`test_export_salt`) — mem and
> `EFS_SIM_KV_DIR`/`EFS_SIM_RAFT_DIR`.
>
> Both durable backends are in (10.5a KV, 10.5b Raft log). The export
> question is **decided** (Sep 4, architecture.md §1): one export per
> cluster, hardcoded name `efs`; no create-export operation; multi-export,
> if ever, is one engine per export, never `export_id` in keys.
>
> **10.5c-9 is in (gated):** production Raft host in `efsd`, env-gated
> `EFS_MD_RAFT=1` (inert when off). Two groups (odd/even shard parity, same
> mapping as the sim), `raft_disk` + one `kv_lsm` per node, Raft messages
> on peer TCP (`EFS_MSG_RAFT`), a tick thread, idempotent `raft-mkfs`.
> Gate: `test_wire` (codec) + `tests/stress/raft_host_smoke.sh` on a
> scratch 4-node cluster (port 19820, `/tmp` storage — live cluster
> untouched): elect, mkfs ROOT on every voter, kill -9 follower then
> leader, restart catch-up keeps ROOT.
>
> **10.5c-10 is in (gated):** LOOKUP and GETATTR on the production host
> are leader + ReadIndex + applied KV when `EFS_MD_RAFT=1` (the in-memory
> table path is unchanged when the flag is off). Follower replies
> NOT_PRIMARY; a missing name is NOT_FOUND. Same scratch smoke, plus
> getattr of ROOT (mode 040755, nlink=2) and a miss lookup, including
> after crash catch-up.
>
> **10.5c-11 is in (gated):** file CREATE on the production host is one
> Raft entry on the dentry shard (`efs_meta_apply_create_file`; LOCAL
> parent = co-located with the parent). Duplicate name is EXIST; a
> directory mode is INVAL (MKDIR is a 2-shard txn, not this helper).
> Same scratch smoke, plus create/lookup/getattr, crash catch-up keeps
> the name.
>
> **10.5c-12 is in (gated):** MKDIR is the in-sim 2-shard txn
> (PREPARE/DECIDE/RESOLVE, same command bytes) on the production host.
> Directory mode on `EFS_MSG_INODE_CREATE` takes that path. The receiving
> node must lead every participant group (`NOT_PRIMARY` otherwise). Same
> scratch smoke: mkdir under ROOT (retry names), lookup, ROOT nlink=3,
> crash catch-up keeps the directory.
>
> **10.5c-13 is in (gated):** last-link file UNLINK on the production
> host is one Raft entry on the dentry shard (`efs_meta_apply_unlink`).
> Missing name is NOT_FOUND; a directory is INVAL (RMDIR is a txn);
> nlink>1 is INVAL (2-shard). `UNLINK_SHARD` is INVAL (old fan-out).
> Same scratch smoke: create+unlink a dedicated name, lookup miss,
> crash catch-up keeps the name gone.
>
> **10.5c-14 is in (gated):** mode/owner SETATTR on the production
> host is one Raft entry on the inode shard (`efs_meta_apply_setattr`).
> Missing ino is NOT_FOUND without proposing; SIZE/MTIME/ATIME are
> INVAL (truncate / utimens later). Same scratch smoke: chmod 0600 on
> the created file, getattr confirms, crash catch-up keeps
> `mode=0100600`.
>
> **10.5c-15 is in (gated):** empty LOCAL RMDIR is the same 2-shard
> txn as MKDIR (PREPARE/DECIDE/RESOLVE) on the production host.
> HASHED/SPLITTING are INVAL/BUSY (dseq-on-used-lanes later). The
> receiving node must lead every participant group. Same scratch
> smoke: mkdir a dedicated name, rmdir, lookup miss, rmdir of a file
> is INVAL, crash catch-up keeps the name gone.
>
> **10.5c-16 is in (gated):** LINK is dest dentry + inode `nlink++` as
> the same 2-shard txn as MKDIR (PREPARE/DECIDE/RESOLVE) on the
> production host. LOCAL dest only; HASHED/SPLITTING are INVAL/BUSY.
> Directory src is INVAL; `LINK_SHARD` is INVAL (old fan-out). Same
> scratch smoke: link the created file, both names, nlink=2, duplicate
> EXIST, miss NOT_FOUND, crash catch-up keeps the extra name and
> nlink=2.
>
> **10.5c-17 is in (gated):** nlink>1 file UNLINK is dest dentry DEL +
> inode `nlink--` as the same 2-shard txn as LINK (PREPARE/DECIDE/
> RESOLVE) on the production host. Last-link with dentry shard ≠
> inode shard uses the same path; last-link on one shard stays a
> single Raft entry. LOCAL parent only; HASHED/SPLITTING are
> INVAL/BUSY. Same scratch smoke: extra link name, unlink it,
> surviving nlink=2, second unlink NOT_FOUND, crash catch-up keeps
> the extra name gone and the remaining link.
>
> **10.5c-18 is in (gated):** utimens is the inode fence on the
> production host (`EFS_MD_CMD_UTIMENS`). SETATTR with only MTIME/ATIME
> bumps `mtime_gen` and assigns the times; mixed mode+time is INVAL;
> SIZE was INVAL until 10.5c-21. Same-group lanes only — a fenced
> lane on another Raft group is INVAL this slice. Same scratch smoke:
> setattr mtime=1000000000 on the created file, getattr, crash catch-up
> keeps that mtime.
>
> **10.5c-19 is in (gated):** same-dir LOCAL file RENAME is src dentry
> DEL + dest dentry PUT + inode parent/ctime as the same 2-shard txn
> as LINK (PREPARE/DECIDE/RESOLVE). Cross-dir, directories, and
> HASHED/SPLITTING are INVAL/BUSY; dest exists is EXIST; `RENAME`
> (by ino) is INVAL (`RENAME_AT` is hosted). Same scratch smoke:
> `raft-smoke-n` → `raft-smoke-m`, old gone, new stays through crash.
> Restart persists `last_applied` without compacting the log
> (`efs_raft_restore_applied`) so CREATE is not replayed onto a KV
> that already renamed the name.
>
> **10.5c-20 is in (gated):** READDIR and LOOKUP_PATH on the production
> host go through ReadIndex + applied KV (same as 10.5c-10 LOOKUP/
> GETATTR). HASHED dir lanes are ReadIndexed via
> `host_read_inode_lanes`; SPLITTING READDIR is BUSY this slice.
> LOOKUP_PATH is hop-by-hop (dentry shard ReadIndex if it differs,
> then child lanes). Same scratch smoke: ROOT listing contains
> `f`/`m`/`l`/mkdir and not `n`/`u`/`h`; `/raft-smoke-f` and
> `/raft-smoke-m` resolve; crash keeps that.
>
> **10.5c-21 is in (gated):** SETATTR SIZE on the production host is
> `EFS_MD_CMD_TRUNCATE` (content_epoch fence + `base_size`). Chunk-
> aligned or zero only — unaligned sizes need a tail candidate and
> stay INVAL this slice. Mixed SIZE+mode/time is INVAL. Same-group
> lanes only. Same scratch smoke: setattr size=131072 on the empty
> created file, getattr, crash catch-up keeps that size (utimens
> after truncate still keeps mtime=1000000000).
>
> **10.5c-22 is in (gated):** chunk publication on the production host
> (`EFS_MD_CMD_PUBLISH` via `REPORT_CHUNKS`) and `GETCHUNKS` from
> ReadIndex + KV. Lane 0 / same-group only — first-use of a lane on
> another Raft group is INVAL this slice. Same scratch smoke: create
> `raft-smoke-p`, empty GETCHUNKS, publish chunk 0 size=131072,
> GETCHUNKS count=1, crash keeps the mapping.
>
> **10.5c-23 is in (gated):** unaligned SETATTR SIZE mints a same-group
> tail candidate and CAS-publishes it inside `EFS_MD_CMD_TRUNCATE` (I22
> range-delete of the rest). Mixed SIZE+mode stays INVAL. Same scratch
> smoke: after publish, size=1000 keeps ci=0, size=0 clears the map,
> crash keeps that.
>
> **10.5c-24 is in (gated):** a node that does not lead (or host) a
> participant group still serves the op. Non-empty `EFS_MSG_RAFT_MKFS`
> is leader-submit (`group` + cmd) or ReadIndex (`group` only) — no
> new opcode. Followers wait apply locally. Unhosted inode RPCs bounce
> to a dual-host (never self). Same scratch smoke: mkdir of an
> even-shard dest from raft_id 0, rmdir/link/rename/readdir of those
> names, crash keeps the even-shard dir.
>
> **10.5c-25 is in (gated):** O_APPEND reserve on the production host
> (`EFS_MD_CMD_APPEND_RSV` / `APPEND_RES`). Zero UUID skips the op-id
> window (sessions not hosted). Reply size is the watermark; getattr
> stays the frontier until REPORT resolves OPEN reservations whose
> range is covered. Unhosted inode groups bounce. Same scratch smoke:
> create `raft-smoke-a`, append 131072, getattr size=0, publish,
> getattr size=131072, crash keeps that size.
>
> **10.5c-26 is in (gated):** SYMLINK on the production host is
> CREATE with `S_IFLNK` plus publish of the target bytes — the same
> shape as FUSE (`efs_fuse_symlink`). No `EFS_MSG_INODE_SYMLINK`,
> no target column on `efs_meta_row`. Publish and GETCHUNKS accept
> files and symlinks; directories stay INVAL. Same scratch smoke:
> create `raft-smoke-s` mode=0120777, publish size=11, getattr and
> GETCHUNKS, duplicate EXIST, crash keeps mode and size.
>
> **10.5c-27 is in (gated):** same-dir LOCAL directory rename on the
> production host. MKDIR scatter puts the dir inode on another group,
> so the host bounces from the dentry type *before* resolve (resolve
> of a missing child row is I9). Ancestry is pver sidecar GUARDs plus
> an exclusive pver PUT on the renamed dir. Cross-dir and HASHED stay
> INVAL. LOOKUP_PATH bounces a hop whose child inode is unhosted, same
> as LOOKUP. Same scratch smoke: mkdir `raft-smoke-rd`, rename to
> `raft-smoke-re`, old NOT_FOUND, new OK mode+nlink=2, miss / exist /
> cross-dir, READDIR and LOOKUP_PATH, crash keeps the new name.
>
> **10.5c-28 is in (gated):** HASHED dest CREATE on the production
> host. Empty LOCAL dir → HASHED via DIR begin / migrate / finish
> (`raft-dir` over existing `EFS_MSG_RAFT_MKFS`, no new opcode).
> First use of a hashed dentry shard on another group is a 2-shard
> txn (parent `used_shards` bit only; dest dentry + co-located file
> inode + dir-lane stamp). SPLITTING dest is BUSY. LOOKUP /
> GETATTR / LOOKUP_PATH bounce when HASHED lanes are unhosted;
> ROOT READDIR stubs those children instead of failing the listing.
> Same scratch smoke: dedicated `raft-smoke-hd`, a file whose hashed
> dentry shard is the other Raft group (child ino shard ≠ parent),
> LOOKUP_PATH, ROOT READDIR, crash keeps the dir and files.
>
> **10.5c-29 is in (gated):** open-unlinked HOLD leases on the
> production host. `EFS_MSG_INODE_HOLD` proposes SESSION
> LEASE_OPEN/CLOSE on the inode shard (existing opcode; owner bytes
> are the session stand-in, epoch=1). Last-link UNLINK with a lease
> keeps nlink=0 (I19); last close reclaims. Directories INVAL.
> Sessions/fencing are not hosted (FLOCK is 10.5c-30). Same scratch smoke:
> dedicated `raft-smoke-k`, open, unlink name, getattr nlink=0,
> crash keeps the inode, close reclaims (getattr NOT_FOUND).
>
> **10.5c-30 is in (gated):** non-blocking flock grant/release on
> the production host. `EFS_MSG_INODE_FLOCK` proposes LOCK
> GRANT/RELEASE on the inode shard (existing opcode; whole-file
> FLOCK domain; owner is the session stand-in, epoch=1). Conflict
> is BUSY. Blocking wait queues, ranges, and F_GETLK are not hosted.
> Flag off is a no-op (old HOLD table). Same scratch smoke: dedicated
> `raft-smoke-w`, EX owner=1, EX owner=2 BUSY, UN, EX owner=2,
> crash keeps the lock, UN owner=2 then EX owner=1.
>
> **10.5c-31 is in (gated):** non-blocking whole-file fcntl on the
> production host. Same `EFS_MSG_INODE_FLOCK` opcode with
> `EFS_FLOCK_FCNTL` (no new opcode; record-lock domain; classic
> process owner kind; owner is the session stand-in, epoch=1).
> Same-domain conflict is BUSY; flock EX on the same file is the
> other domain and succeeds. FUSE `.lock` stays local. Flag off is
> a no-op. Same scratch smoke: dedicated `raft-smoke-c`, EX
> owner=1, EX owner=2 BUSY, flock EX owner=2 OK, UN, EX owner=1
> held through crash.
>
> **10.5c-32 is in (gated):** non-blocking fcntl byte ranges on the
> production host. Same `EFS_MSG_INODE_FLOCK` opcode; optional
> `EFS_FLOCK_RANGE_LEN` suffix (two native uint64_t start,end,
> half-open). Absent suffix is whole-file `[0, ~0]` (31). Struct
> size is unchanged. FLOCK domain rejects a non-whole-file range
> (INVAL). Adjacent ranges grant; overlap is BUSY. Blocking
> waits are not hosted. Partial-unlock split / adjacent merge
> is not this slice. Same scratch smoke: dedicated
> `raft-smoke-t`, EX `[0,100)` owner=1, EX `[100,200)` owner=2 OK,
> EX `[50,150)` BUSY, inverted INVAL, flock-domain range INVAL,
> owner=2 `[100,200)` held through crash.
>
> **10.5c-33 is in (gated):** F_GETLK as a leader ReadIndex on the
> production host. Same `EFS_MSG_INODE_FLOCK` opcode with
> `EFS_FLOCK_GETLK` (no new opcode; no Raft entry). Reply packs
> the first conflicting record (`type`/`owner`/`start`/`end`) or
> F_UNLCK (`type=un`). Same-owner does not conflict. Flag off is
> INVAL. FUSE `.lock` stays local. Same scratch smoke: on
> `raft-smoke-t`, GETLK `[50,150)` reports owner=1 `[0,100)`,
> own-range GETLK is UNLCK, free range is UNLCK, ino 0 INVAL,
> miss NOT_FOUND; after crash GETLK `[100,200)` still reports
> owner=2. Cutover of `efs-test` is not next (not step 11).
>
> **10.5c-34 is in (gated):** blocking lock waits on the production
> host. `EFS_FLOCK_WAIT` on a conflicting grant queues the request
> at the leader (FIFO per inode, leader memory, not Raft state) and
> holds the RPC — the reply IS the grant, and the grant is still a
> Raft record. A queued waiter blocks a later conflicting request
> (no barging, no starvation). Leader loss replies NOT_PRIMARY and
> the client re-issues; the queue rebuilds there. WAIT with UN or
> GETLK is INVAL; flag off is INVAL. The queue has an owner-keyed
> dequeue hook (`server_raft_host_lock_wait_drop_owner`) for the
> session revocation barrier (10.5c-35). FUSE `.lock` stays local.
> Same scratch smoke: dedicated `raft-smoke-q` — a waiter pends
> behind a held EX, a queued SH does not barge past the EX waiter,
> release grants in FIFO order, and a waiter pending across the
> leader kill re-issues on the new leader and grants after the
> surviving holder's release. Cutover of `efs-test` is not next
> (not step 11).
>
> **10.5c-35a is in (gated):** session record + register + establish
> on the production host. `EFS_MD_SESS_CREATE` / `REGISTER` /
> `ESTABLISH` apply the same encoding as the sim (no new opcode;
> mgmt `raft-session` submits via `EFS_MSG_RAFT_MKFS`). GET is a
> ReadIndex (sub=0, not a log command); salt carries epoch, ACTIVE
> state, and the registered shard's touched bit. CREATE is
> idempotent. Same scratch smoke: create uuid,
> register its session shard, establish, GET ACTIVE+touched, and
> the record survives the leader/follower kill.
>
> **10.5c-35b is in (gated):** real `(uuid, epoch)` session identity
> on HOLD/FLOCK. An optional `EFS_SESS_WIRE_LEN` (20-byte) suffix on
> `efs_msg_inode_hold` / `efs_msg_inode_flock` carries `(uuid[16],
> epoch)`; absent keeps the `uint64_t` stand-in (epoch 1). The host
> runs `efs_session_accept` on the inode shard before proposing
> (wrong/not-established epoch → BUSY). mgmt `raft-hold` /
> `raft-flock` / `raft-fcntl` take `[uuid-hex epoch]`. Same scratch
> smoke: a HOLD/FLOCK carrying an established uuid is accepted, a
> wrong epoch is BUSY, and the stand-in path still works.
>
> **10.5c-35c is in (gated):** the revocation barrier on the production
> host. Apply hosts BEGIN / FENCE_LOC / ACK / FINISH / LEASE_DROP
> (same bytes as the sim). mgmt `raft-session fence` is the
> coordinator: it reads the frozen `touched_shards` bitmap (GET
> shard≥4096 returns one word), FENCE_LOCs every set bit, ACKs,
> FINISH, then LEASE_DROP of the old epoch. FENCE_LOC dequeues
> in-memory waiters of that uuid/epoch (never granted; BUSY/STALE).
> Same scratch smoke: a waiter of epoch 1 is BUSY after fence,
> epoch 1 is rejected, epoch 2 is accepted after establish, and
> GET after crash is ACTIVE at epoch 2.
>
> **10.5c-35d is in (gated):** append-reservation reclaim on fence.
> `LEASE_DROP` of epoch E resolves that session's OPEN reservations
> on the shard as `FENCED_HOLE` (committed zero hole; frontier
> advances). Production `raft-append` takes an optional
> `(uuid, epoch)` suffix (same as HOLD) so the reservation is
> tagged; absent keeps the zero-UUID stand-in. Gate: same smoke —
> reserve 128 KiB under epoch 1, getattr stays 0, fence, getattr
> is 131072, old-epoch append is BUSY.
>
> **The one rule 10.5c owes 10.5b** (`include/efs/raft_disk.h`): the Raft log is
> the durability boundary and the applied KV is a replayable view, so never
> call `efs_raft_snapshot()` until the KV is durable through `last_applied`
> (`efs_kv_lsm_flush()`). The snapshot drops the log prefix that would
> otherwise replay those commands. `efs_raft_new()` starts at
> `last_applied = snap_idx`. The host persists applied without
> compacting and restores it on restart (`efs_raft_restore_applied`)
> so a durable KV is not re-applied. Apply must still stay idempotent
> in the window before that persist.
>
> Production `efsd` still uses the in-memory table and the snapshot /
> root-2PC flush; only after 10.5c does step 11 delete that flush. If a
> decision is missing, stop and ask.

**Rule for picking the next one after that:** the order is
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
| The read path, or read prefetch/caching | [protocols/data.md](protocols/data.md) "validated collect" | I24, I13 | posix, posix2 (cross-client visibility) |
| Client reconnect, leases, locks, open-unlinked | [protocols/sessions.md](protocols/sessions.md) | I19, I23, I16 | posix2, posixstress |
| Cross-shard anything | [protocols/transactions.md](protocols/transactions.md) | I16, I17, I9 | posix2, posixstress |
| Raft, KV, replication, membership | [architecture.md §7.1/§7.8](#architecture), [failure-tolerance.md](failure-tolerance.md) | I1–I4, I10, I18 | `tests/test_sim`, leaks |
| Production Raft host (`EFS_MD_RAFT`) | `src/server/raft_host.c`, [architecture.md §10](#architecture) 10.5 | I1–I4, I16; never `efs_raft_snapshot()` until KV flush-through-applied | `tests/test_wire`, `tests/stress/raft_host_smoke.sh` (scratch cluster; not live `efs-test`) |
| Simulator / applied KV SM | [verification.md](verification.md), `include/efs/sim.h`, `include/efs/meta_apply.h`, `include/efs/raft.h` | I1–I4, I9, I10, I13–I16, I20–I23, I25 | `tests/test_sim`, `tests/test_meta_apply`, `tests/test_raft` |
| Op-ID / idempotency window | [architecture.md §7.9](#architecture), `include/efs/opid.h` | I16 | `tests/test_sim` |
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

---

### 5. If you are an AI agent — or briefing one

This page exists so that a **less advanced model can produce a correct
change**. That works only if the task arrives pre-chewed: the model's job is
execution inside hard edges, not exploration. The briefer (human or
orchestrator) owns: picking the step (§1), decomposing it until every
instruction is mechanical, naming the exact files to read (§2 — the Read
column and nothing else), and reviewing the diff. The agent owns: staying
inside the named files, and the checklist in §3 — all of it.

A briefing that works, paste-able:

```text
You are making ONE behavior-preserving change to the efs repo.
Read first, in order, and read nothing else:
  docs/arch/START-HERE.md, then only the files your routing row names.
Task: <one step, decomposed until mechanical: what moves, what does
not, what is forbidden>
Hard rules: no logic changes outside the task; no renames; no new
dependencies; no files outside the ones named; if anything seems to
need a design decision the spec does not contain, STOP and report —
do not decide.
Done means: builds on a test node (never in $HOME), make test passes,
the routing row's gate passes via tests/run_tests.sh, and you record
the result directory. A timeout is a failure. Verify the mount with
findmnt before trusting any suite result.
```

Two traps a pasted briefing must name because an outside agent cannot
rediscover them (the other recurring ones are already in §3):

- **EEXIST on a unique, never-used name is a bug, never benign.** Do not
  swallow it as a race and move on.
- **`pgrep -x`, never `pgrep -f`** on this project's processes — the `-f`
  pattern matches your own ssh command line and kills your own session.


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
2. **complete the profile cutover barrier** — the control plane enters
   `PROFILE_CUTOVER(P+1)`, *pushes* `P+1` to every publication authority,
   and waits for each to durably ACK it. From that point publication
   validation ([data protocol](protocols/data.md)) rejects any new
   publication whose durability evidence is not on the target profile, so
   the set of old-profile current generations can only shrink;
3. re-stripe **every protected data generation published before the
   cutover** to the new k+f width — each re-striped chunk is a *new
   immutable generation* written under the new **coding profile**
   (`coding_profile_id`), never an in-place reinterpretation of an
   existing generation;
4. verify that no current generation remains on the old profile;
5. only then commit `effective_f = target_f`.

**Step 2 is a barrier that must reach the authorities that enforce it.** A
control-plane Raft commit does not change what 4096 independent lane leaders
believe; a leader still holding profile `P` will happily accept an
old-profile publication after the "cutover", and the scan below is then
verifying a set that is still growing. This is exactly the mistake session
fencing had before round 7 — an authority cannot enforce a decision it has
never been told about — so `coding_profile_id`/`profile_epoch` is
**installed shard-local configuration**, replicated through each shard's own
Raft group, and the barrier is complete only when every publication
authority has ACKed:

```text
control plane commits PROFILE_CUTOVER(P+1)
push P+1 to every publication authority
every authority durably ACKs           <- barrier complete
only now begin the old-profile scan
```

Without the barrier the scanner races live writers and the verification is
meaningless:

```text
scanner re-stripes chunk X to 2+2
scanner moves on
    client publishes a NEW generation of X, still 2+1     <- allowed
scanner finishes, "verified"
effective_f := 2        -- but the current X survives only 1 loss
```

Because the cutover is committed *before* the scan and publication
validation enforces the current profile (round 7), any generation created
after the barrier is already on the target profile, and the scan only has
to cover generations that existed before it — a set that cannot grow. That
is what makes "no current generation is on the old profile" provable rather
than merely observed. In-flight writers holding the old profile are
rejected at publication and retry on the new one; their fragments are
orphans (L7), which is P3 working as intended.

The guarantee changes at that final commit, not when the operator asks.

**Lowering f is not the same sequence run backwards.** Raising works because
the weaker guarantee stays advertised while stronger data accumulates —
every intermediate state is at least as safe as what was promised. Lowering
inverts that: switching new writes to the weaker profile first would publish
2+1 data while the cluster still advertises f=2, so the advertised guarantee
would be false for as long as the migration takes. The advertised guarantee
must therefore drop **first**:

```text
LOWERING f:  2+2  ->  2+1
1. commit effective_f = target_f (the weaker guarantee)   <- advertise first
2. profile cutover barrier to the weaker profile (as above)
3. re-stripe / allow old wider generations to age out
4. only then shrink metadata RF and release replicas
```

Metadata RF comes last in both directions for the same reason: replicas are
removed only once nothing depends on the stronger quorum, and added before
anything does.

**Re-striping must not touch user-visible metadata.** It rewrites a
generation's *representation*, not the file's content, so it never updates
`size`, `mtime` or `ctime` — a migration that silently restamped mtime on
every chunk it re-encoded would corrupt every incremental-backup and
`make`-style workflow on the system while reporting success. The same rule
applies to repair/rebuild. Both are placement changes, not reformats. EC stripe
width k is independently configurable (wider k on larger clusters trades
encode CPU for storage efficiency, e.g. 4+3 = 1.75× overhead instead of
2+3 = 2.5×); the binding constraint is always N ≥ max(2f+1, k+f).

### Degraded operation

While the cluster is already operating with `u` unavailable failure domains
(`u ≤ f` failures already consumed), a data generation may publish with
`D ≥ k + (f − u)` durable fragments, marked degraded and queued for repair
— the invariant "survives the remaining f−u further losses" is maintained
throughout. The full rule is in [the data protocol](protocols/data.md);
rebuild scheduling is in [performance.md](performance.md).

**`u` is protection debt, not a headcount — repair pays it back, a node
coming back does not.** It is tempting to define `u` as "domains currently
down" and recompute it as nodes return. That is wrong, and it silently
under-protects data:

```text
f = 1, k = 2 (2+1)
A unavailable                       -> u = 1
generation G publishes to B, C      -> degraded, legally (k + f-u = 2)
A returns                           -> "u = 0"?  A does not hold G
B dies                              -> G has one fragment, k = 2
-> acknowledged data lost after ONE failure, at f = 1
```

The returning node restores *capacity*, not the missing fragments, so the
budget it appears to give back was never re-earned. `u` is therefore defined
per generation as **outstanding protection debt**: a degraded generation
consumes budget from the moment it publishes until its missing fragments
have actually been reconstructed and made durable. Aggregate degraded debt
is what the control plane reports and what admission for further degraded
publication is checked against; only repair retires it. This is also why
repair is prioritized above rebuild-for-balance
([performance.md](performance.md)) — debt is a live reduction of the
advertised guarantee.

**Unavailability is a committed control-plane state, never a client's
timeout.** Degraded publication is allowed only against domains the control
plane has *decided* are unavailable and recorded as such; it is never used
because a target is merely *slow*. In an asynchronous network a client
cannot distinguish the two, and letting it try would let any latency spike
quietly lower the protection level of freshly written data.


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
  split into kinds, and only the first kind takes an exclusive lock:

  ```text
  exclusive / CAS keys        the chunk-map entry, a dentry, an inode row
                              -> conflict, version check, one holder

  commutative reductions      MAX(lane.max_end,   ...)
                              MAX(lane.max_mtime, ...)
                              MAX(lane.max_ctime, ...)
                              MAX(dir_lane.mtime, ...)
                              -> transaction PAYLOAD, not a lock

  read / predicate guards     lane_seq · dentry_seq · parent_version ·
                              content_epoch · mtime_gen
                              -> SHARED: many guards coexist; a guard
                                 conflicts with anything that would change
                                 the guarded state, through the decision
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

  **"Not a lock" must not mean "not visible."** A reduction still becomes
  effective at its transaction's *decision*, and the decision can precede
  the reducer materializing it into `lane.max_end`:

  ```text
  lane.max_end = 1 GiB
  T publishes at 2 GiB; T's COMMIT decision is durable
  T's intent has not been resolved yet; lane.max_end is still 1 GiB
  stat() reads lane.max_end -> 1 GiB          -- WRONG: T already returned
  ```

  So a lane's reduction state is not just its materialized marks. Pending
  reduction intents are stored **discoverably under the lane's own key
  prefix**, and the authoritative read of a lane is

  ```text
  effective(lane.max_X) = MAX( materialized lane.max_X,
                               reduction payloads of pending intents on this
                               lane whose decision resolves to COMMIT )
  ```

  resolved by the same four-state rule below. Materialization by the reducer
  is therefore a background *compaction* of already-visible state, never the
  event that makes a committed reduction observable. Because the pending set
  is bounded by the in-flight transactions on that lane, this is a short
  scan, and it is what makes I21 hold end to end rather than only at the
  moment of commit.
- **Read/predicate guards: the third primitive, and several protocols are
  unsound without it.** A transaction frequently needs an *observation* to
  stay true until it decides — not a value it intends to write. Checking a
  version at prepare time is not enough on its own, because the gap between
  the final check and the decision is exactly where the observation can be
  invalidated. So a guard is a real, durable, *shared* participant record:

  ```text
  guard(T, key, observed_version)
      -> succeeds alongside other guards on the same key
      -> conflicts with any mutation that would change that key's state
      -> is held until T decides, not until T finishes reading
  ```

  Guards are what make these protocols correct, and they are the same
  mechanism in every case, which is the point of naming it once:

  ```text
  O_APPEND reservation   guards the active lanes' lane_seq  (§7.3)
  RMDIR on a HASHED dir  guards each dentry shard's dentry_seq
  directory rename       guards every ancestor's parent_version (§7.4)
  stat() / read fallback guards the collected lane / chunk-map versions
  ```

  **The RMDIR case also needs the guard to cover things that do not exist
  yet — the classic phantom.** Emptiness is a statement about a *range*, and
  an empty range contains no key whose version could be checked:

  ```text
  RMDIR: shard 17 holds no entries of this directory   -> "empty"
  CREATE: inserts `foo` into shard 17
  RMDIR: commits on its earlier observation
  -> a directory removed while an entry existed
  ```

  A version check over the existing keys cannot see this, because the
  conflicting write created a key that was not there to be checked. The fix
  is to give the predicate a **materialized witness**: every directory
  carries a `dentry_seq` per dentry shard, bumped by *every* dentry mutation
  on that shard including inserts. `RMDIR` guards those sequence keys, so the
  `CREATE` above conflicts with the guard and one of the two aborts. This
  gives predicate-level isolation without MVCC and without a distributed
  entry counter (rejected in [directory.md](directory.md) — a counter
  rebuilds the per-directory hotspot that spreading exists to remove).
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
4. Raft-commit the metadata publication of G  (chunk-map/size/mtime/ctime)
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

  The formulas themselves are defined once, in [§7.3 of the
  spec](#architecture): a fixed `LMAX = 64` for every file, the lane
  is the chunk index modulo `LMAX`, and the lane's shard is the odd-stride
  permutation from the inode's shard. Two properties fall out of that
  formula, and both are load-bearing:

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
                              [entries tagged E], resolving any pending
                              reduction intents; record every txid that
                              resolved as UNDECIDED  -> the decision set D
  3. collect those lane_seq values again, and re-resolve D
  4. re-read inode row -> E, M, A unchanged?
  5. if every lane_seq is unchanged AND E/M/A unchanged AND every txid in D
     is still undecided:
         the lane vector existed simultaneously (all values held between the
         end of collect 1 and the start of collect 2) -> MAX is linearizable
     else: retry
  ```

  **Step 5's decision clause is load-bearing, and version checks alone
  cannot replace it.** A transaction's intents are written at PREPARE, so
  they are already present in the keys a reader inspects; what changes later
  is the *decision*, which lives somewhere else entirely. A reader that
  resolved lane A's intent as undecided (and so excluded it), and then —
  after the transaction commits — resolved lane B's intent as committed (and
  so included it), has mixed two different states of one atomic transaction,
  and every `lane_seq` it checks is unchanged, because nothing about the
  intents changed. Only the decisions moved. The saving grace is that a
  decision is **final**: undecided → {COMMIT, ABORT} happens once and never
  reverses, so the reader only has to re-check the txids it resolved as
  *undecided*; anything it saw already decided is stable by construction.
  That keeps the extra validation proportional to in-flight transactions
  the read actually touched, usually zero.

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

  **"Only `utimens` can move a timestamp backwards" is a property efs
  enforces, not one the clock provides.** POSIX says an implicitly updated
  timestamp takes the current time, and Linux `CLOCK_REALTIME` can step
  backwards (NTP correction, operator adjustment); the failure model does not
  assume synchronized or monotone clocks anywhere else, so it must not
  quietly assume one here. efs resolves this deliberately: implicit
  mtime/ctime updates are **clamped by `MAX`**, so a clock step cannot make a
  file's implicit timestamps go backwards, and only `utimens` — an explicit
  request to set a time — can. The cost is that after a backwards clock step
  an implicit timestamp may read slightly ahead of wall clock until the
  clock catches up. That is a documented semantic choice suited to HPC, not
  bit-exact wall-clock POSIX behavior, and it is why `mtime_gen` guards only
  the one operation that can genuinely regress a time.

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
  (and `base_size` for truncate). `stat()` computes, under the same
  double-collect validation:

  ```text
  size  = MAX( inode.base_size,
               active current-epoch lanes' effective max_end )
  mtime = MAX( inode.base_mtime,
               lanes' max_mtime where lane.mtime_gen == inode.mtime_gen )
  ctime = MAX( inode.base_ctime,
               lanes' max_ctime )                        <- no gen guard
  ```

  **Only `utimens` bumps `mtime_gen`, and nothing guards ctime.** Getting
  this wrong makes a timestamp move backwards. Suppose a generation counter
  guarded both times and every status change bumped it:

  ```text
  write at t=100     -> lane max_mtime = 100, stamped gen 7
  chmod at t=200     -> ctime := 200, counter := 8
  stat()             -> the lane's mtime=100 is now filtered out as stale
                     -> mtime falls back to base_mtime: it moved BACKWARDS
  ```

  A generation guard is only ever needed for a timestamp that can be set
  **backwards**, and exactly one operation can do that: `utimens`, on mtime
  (and atime). Every other source of both times is monotone "now" — `chmod`,
  `chown`, `link`, `unlink`, `truncate` and writes all move ctime forward —
  so plain `MAX` is already correct for them, and giving ctime a generation
  would create the bug rather than prevent one. Hence: `chmod`/`chown`/
  `link`/`unlink` bump no counter at all, `utimens` bumps `mtime_gen`, and
  ctime is an unguarded reduction.

  **`mtime_gen` reaches the lanes the same way `content_epoch` does** — by
  the bounded inode+active-lane fence (truncate, below). A lane stamps its
  `max_mtime` with the generation it has been fenced to, so a publication
  carrying a stale generation is rejected and retried rather than silently
  filtered out later; without that, a write racing a `utimens` could have
  its mtime dropped after returning success. `utimens` is rare, the fence is
  ≤65 authorities, and the write path still never reads the inode row.

  `base_size` is what makes truncate correct: after a shrink the lanes are
  epoch-invalidated and contribute nothing, so the inode's own value is the
  answer until new writes exceed it. So a `stat()` after a returned
  `write()` — from any client — sees mtime and ctime at least as new as that
  write, and an explicit `utimens` is honored exactly. This is defined efs
  behavior, not "where POSIX permits."
- **atime is an explicit policy choice, not an omission.** A strict POSIX
  access timestamp would turn every read into a metadata mutation — the
  precise opposite of the read path this architecture is built for, and the
  reason `relatime` is the Linux default and `noatime` is universal in HPC.
  efs therefore defines: **`noatime` semantics are the default** — reads do
  not update atime at all, exactly as Linux `noatime` means; the stored
  atime is whatever an explicit `utimens` (or the file's creation) last set,
  and it simply does not advance on access. `relatime` semantics — advance
  atime only when it precedes mtime/ctime, or
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
  atomic. A reader that meets an uncommitted intent resolves it against the
  durable decision record using the **four-state rule** of
  [transactions.md](transactions.md) — COMMIT → the new value, ABORT → the
  old value, NO-DECISION → the old value (the read linearizes before the
  eventual decision), and *cannot establish authority* → a distinct error,
  **never "absent"** (I9). Many writers to disjoint chunks of one file still
  run fully in parallel: their transactions touch disjoint keys and do not
  contend. The **relaxed mode** (§3 of the spec) skips the transaction and
  publishes per-chunk.

  **An atomic write decision is only half of I24 — the read side needs its
  own protocol.** Committing `{A, B}` as one decision does not stop a reader
  from seeing a mixture, because the *reader* takes time too:

  ```text
  reader fetches chunk A                     -> old A
       writer's {A,B} decision commits
  reader fetches chunk B                     -> new B
  reader returns old A + new B               -- a state no serialization
                                                of read and write produced
  ```

  POSIX requires `read`/`write` effects to be atomic *with respect to each
  other*, so this is a real violation, and no amount of write-side atomicity
  fixes it. A multi-chunk read is therefore a **validated collect**, exactly
  parallel in structure to `stat()`:

  ```text
  1. collect the chunk-map versions covering the requested range,
     resolving any publication intents; record the txids that resolved
     as UNDECIDED -> decision set D
  2. fetch the fragments in parallel (the expensive part)
  3. re-read those versions AND re-resolve D
  4. both unchanged -> return the data
     either moved   -> discard and retry (bounded)
     exhausted      -> read-only multi-shard transaction over the range
  ```

  Step 3 re-resolves `D` for exactly the reason `stat()` does: a writer's
  intents are in place before the read begins, so a read that spans a
  transaction's *decision* can splice old and new chunks while every version
  it checks stays put. Decisions are final, so only the undecided ones need
  re-checking.

  The revalidation is metadata-only and overlaps the data fetch, so the cost
  is one extra small round per read, not a serialization point; single-chunk
  reads (the overwhelming majority) need no validation at all, since one
  chunk-map read is already atomic.

  **This is also the rule that keeps read prefetch honest.** The per-lane
  chunk-map windows that make sequential reads fast
  ([performance.md](../performance.md)) are *prefetch*, not a cache: a
  prefetched chunk map may be used without revalidation only within the read
  operation whose linearization interval covers the prefetch. Carrying one
  across later reads would turn it into a stale authoritative cache and
  contradict immediate cross-client visibility (§3 of the spec).

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
- **Truncate is a content-epoch bump, distributed by a bounded fence.**
  Truncate (or any wholesale content replacement) advances the inode's
  `content_epoch` and stamps the new authoritative `base_size`. But a bump
  recorded only on the inode row fences nothing: the lane leaders are the
  ones that must reject stale-epoch publications, and **ordinary writes must
  never consult the inode shard** — that is the whole point of the lanes. So
  the epoch has to be *pushed*, and truncate is the operation that pays:

  ```text
  truncate(FileID, S):
    transaction over { inode row } ∪ { active lanes }        (≤ 65 shards)
      inode:  content_epoch := E+1 ; base_size := S ; times := now
      lane:   accept E+1 (reject in-flight publications naming E)
              discard the lane's epoch-E size/time high-water marks
              RANGE-DELETE the lane's chunk-map entries beyond S
  ```

  The active-lane bitmap bounds the fence at 65 authorities, and truncate is
  inherently a file-wide serialization event, so it is the right operation to
  carry the cost. Rare ops pay; the hot path stays lane-local (P2).

  **The range delete is not an optimization — it is what stops truncated
  data from coming back.** Retaining chunk-map entries beyond the new size
  and reclaiming them lazily looks like an O(1) shrink, but it resurrects
  data on the next write:

  ```text
  chunk 100 = AAAA…               (published, 128 KiB)
  truncate(file, 0)               (lazy: chunk-map entry for 100 survives)
  pwrite(4 KiB @ chunk 100)       (sub-chunk RMW takes the OLD generation
                                   as its base)
  -> the 124 KiB of AAAA… around the new 4 KiB is back, after a truncate
     that returned success
  ```

  Remembering only the latest `base_size` does not fix it either: with
  shrink → extend → shrink cycles, whether an old entry survives depends on
  the *smallest* size any truncate newer than that entry imposed, not the
  most recent one. Deleting the entries is the rule that composes.

  It is also cheap, because the KV is **ordered** and a lane holds chunks
  `i, i+64, i+128, …` in chunk-index order: the entries beyond `S` are a
  contiguous key suffix, so each lane does **one range delete**, not one
  delete per chunk. Shrinking a 1 TiB file is 64 range deletes, not 8M key
  removals — the O(1)-ish shrink survives, and correctness with it. The
  fragments themselves are untouched: the orphaned generations are reclaimed
  lazily (L7), which is a space concern, not a correctness one.

  What the bump does **not** do is invalidate committed chunk data *below*
  the new size. Surviving entries are keyed by `FileID` and candidate
  generation, not by epoch (above), so the prefix POSIX requires to remain
  readable stays exactly where it was.

  **The straddling tail chunk is part of the truncate transaction, not a
  follow-up.** When `S` falls inside an existing chunk, the bytes from `S` to
  that chunk's end must be forgotten *permanently* — if the file later grows
  again, POSIX requires the extended region to read as zeros, not as
  whatever the old generation held there. A tail rewrite published after the
  truncate commits would leave a window where the file is nominally `S` bytes
  long but the tail chunk still carries the old bytes past `S`. So the
  rewrite is prepared first and committed *with* everything else:

  ```text
  truncate(FileID, S), S inside chunk C:
    1. read C's committed generation
    2. build a candidate: [0,S) preserved, [S, chunk_end) zeroed
    3. durably write its k+f fragments  (ordinary immutable PUT path)
    4. ONE transaction commits, atomically:
         inode:  content_epoch := E+1 ; base_size := S ; times
         lanes:  epoch fence + discard epoch-E marks
         lane(C): CAS-publish the new tail generation
                  (expected = the generation read in step 1)
         lanes:  RANGE-DELETE every chunk-map entry beyond C
  ```

  If the tail CAS loses to a concurrent writer, the whole truncate retries
  from step 1 — it never commits a size without the matching tail, and it
  never publishes a tail without the size. A chunk-aligned truncate skips
  steps 1–3 entirely. The rule for ordering against concurrent writes:

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

  **A private append counter is not EOF, and using one is a correctness
  bug.** If the reservation authority kept its own watermark, an ordinary
  extending write — which publishes on a *lane*, not on the inode shard —
  would never advance it:

  ```text
  pwrite(offset = 1 GiB)      -> size is now 1 GiB+ (lane max_end)
                                 a private append_eof is still 0
  O_APPEND write: reserve()   -> returns offset 0
  -> the append overwrites the file's data instead of extending it
  ```

  So the reservation must be taken against the file's **real** EOF, and it
  must serialize against *everything* that can move EOF, not merely against
  other reservations. It does that with the read-set machinery `stat()`
  already uses — no fence, and **no new cost on the ordinary write path**:

  ```text
  append authority = inode_shard(ino)

  reserve_append(FileID, len):
    read set:  the active-lane EOF vector, validated by lane_seq
               (the stat() double collect, incl. pending committed
                reductions)
    eof     =  MAX(base_size, active lanes' effective max_end,
                   reservation_watermark)
    write:     reservation_watermark := eof + len
               record reservation (FileID, eof, len, session)
    commit iff no lane_seq in the read set changed  -> else retry
    return eof
  ```

  An extending publication bumps its lane's `lane_seq` — which it already
  does — so a concurrent reservation's validation fails and it retries: the
  reservation and the extending write serialize on one side or the other of
  EOF determination, which is exactly what `O_APPEND` requires. Ordinary
  writers pay nothing for this; the appender absorbs the retry, consistent
  with `O_APPEND` being the one accepted same-file hotspot. The watermark is
  a monotone allocation cursor derived from observed EOF, never an
  independent counter.

  **Validating EOF once is not enough — the barrier must outlive the
  reservation.** POSIX couples EOF determination and the append write into
  one atomic step, so no intervening modification may fall between them.
  Validating at reservation time closes the race *while choosing* the
  offset, and leaves the interval afterwards open:

  ```text
  EOF = 100
  append A reserves [100,200)          -- A's data still in flight
  ordinary pwrite extends to 400       -- publishes lane.max_end = 400
  a reader now sees size 400 while A is unresolved
  -> no serialization explains it: pwrite-first means A should have
     taken 400; A-first means A's bytes must be visible before it
  ```

  So while a FileID has **unresolved reservations**, an *append barrier* is
  installed on its active lanes — the same bounded inode fence truncate uses
  (≤65 authorities), taken once when the reservation set becomes non-empty
  and released once when it drains, **per append burst, not per append**.
  Its rule at a lane leader is narrow, and it is a rule about EOF only:

  ```text
  publication that does not move max_end            -> unaffected
  publication within the reservation_watermark      -> unaffected
      (this includes the appenders' own reserved ranges)
  publication that would push EOF past the watermark
      -> must first advance the watermark on the inode shard,
         i.e. it orders against the outstanding reservations
  ```

  Disjoint and non-extending writes — nearly all concurrent traffic, and all
  of the data movement of the appenders themselves — continue in full
  parallel. Only the act of *extending past the outstanding append region*
  is ordered, which is precisely the operation `O_APPEND` cannot tolerate
  being reordered against.

  **Reservation length and short writes.** Reserving a requested length is
  only sound if the append is length-faithful, so efs fixes the length at the
  point where it is already known: the client holds the entire FUSE write
  request buffer before it reserves, and reserves exactly the length it will
  attempt. A shorter transfer therefore never arises from the usual cause
  (a partial device write); it can only arise from *failure*, and failure
  resolves as a committed zero hole rather than as a silent short append.
  efs does not hand back an offset for `len` and then write less than `len`
  while leaving the difference undefined — B's offset depends on A's length
  being real.

  The commit is one Raft mutation on the inode shard (the accepted
  hotspot). The client then writes its reserved `[eof, eof+len)` range
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

  **Crash semantics, stated precisely.** The `reservation_watermark` is
  internal allocation state — it is *not* the visible EOF;
  the visible size advances only as the frontier crosses resolved
  reservations. Every reservation must eventually *resolve*, and there are
  three ways, not one — a live client that simply fails must not be able to
  block the frontier until somebody kills it:

  ```text
  COMPLETED       the reservation's publication commits
  ABORTED_HOLE    the owning client is alive and its append failed:
                  it commits the abort itself, and the reserved range
                  becomes a committed zero hole
  FENCED_HOLE     the owning session is fenced (sessions.md); recovery
                  commits the same zero hole on its behalf
  ```

  The client is *required* to resolve a reservation it cannot complete;
  session fencing is the backstop for a client that stops participating
  altogether, not the ordinary path. Each resolution is a deliberate
  committed state, never an emergent one, and each unblocks the frontier for
  everyone queued behind it. The
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

- **The inode row lives on `inode_shard(ino)`** ([§5 of the
  spec](#architecture) defines the function; this document never
  restates placement formulas, it explains them).
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
defined in [the data protocol](data.md): *write-generated* size, mtime and
ctime live in the file's bounded write lanes (otherwise every write would
serialize
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
    spread directory:  inode_shard(new_ino) = dentry_shard(parent_ino, name)
                                              (the 64-shard permutation below)

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

The LOCAL inode row carries `nents`, a count of immediate children
(create/mkdir/link dest increment it; unlink/rmdir/a name leaving a directory
decrement it). Crossing `nents > EFS_DIR_SPREAD_MIN` commits `SPLITTING` on
that same parent-row PUT — one Raft entry, no extra round. After spread the
count is frozen; HASHED emptiness is the per-lane `dentry_seq` read set, not
a distributed counter (the spec rejected that). Runtime override:
`EFS_DIR_SPREAD_MIN` (tests). Pressure-triggered spread is specified; the
bound is not a number in the spec and is not invented here. Draining
SPLITTING leftovers is the background migrator (in-memory queue, GC-thread
pass on the host, opportunistic drain in the sim after a size-trigger
flip). `raft-dir migrate` plus finish stay as the idempotent operator
path.

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
dir.layout = SPLITTING(e)     new writes go to the directory's 64 dentry
                              shards; a migrator moves existing dentries
                              idempotently, in batches
    |
    v  (all dentries moved; owner Raft-commits HASHED)
dir.layout = HASHED(e)        all dentries on the 64 dentry shards; old
                              local range is GC'd
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

**For that to be bounded, the directory's shard set must be bounded — so a
spread directory uses a fixed 64-shard permutation, exactly like file
lanes.** Hashing names freely over all 4096 shards would let one directory's
used set grow to 4096, and `stat(dir)` with it; "bounded like a file's
lanes" would be wishful. A directory therefore has at most 64 dentry shards,
chosen by the same construction data.md uses for lanes — a name hashes to
one of 64 lanes, and the lane maps to a shard by the odd-stride permutation
defined in [§5/§7.3 of the spec](#architecture) (odd stride over a
power-of-two shard count ⇒ 64 DISTINCT shards; lane 0 is the directory's own
inode shard):

64 independent Raft leaders is the same throughput budget deemed sufficient
for the hottest single file, and it makes the used-shard set a **64-bit
bitmap on the directory's inode row** — a bounded `stat()` collect, by
construction rather than by hope. Global spread is unchanged: different
directories get different strides and different home shards, so a million
directories still cover all 4096 shards.

**First use of a dir lane touches the parent shard — the one exception.**
"The parent shard is not involved" holds for steady state, not for the first
mutation to land on a given lane: that one registers the lane in the
directory's used-shard bitmap, carried inside the transaction it is already
part of, exactly like `active_lanes` activation for files. It happens at
most 64 times in a directory's lifetime, so it is not a rate-proportional
cost. Without it `stat(dir)` could miss a shard that holds a newer mtime.

**Directory `utimens` needs the same generation fence as a file's.** An
explicit backwards mtime on a directory cannot stick while older
`dir_lane.max_mtime` values are still visible to the reduction, so the
directory inode row carries `dir_mtime_gen`, only `utimens` bumps it, and
the bump is distributed to the used lanes by the same bounded fence
(≤65 authorities) — the directory analogue of the inode fence in
[data.md](data.md). As with files, ctime needs no generation.

The reduction is validated by the same double-collect protocol as
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

**Reclamation is driven by acknowledgement, not by elapsed time.** The
failure model allows a message to be delayed arbitrarily and duplicated
([the spec §2](#architecture)), so no timer can *prove* that a
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

### Open-unlinked inode lifetime

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

### Distributed POSIX locking

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

#### Blocking waits: the wait lives on the authority

`F_SETLKW` and blocking `flock` are where "distributed" stops being a
placement detail. The protocol:

```text
client -> lock authority (the inode's shard leader):
          LOCKW(FileID, range, type, owner, op_id)
    conflicting -> the request stays OPEN at the leader; the waiter enters
                   an in-memory queue in arrival order
    grantable   -> the lock record commits via Raft; the held RPC replies
release/unlock -> the leader scans the queue in FIFO order and grants every
                  waiter whose request no longer conflicts; a waiter that
                  still conflicts stays queued
```

- **The grant is the reply.** A blocking lock request is a long-lived RPC
  the leader answers only when the lock is granted or the wait fails. No
  polling, no client-side retry loop, no timer. The wait queue is leader
  memory, not Raft state: a wait has no durability value (the client can
  always re-ask), and replicating it would put lock *contention* on the
  WAL.
- **Fairness rule.** Grants are made in arrival order; a still-conflicting
  waiter does not block non-conflicting requests behind it — except that a
  queued *exclusive* request stops later overlapping *shared* requests from
  being granted ahead of it, so an exclusive waiter cannot be starved by a
  shared-lock stream. POSIX requires no fairness at all; this is the
  cheapest rule that makes starvation impossible rather than merely
  unlikely.
- **Failover: the client re-issues, and the op-id makes it safe.** If the
  lock authority's leader changes while a wait is outstanding, the held
  request fails with it; the client re-issues the *same* `(op_id)` request
  to the new leader. Idempotency (I16) covers the case where the old leader
  committed the grant just before failing — the re-issued request finds the
  recorded grant and returns it instead of queueing again. FIFO order is
  not preserved across a failover; failover is rare, and fairness is
  policy, not correctness.
- **A fenced waiter is dequeued and never granted.** The revocation barrier
  already requires every touched shard to "resolve or abort that session's
  undecided work"; a queued lock wait is exactly that. The fence drops the
  queue entry and fails the held RPC, so a dead client's blocked syscall
  cannot wedge behind a lock it will never release, and a session fenced
  while waiting is never granted the lock.
- **Interruption cancels by op-id.** A signal (`EINTR`) aborts the wait
  locally; the client sends a cancel naming the wait's op-id. The cancel is
  idempotent: if the grant raced the cancel, the client owns the lock and
  simply unlocks it.

#### Range algebra is POSIX's, unchanged, on one authority

Because every record for one inode lives on one shard, the authority runs
exactly the local algorithm:

- a record covers `[start, end]`; `flock` is the whole-file range in its
  own domain;
- a partial unlock **splits** a record; adjacent same-owner, same-type
  records **merge**;
- a type change (`F_RDLCK`→`F_WRLCK`, `LOCK_SH`→`LOCK_EX`) converts in
  place, or queues like any other request while a conflict remains;
- `F_GETLK` is a leader read (no Raft entry) reporting the first
  conflicting record — the owner mapped back to a client-local pid for
  classic `fcntl` — or `F_UNLCK`.

**Resource bound.** Splitting makes the per-inode record count adversarial
— a process can lock and unlock alternating bytes and grow the table
without bound. The authority caps records per inode and returns `ENOLCK`
past the cap; that is the POSIX error for exactly this condition.

#### Deadlock detection: same-inode only — and that is conformant

POSIX lists `EDEADLK` under errors `F_SETLKW` **may** fail with, not shall,
and Linux itself detects deadlock cycles only for classic `fcntl` locks
(OFD `fcntl` and `flock` waits are never checked). efs matches that
exactly:

- **Same-inode cycles are detected.** The lock authority holds the complete
  wait-for graph for its inode — the holder set plus its wait queue — so a
  cycle among byte ranges of one file is caught and the incoming request
  fails `EDEADLK`.
- **Cross-inode cycles are not detected.** A cycle spanning two files can
  span two shards and two clients, and no single authority ever sees the
  whole cycle; a distributed wait-for protocol is rejected as cost and
  complexity on a path that is rare and self-inflicted. This is conformant
  because detection is a MAY — and no wait is ever *stuck*: every blocked
  wait is interruptible by signal and is torn down by session fencing, so a
  deadlocked application can always be killed out of it.


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
            `include/efs/kv.h` + `src/kv/kv_mem.c` are the seam; production
            still serializes to the EFSM blob until step 4 wires flush.
meta/       the POSIX op handlers — pure-ish functions over the kv/ and
            raft/ interfaces; no socket or FUSE calls inline.
            Table implementation lives in `src/meta/metadata.c` (moved from
            `src/common/`; still oversized — split by responsibility next).
wire/       the protocol — versioned encode/decode, nothing else.
data/       the data plane — EC encode/decode, RDMA PUT/GET, generation
            fencing. Store + transport vtables live here (`efs/store.h`,
            `efs/transport.h`); production NVMe I/O is still `server/store.c`
            until handler dispatch is carved (Phase M step 4).
client/     the FUSE adapter — thin; translates FUSE ops to meta/data calls.
            Path/RPC/data already live in `src/client/{ops,read,write,inode_rpc}.c`;
            `efs_fuse.c` is the translation layer (still oversized — follow-on).
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

The gate is `docs/check-architecture.py` (stdlib only). Run it with
`make docs-check` (login-node safe) or as part of `make test` on a build
node. It regenerates `architecture-full.md` into a temp file and diffs;
it never clobbers the checked-in copy.

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

**Harness (Sep 2).** `include/efs/sim.h` + `src/sim/` + `tests/test_sim`
drive a **fixed RF=3 Raft group** (servers 0..2) applying the §5 ordered-KV
SM (`src/meta/meta_apply.c`) against mem store and loop transport. Same seed
replays the same history hash. Metadata mutations are Raft log commands;
reads are leader + ReadIndex (no clock leases). Crash keeps KV + Raft persist
and restarts the SM from them. Faults that gate today: message drop,
partition (including minority leader / lost quorum), crash+restart with the
same disk, PUT-then-crash before publish (unpublished must not be readable),
publish without all k+f fragments, silent fragment corruption (skipped, never
decoded). Raft invariants gated in-sim: I1 (one leader per term), I2/I10
(acknowledged create survives f=1 replica crash), I3 (higher term fences the
old leader), I4 (partitioned leader cannot advance commitIndex). Op-IDs (I16)
persist in the CREATE batch. Production RPCs do not carry op-IDs yet, and
production `efsd` still uses the in-memory table. Sessions (I23) and
open-unlinked leases (I19) are driven: `efs_session` is a pure SM over one
KV, and the simulator runs the three-phase revocation barrier
(`ACTIVE(E) → FENCING → FENCE/ACK → ACTIVE(E+1)`) plus last-link keep /
reclaim. A fence-ACK withheld mid-barrier is a real event in `tests/test_sim`.
Data-target PUTs are not fenced. Production `efsd` still uses the in-memory
table. Cross-shard
transactions (I17) are driven: `efs_txn` is a pure SM over one KV per
participant, and the simulator runs MKDIR as a 2-shard PREPARE / DECISION /
RESOLVE (visibility at the coordinator's durable decision; L5 abort after
crash-during-prepare). Reconfiguration (I18) is driven: the simulator owns a control-plane
Raft group for **desired** placement, and each metadata group moves **actual**
membership through joint consensus (learners catch up before they vote). A
membership-transition interrupt (drop the new majority mid-joint) is a real
generator event in `tests/test_raft`. Production `efsd` still uses the
in-memory table.

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
ino reuse: delayed CLOSE / UNLOCK naming the previous incarnation
concurrent directory renames validating overlapping ancestries
create racing an rmdir emptiness check on a hashed directory
multi-chunk read interleaved with a multi-chunk write's decision
stat/read between a transaction's COMMIT decision and its resolution
truncate concurrent with in-flight publications on every active lane
sub-chunk write into a chunk whose range a truncate removed
O_APPEND reservation racing an ordinary extending pwrite
lost O_APPEND reservation reply, then retry of the same op ID
utimens racing an in-flight write on a lane
coding-profile cutover racing a live writer on the scanned chunk
silent fragment corruption (payload and identity)
transaction decision flipping between a collect and its revalidation
insert into a shard an rmdir already observed empty (phantom)
extending pwrite while an append reservation is unresolved
live client abandoning an append without being fenced
truncate crash between tail-generation PUT and the truncate decision
publication authority that has not yet ACKed a profile cutover
degraded generation, then the "returned" node dies before repair
clock step backwards between two implicit timestamp updates
out-of-order reply arrival at the dedup ack watermark
first use of a directory lane racing another first use
lock waiter outstanding across lock-authority leader failover
session fenced while its lock request is queued
grant racing an op-id cancel from an interrupted waiter
partial unlock splitting a record, then a conflicting F_SETLKW
two queued waiters granted in FIFO order on one release
queued exclusive request vs. a stream of later shared requests
same-inode fcntl deadlock cycle (must fail EDEADLK)
per-inode lock record cap exceeded (must fail ENOLCK)
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
not match the current placement is rejected (§7.3), that no read ever returns
a mix of chunks from different write calls (I24, which needs the read-side
validation and is invisible to a write-only checker), that a `stat` never
reports a size older than a returned write while its transaction is still
unresolved (I21), that truncated ranges never reappear through a later
sub-chunk write (I22), that a file's visible size never regresses across an
append reservation's lifetime (the EOF barrier, §7.3), that a truncate is
never observable without its zero-filled tail, that no chunk is published on
a profile the cutover barrier has retired, and that f simultaneous
losses never lose a published chunk (I11) — counting a degraded generation's
unrepaired fragments as **protection debt** rather than as budget restored by
a returning node — under every crash/interleaving the generator can produce.
The lock events add three checker properties: no two conflicting records are
ever simultaneously granted; a queued request is never granted to a session
that was fenced while waiting; and every wait eventually resolves — grant,
error, cancel, or fence — so no waiter wedges forever.

The corruption fault is the reason I25 exists as an invariant rather than an
implementation habit: it is only ever *tested* if the simulator can flip bits
in a durable object, and an EC decoder without integrity checking fails that
test by producing confidently wrong data rather than an error.

**Step 9 in-sim (gated).** Fragments are FileID-scoped and named by a unique
candidate generation (never G+1). Publication is `CAS(expected = committed
base)` of that candidate plus a lane MAX in one KV batch; a CAS miss does
not apply the MAX (I21). A publication naming a superseded `content_epoch`
is rejected; data-target PUT is not (I22/I23). `test_sim` and
`test_meta_apply` gate I13, I14, I15, I20, I21, I22 (epoch fence on lane-0
files), and I25, plus rejection of duplicate-node / wrong-profile evidence.
Not in this step: multi-chunk I24, truncate range-delete, O_APPEND, degraded
`u` / profile-cutover barriers. Production `efsd` is unchanged.

**Step 10 in-sim (gated).** A directory moves LOCAL → SPLITTING(e) → HASHED(e).
During SPLITTING the hashed side wins: a tombstone is ENOENT and the
migrator never resurrects it (I8). CREATE co-locates the file inode with
the dentry shard. POSIX locks are a pure SM over KV: classic+OFD `fcntl`
share one conflict domain, `flock` is independent; conflict → AGAIN, wait
FIFO, same-inode cycle → DEADLK, cap → NOLCK. A session fence dequeues
waiters as STALE and never grants them. `test_sim`, `test_lock`, and
`test_meta_apply` gate I8 and those lock properties. Not in this step:
hashed RMDIR/`dentry_seq`, directory-rename ancestry, `utimens` dir fence,
pressure-based spread, partial-unlock split, production `efsd` wiring.

**Step 10.5a in-sim (gated): the simulator runs on the durable KV.** Set
`EFS_SIM_KV_DIR=<dir>` and every server's applied state lives in
`efs_kv_lsm_open` instead of `kv_mem`, with no test changes — each sim
instance gets its own store under that directory, because `kv_mem` hands
every instance an empty one and reusing a directory would carry state across
cases. The sim's memtable threshold is deliberately tiny (512 B) so the runs
cross it: a threshold the sim never reaches would check every invariant
against a memtable and never against a segment or a compaction result. The
gate is all five sim suites OK with segments actually produced (51 L0 + 81
L1 on the recorded run). This is the substitution that makes the durable
store's correctness a property the existing invariant checkers prove, rather
than a separate claim.

`test_kv_lsm` covers what the sim cannot: semantics parity with `kv_mem`
(including the short-buffer probe and batch atomicity), a **real** crash —
the child `_exit`s without unwinding after a batch returned, so only an
fsync that already happened can make the parent's reopen see the write — a
torn WAL tail that must be discarded while its prefix survives, tombstones
shadowing values across levels, and 4000 keys with a third deleted verified
through auto-compaction and a reopen. One bug it caught: the compaction merge
advanced the winning source before comparing the others against its key, so
older duplicates survived their own tombstone. A scan-only test cannot see
that — it needs the compacted output read back.

**Step 10.5b in-sim (gated): the simulator runs on the durable Raft log.**
Set `EFS_SIM_RAFT_DIR=<dir>` and every server's Raft persistent state lives
in `efs_raft_disk_open` instead of `raft_mem`, with no test changes; set both
variables and both backends are durable at once. Two things make this a real
substitution rather than a link-time one. First, **all three of a server's
groups share one log**, which is the production shape (thousands of groups
per node multiplexing one fsync stream) and puts that sharing under test —
a group whose records leaked into another's state would show up as a
cross-group hard-state or log failure. Second, **a simulated restart really
closes and reopens the log**, so every crash the simulator injects becomes a
replay of what was on disk, under all the existing fault injection, rather
than a store object that quietly kept its RAM.

The gate is all five sim suites OK on the durable Raft store alone and on
both durable stores together. It was verified **non-vacuous by negative
control**: making replay silently drop ENTRY records fails `test_raft_store`
(16 checks) and `test_sim` (3, in transaction recovery). A gate that cannot
fail is not a gate, and for a store whose whole job is "what survives a
restart" that is the only way to know the sim depends on it.

`test_raft_store` covers what the sim cannot: answer-for-answer parity with
`raft_mem` driven through one shared script (gap rejection, in-place slot
overwrite, the short-buffer required-length report, snapshot-index term
lookup, `NOT_FOUND` vs never-saved config), a **real** crash via
`fork`+`_exit`, a torn tail that must be discarded *without* losing the good
records behind it and must leave the file appendable, truncation surviving a
reopen (a follower that loses a truncation gets conflicting entries back and
diverges), and rotation preserving live state through a reopen. One bug it
caught: replaying a *rotated* log applies the snapshot record before any
entries exist, which the validation inherited from `raft_mem` rejected —
correct for a live caller, wrong for replay, so the prefix drop is clamped on
the replay path and strict on the callback.

**Step 10.5c-1 in-sim (gated): single-shard reads and SETATTR over the
applied KV.** READDIR walks LOCAL / SPLITTING / HASHED including the
lane-0 alias (a hashed name that lives on the inode's own shard is not a
second copy). SETATTR mode/owner is one Raft entry on the inode shard:
ctime MAX-clamps, mtime does not move, a stale `expect_gen` is STALE.
GETATTR is the §7.3 / §7.4 validated double collect — MAX over the inode
row and the active write lanes (a file) or the `used_shards` dir lanes (a
HASHED directory), including committed-but-unmaterialized reductions, then
a recheck of `lane_seq` / pending txids / `content_epoch` / `mtime_gen` /
`used_shards`; an unreachable coordinator is I9, not absence. A hashed
create or unlink stamps its dir lane on the dentry shard and does not
move the directory inode row's times. LOOKUP_PATH is a batched
ancestor walk (resume from the terminal, I9 on a dangling dentry,
INVAL through a non-directory). The simulator proposes SETATTR through
Raft, serves GETATTR/READDIR via ReadIndex (file lanes or dir lanes), and hops LOOKUP_PATH with a
ReadIndex per shard; a leader crash does not change a linearizable read.
Gate: `test_meta_apply`, `test_sim`.

**Step 10.5c-2 in-sim (gated): `utimens` inode fence.** Only utimens can
set a time backwards, so it is the only op that bumps `mtime_gen` and
pushes that generation onto every active write lane (file) or used dir
lane (HASHED/SPLITTING directory). getattr ignores lane mtimes stamped
under an older generation; a later write re-stamps at the new generation
and is visible again. atime-only does not fence. chmod after utimens
does not hide the explicit mtime (it does not bump `mtime_gen`). The
simulator proposes the fence through Raft; a leader crash / restart does
not resurrect a stale lane mtime. Gate: `test_meta_apply`, `test_sim`.

**Step 10.5c-3 in-sim (gated): truncate range-delete.** SETATTR(size) is
one apply batch over the inode row and active lanes: bump `content_epoch`,
set `base_size`, fence each lane, range-delete chunk-map keys whose
byte range starts at or past the new size, and optionally CAS-publish the
straddling tail chunk in the same batch. A publish carrying the old epoch
is STALE (I22). The simulator proposes truncate through Raft; crash /
restart does not restore deleted chunks. Gate: `test_meta_apply`,
`test_sim`. Not in this step: O_APPEND, production `efsd`.

**Step 10.5c-4 in-sim (gated): O_APPEND.** Reservation is against the real
EOF (`MAX` of `base_size`, active-lane ends, pending committed reductions,
and the live watermark) — a private counter that ignores a prior pwrite
is a correctness bug. An unresolved burst holds an append barrier:
publish with `new_size` past the watermark is BUSY; getattr reports the
contiguous resolved frontier, not lane `max_end`. Resolve is COMPLETED,
ABORTED_HOLE, or FENCED_HOLE; a retried reserve of the same op-ID
recovers the same offset (`opid.extra`, I16). The simulator proposes
reserve and resolve through Raft; crash / restart keeps the reservation.
Gate: `test_meta_apply`, `test_sim`. Not in this step: cross-shard
rename/link/rmdir, production `efsd`.

**Step 10.5c-5 in-sim (gated): LINK, UNLINK nlink>1, RMDIR, file RENAME.**
LINK is dest dentry + inode `nlink++`. UNLINK last-link is one shard iff
dentry and inode co-locate, else a 2-shard txn. RMDIR is a txn: LOCAL
emptiness is one range check; HASHED emptiness is `dentry_seq` guards on
used lanes (the participant cap stays 8). File RENAME is same-dir LOCAL
one shard / cross-dir a txn; replace is not in this step. Directory
rename is INVAL until `parent_version` is on the inode row. A dest dentry
PUT CASes the current txn version so a reused name is not stuck at
expected 0. The simulator proposes through Raft; I17 abort leaves no
half-apply. Gate: `test_meta_apply`, `test_sim` (mem and durable). Not
in this step: directory rename, production `efsd`.

**Step 10.5c-6 in-sim (gated): directory rename with `parent_version`.**
The inode row is 128 B and carries `parent` + `parent_version` (bumped on
reparent). Ancestry of `dst_parent` is a read-set of pver sidecar guards,
not exclusive on ancestor inodes. src in the chain is INVAL. Concurrent
`rename(a→b/a)` ‖ `rename(b→a/b)` BUSYs the second on the first's
exclusive pver PUT (I17). Cross-dir directory rename adjusts parent nlink;
file rename does not. The participant cap stays 8. Replace is not in this
step. Gate: `test_sim` (mem and durable). Not in this step: a new export,
production `efsd`.

**Step 10.5c-7 in-sim (gated): export ROOT is a Raft mkfs.** Boot no longer
seeds the root with a local KV write. It elects, proposes
`efs_meta_apply_init` (leader-stamped `now`), and apply is idempotent.
getattr of ROOT is ReadIndex. 100% drop cannot create the export.
Crash/restart keeps ROOT. Gate: `test_sim` (mem and durable). Not in
this step: production `efsd` (that is 10.5c-9).

**Step 10.5c-8 in-sim (gated): export salt at mkfs.** MKDIR scatter hashes
with the per-export salt chosen at mkfs (`hash(parent, name, salt) &
0xFFF`). Salt lives on the ROOT shard; a missing record reads as 0;
idempotent mkfs does not change it. Crash/restart keeps salt and later
mkdirs still scatter with it. Gate: `test_meta_apply`, `test_sim` (mem
and durable).

**Step 10.5c-9 (gated): production Raft host in `efsd`.** Env-gated
`EFS_MD_RAFT=1` (no-op when unset). Two groups (odd/even shard parity),
one `raft_disk` + one `kv_lsm` per node under `<storage>/mdraft/`, Raft
messages on existing peer TCP, a tick thread, idempotent `raft-mkfs` on
the ROOT group. Gate: `test_wire` (codec) and
`tests/stress/raft_host_smoke.sh` on a scratch 4-node cluster (port 19820,
`/tmp` storage; live cluster untouched) — elect, mkfs ROOT on every
voter, kill -9 follower then leader, restart catch-up keeps ROOT.
Not in this step: LOOKUP/GETATTR (that is 10.5c-10).

**Step 10.5c-10 (gated): LOOKUP/GETATTR through ReadIndex + KV.** When
`EFS_MD_RAFT=1`, `EFS_MSG_INODE_LOOKUP` / `GETATTR` skip the in-memory
table: leader + ReadIndex, then `efs_meta_apply_getattr` /
`efs_meta_apply_lookup` on the applied KV. A follower replies
NOT_PRIMARY; a missing name is NOT_FOUND. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — getattr of ROOT (dir, nlink=2) on
the leader, NOT_PRIMARY on a follower, lookup miss, same after kill -9
catch-up. Not in this step: CREATE (that is 10.5c-11).

**Step 10.5c-11 (gated): file CREATE through Raft.** When `EFS_MD_RAFT=1`,
`EFS_MSG_INODE_CREATE` proposes `EFS_MD_CMD_CREATE` (same encoding as
the sim) on the dentry-shard group, waits `last_applied`, then LOOKUP +
GETATTR. Duplicate is EXIST; `S_IFDIR` is INVAL (MKDIR is a 2-shard
txn). `CREATE_SHARD` is INVAL (old fan-out). Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — create a file under ROOT, lookup +
getattr, second create EXIST, name survives kill -9 catch-up.
Not in this step: MKDIR (that is 10.5c-12).

**Step 10.5c-12 (gated): MKDIR through Raft.** When `EFS_MD_RAFT=1`,
`S_IFDIR` CREATE is a 2-shard txn (`EFS_MD_CMD_PREPARE` / `DECIDE` /
`RESOLVE`, same encoding as the sim) over parent dentry+row+dseq and
child inode+alloc. The receiving node must lead every participant
group; otherwise `NOT_PRIMARY`. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — mkdir under ROOT (retry names until
accepted), lookup, ROOT nlink=3, name survives kill -9 catch-up.
Not in this step: last-link UNLINK (that is 10.5c-13).

**Step 10.5c-13 (gated): last-link file UNLINK through Raft.** When
`EFS_MD_RAFT=1`, `EFS_MSG_INODE_UNLINK` proposes `EFS_MD_CMD_UNLINK`
(same encoding as the sim) on the dentry-shard group, waits
`last_applied`. Missing name is NOT_FOUND; `S_ISDIR` and nlink>1 are
INVAL (RMDIR / hardlink unlink are 2-shard txns). `UNLINK_SHARD` is
INVAL (old fan-out). Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — create+unlink a file under ROOT,
lookup miss, second unlink NOT_FOUND, name stays gone after kill -9
catch-up.
Not in this step: SETATTR (that is 10.5c-14).

**Step 10.5c-14 (gated): mode/owner SETATTR through Raft.** When
`EFS_MD_RAFT=1`, `EFS_MSG_INODE_SETATTR` proposes `EFS_MD_CMD_SETATTR`
(same encoding as the sim) on the inode-shard group, waits
`last_applied`. Missing ino is NOT_FOUND (no propose); SIZE/MTIME/ATIME
are INVAL (truncate / utimens later). Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — setattr mode 0600 on the created
file, getattr confirms, miss ino NOT_FOUND, SIZE mask INVAL, mode
survives kill -9 catch-up.
Not in this step: RMDIR (that is 10.5c-15).

**Step 10.5c-15 (gated): empty LOCAL RMDIR through Raft.** When
`EFS_MD_RAFT=1`, directory UNLINK is a 2-shard txn (`EFS_MD_CMD_PREPARE`
/ `DECIDE` / `RESOLVE`, same encoding as MKDIR) over parent dentry +
parent nlink/dseq and child inode. HASHED/SPLITTING are INVAL/BUSY.
The receiving node must lead every participant group; otherwise
`NOT_PRIMARY`. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — mkdir a dedicated name, rmdir,
lookup miss, second rmdir NOT_FOUND, rmdir of a file INVAL, name
stays gone after kill -9 catch-up.
Not in this step: LINK (that is 10.5c-16).

**Step 10.5c-16 (gated): LINK through Raft.** When `EFS_MD_RAFT=1`,
`EFS_MSG_INODE_LINK` is a 2-shard txn (`EFS_MD_CMD_PREPARE` / `DECIDE`
/ `RESOLVE`) over dest dentry + dest parent mtime/dseq and source
inode `nlink++`. LOCAL dest only; HASHED/SPLITTING are INVAL/BUSY.
Directory src is INVAL. `LINK_SHARD` is INVAL (old fan-out). The
receiving node must lead every participant group; otherwise
`NOT_PRIMARY`. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — link the created file under ROOT,
both names, nlink=2, duplicate EXIST, miss NOT_FOUND, directory INVAL,
extra name and nlink survive kill -9 catch-up.
Not in this step: nlink>1 UNLINK (that is 10.5c-17).

**Step 10.5c-17 (gated): nlink>1 UNLINK through Raft.** When
`EFS_MD_RAFT=1`, file UNLINK with `nlink>1` (or last-link with
dentry shard ≠ inode shard) is a 2-shard txn (`EFS_MD_CMD_PREPARE`
/ `DECIDE` / `RESOLVE`) over dest dentry DEL + dest parent mtime/dseq
and source inode `nlink--` (or inode DEL). LOCAL parent only;
HASHED/SPLITTING are INVAL/BUSY. Last-link on one shard stays
`EFS_MD_CMD_UNLINK`. The receiving node must lead every participant
group; otherwise `NOT_PRIMARY`. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — extra link of the created file,
unlink that name, surviving nlink=2, second unlink NOT_FOUND, extra
name stays gone after kill -9 catch-up.

**Step 10.5c-18 (gated): utimens inode fence through Raft.** When
`EFS_MD_RAFT=1`, SETATTR with only MTIME/ATIME is `EFS_MD_CMD_UTIMENS`
on the inode shard: bump `mtime_gen`, assign mtime/atime. Mixed
mode+time is INVAL; SIZE is INVAL. A fenced lane whose Raft group is
not the inode group is INVAL this slice. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — setattr mtime=1000000000 on the
created file, getattr, mtime survives kill -9 catch-up.

**Step 10.5c-19 (gated): same-dir LOCAL file RENAME through Raft.**
When `EFS_MD_RAFT=1`, `RENAME_AT` of a file in a LOCAL directory is a
2-shard txn (`EFS_MD_CMD_PREPARE` / `DECIDE` / `RESOLVE`) over src
dentry DEL + dest dentry PUT + inode parent/ctime + parent mtime/dseq.
Cross-dir, directories, HASHED/SPLITTING are INVAL/BUSY; dest exists
is EXIST; `RENAME` (by ino) is INVAL. The receiving node must lead
every participant group; otherwise `NOT_PRIMARY`. Restart restores
`last_applied` without compacting (`efs_raft_restore_applied`) so
CREATE is not replayed onto a KV that already renamed the name.
Flag off is a no-op. Gate: `tests/stress/raft_host_smoke.sh` —
`raft-smoke-n` → `raft-smoke-m`, old NOT_FOUND, new OK, old stays
gone after kill -9 catch-up.

**Step 10.5c-20 (gated): READDIR and LOOKUP_PATH through ReadIndex +
KV.** When `EFS_MD_RAFT=1`, `INODE_READDIR` and `INODE_LOOKUP_PATH`
are served from the applied KV (not the in-memory table). READDIR
ReadIndexes the directory (and HASHED used dir-lane groups) then
scans; SPLITTING is BUSY. LOOKUP_PATH walks hop-by-hop with a
ReadIndex on each dentry shard. Flag off is a no-op. Gate:
`tests/stress/raft_host_smoke.sh` — ROOT listing contains the
created/renamed/link/mkdir names and not the unlinked ones;
`/raft-smoke-f` and `/raft-smoke-m` resolve; listing survives
kill -9 catch-up.
Remaining: cross-group propose, then the rest of the mutations.
Not in this step: cutting over the live `efs-test` table, step 11.

**Step 10.5c-21 (gated): SETATTR SIZE / chunk-aligned truncate through
Raft.** When `EFS_MD_RAFT=1`, `SETATTR` with only the SIZE bit proposes
`EFS_MD_CMD_TRUNCATE` (`content_epoch++`, `base_size = S`). Chunk-
aligned or zero only; unaligned sizes (need a tail candidate) and
mixed SIZE+mode/time stay INVAL. Same-group lanes only — a fenced
lane on another Raft group is INVAL this slice. Flag off is a no-op.
Gate: `tests/stress/raft_host_smoke.sh` — empty `raft-smoke-f` to
size 131072, getattr, size survives kill -9 catch-up (utimens after
truncate still keeps mtime=1000000000).

**10.5c-22 — chunk publish + GETCHUNKS from KV (same-group / lane 0).**
`REPORT_CHUNKS` packs one `EFS_MD_CMD_PUBLISH` per rec (CAS + lane
MAX). `GETCHUNKS` is ReadIndex + `efs_meta_apply_get_chunk`. First-use
of a lane whose Raft group ≠ inode group is INVAL this slice (that is
a 2-shard txn). Flag off is a no-op. Gate: same smoke — create
`raft-smoke-p`, empty GETCHUNKS, publish chunk 0 size=131072,
GETCHUNKS count=1, mapping and size survive kill -9 catch-up.

**10.5c-23 — unaligned truncate tail CAS (same-group / lane 0).**
SETATTR SIZE that is not chunk-aligned mints a tail candidate and
CAS-publishes it in the same `EFS_MD_CMD_TRUNCATE` entry. A later
size=0 range-deletes the map. Same-group lanes only. Flag off is a
no-op. Gate: same smoke — after publish, size=1000 keeps ci=0,
size=0 clears GETCHUNKS, crash keeps that.

**10.5c-24 — cross-group propose (no new opcode).** A node that does
not lead a participant group submits via non-empty `EFS_MSG_RAFT_MKFS`
(`group` + cmd) or ReadIndex (`group` only). Followers wait apply.
Unhosted CREATE/LOOKUP/GETATTR/UNLINK/LINK bounce to a dual-host
(never self). RMDIR looks up the dentry first so an even-shard child
bounces before resolve. READDIR emits a dentry stub for unhosted
children. Directory RENAME is INVAL from `dent.type`. Flag off is a
no-op. Gate: same smoke — mkdir of even-shard `raft-smoke-xg*` from
raft_id 0, lookup/rmdir/link-dir/rename-dir/readdir, crash keeps
that dir (ROOT nlink=4).

**10.5c-25 — O_APPEND reserve (resolve on REPORT).** `INODE_APPEND`
proposes `EFS_MD_CMD_APPEND_RSV` on the inode group (bounce if
unhosted). Zero UUID skips the op-id window. Reply size is the
watermark; getattr stays the frontier while nopen>0. After a
covering `REPORT_CHUNKS` publish, OPEN reservations resolve
COMPLETED (`EFS_MD_CMD_APPEND_RES`) so the next write past the
watermark is not BUSY. Flag off is a no-op. Gate: same smoke —
create `raft-smoke-a`, append 131072, getattr size=0, publish,
getattr size=131072, crash keeps that size.
Not in this step: cutting over the live `efs-test` table, step 11.

**10.5c-26 — SYMLINK as CREATE + publish.** FUSE already creates
`S_IFLNK` and stores the target as ordinary chunk bytes. Host
CREATE packed that mode; publish/GETCHUNKS wrongly required
`S_ISREG`. Both now accept files and symlinks (directories stay
INVAL). No new opcode, no target column. Flag off is a no-op.
Gate: same smoke — create `raft-smoke-s` mode=0120777, publish
size=11, getattr+GETCHUNKS, duplicate EXIST, crash keeps mode
and size.

**10.5c-27 — same-dir LOCAL directory rename.** Host RENAME of a
directory is no longer INVAL. Bounce from the dentry type before
resolve (a scattered MKDIR dest on another group would otherwise
be I9). pver sidecar GUARDs on dst_parent ancestry and an exclusive
pver PUT on the renamed dir. Cross-dir and HASHED stay INVAL.
LOOKUP_PATH bounces unhosted child-inode hops like LOOKUP. Flag
off is a no-op. Gate: same smoke — mkdir `raft-smoke-rd`, rename
to `raft-smoke-re`, old gone / new present, miss NOT_FOUND, exist
EXIST, cross-dir INVAL, READDIR+LOOKUP_PATH, crash keeps the new
name.

**10.5c-28 — HASHED dest CREATE.** Empty LOCAL dir split via DIR
begin / migrate / finish (`raft-dir` over existing `RAFT_MKFS`).
First use of a hashed dentry shard on another group is a 2-shard
txn (parent `used_shards` bit; dest dentry + co-located file inode
+ dir-lane). SPLITTING dest is BUSY. Unhosted HASHED lanes bounce
on LOOKUP/GETATTR/LOOKUP_PATH; ROOT READDIR stubs those children.
Flag off is a no-op. Gate: same smoke — dedicated `raft-smoke-hd`,
hashed-dentry file whose inode shard differs from the parent,
LOOKUP_PATH, ROOT READDIR, crash keeps the dir and files.

**10.5c-29 — HOLD open-unlinked leases.** `EFS_MSG_INODE_HOLD`
proposes SESSION LEASE_OPEN/CLOSE on the inode shard (no new
opcode; owner is the session stand-in). Last-link UNLINK with a
lease keeps nlink=0 (I19); last close reclaims. Directories INVAL.
Sessions/fencing are not hosted (FLOCK is 10.5c-30). Flag off is a no-op. Gate:
same smoke — dedicated `raft-smoke-k`, open, unlink, getattr
nlink=0, crash keeps the inode, close reclaims.

**10.5c-30 — non-blocking FLOCK grant/release.** `EFS_MSG_INODE_FLOCK`
proposes LOCK GRANT/RELEASE on the inode shard (no new opcode;
whole-file FLOCK domain; owner is the session stand-in). Conflict
is BUSY. Blocking wait queues, ranges, and F_GETLK are not hosted.
Flag off is a no-op. Gate: same smoke — dedicated `raft-smoke-w`, EX
owner=1, EX owner=2 BUSY, UN, EX owner=2, crash keeps the lock.

**10.5c-31 — non-blocking whole-file fcntl.** Same
`EFS_MSG_INODE_FLOCK` opcode with `EFS_FLOCK_FCNTL` (no new opcode;
record-lock domain; classic process owner kind). Same-domain
conflict is BUSY; flock on the same file is the other domain and
does not conflict. Ranges, F_GETLK, and blocking waits are not
hosted. Flag off is a no-op. Gate: same smoke — dedicated
`raft-smoke-c`, EX owner=1, EX owner=2 BUSY, flock EX owner=2 OK,
UN, EX owner=1 held through crash.

**10.5c-32 — non-blocking fcntl byte ranges.** Same
`EFS_MSG_INODE_FLOCK` opcode; optional `EFS_FLOCK_RANGE_LEN`
suffix (two native uint64_t start,end, half-open). Absent suffix
is whole-file. Struct size is unchanged. FLOCK domain rejects a
non-whole-file range (INVAL). Adjacent ranges grant; overlap is
BUSY. Blocking waits are not hosted. Flag off is a
no-op. Gate: same smoke — dedicated `raft-smoke-t`, EX `[0,100)`
owner=1, EX `[100,200)` owner=2 OK, EX `[50,150)` BUSY, inverted
INVAL, flock-domain range INVAL, owner=2 `[100,200)` held through
crash.

**10.5c-33 — F_GETLK leader read.** Same `EFS_MSG_INODE_FLOCK`
opcode with `EFS_FLOCK_GETLK` (no new opcode; no Raft entry).
Reply packs the first conflicting record or F_UNLCK. Same-owner
does not conflict. Flag off is INVAL. Gate: same smoke — on
`raft-smoke-t`, GETLK `[50,150)` reports owner=1 `[0,100)`,
own-range and free-range UNLCK, after crash GETLK `[100,200)`
still reports owner=2.

**10.5c-34 — blocking lock waits.** Same opcode with
`EFS_FLOCK_WAIT`: a conflicting grant queues FIFO at the leader
(leader memory, not Raft state) and the held RPC's reply is the
grant. A queued waiter blocks a later conflicting request (no
barging). Leader loss replies NOT_PRIMARY and the client
re-issues. WAIT with UN/GETLK is INVAL; flag off is INVAL. Gate:
same smoke — dedicated `raft-smoke-q`, a waiter pends behind a
held EX, a queued SH does not barge past the EX waiter, release
grants in FIFO order, and a waiter pending across the leader
kill re-issues on the new leader and grants after the surviving
holder's release.

**10.5c-35a — session record + register + establish.** Production
host applies `EFS_MD_SESS_CREATE` / `REGISTER` / `ESTABLISH`
(same bytes as the sim). GET is a ReadIndex (sub=0, not a log
command). No new opcode: mgmt `raft-session` reuses
`EFS_MSG_RAFT_MKFS` submit. CREATE is idempotent. Gate: same
smoke — create a uuid, register its session shard, establish,
GET reports ACTIVE + touched, and the record survives crash.

**10.5c-35b — real session identity on HOLD/FLOCK.** Optional
`EFS_SESS_WIRE_LEN` suffix carries `(uuid, epoch)` on
`efs_msg_inode_hold` / `efs_msg_inode_flock`; absent keeps the
stand-in. The host runs `efs_session_accept` on the inode shard
before proposing (wrong/not-established epoch → BUSY). mgmt
`raft-hold` / `raft-flock` / `raft-fcntl` take `[uuid-hex epoch]`.
Gate: same smoke — established uuid accepted, wrong epoch BUSY,
stand-in path intact.

**10.5c-35c — revocation barrier.** Production host applies
`EFS_MD_SESS_BEGIN` / `FENCE_LOC` / `ACK` / `FINISH` /
`LEASE_DROP`. mgmt `raft-session fence` is the coordinator (GET
shard≥4096 reads a `touched_shards` word). FENCE_LOC dequeues
waiters of that uuid/epoch (never granted). Gate: same smoke —
waiter of epoch 1 BUSY after fence, epoch 1 rejected, epoch 2
accepted after establish, ACTIVE epoch 2 after crash.

**10.5c-35d — append-reservation reclaim on fence.** `LEASE_DROP`
of epoch E resolves that session's OPEN reservations on the
shard as `FENCED_HOLE`. Optional `(uuid, epoch)` suffix on
`raft-append` tags the rsv; absent keeps the stand-in. Gate:
same smoke — reserve 128 KiB under epoch 1, getattr stays 0,
fence, getattr is 131072, old-epoch append BUSY.

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

### Sep 1 2026 — nine external review rounds

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
#### Round 8 — composition edge cases

Round 8 found no structural problem: the reviewer's verdict was that the
architecture is stable and the remaining work is where correct mechanisms
*compose*. Four of the findings were real bugs in the specification.

*Atomicity had only a write side.* I24 was satisfied by publishing every
chunk of a call under one decision — but a reader takes time too. Fetch
chunk A, let the writer's `{A,B}` decision commit, fetch chunk B, and the
read returns old A with new B: a state no serialization of those operations
produced, and POSIX makes `read` and `write` atomic *with respect to each
other*. Write-side atomicity cannot fix it. A multi-chunk read is now a
validated collect — the same shape as `stat()` — and, as a direct
consequence, the per-lane chunk-map prefetch windows are explicitly prefetch
rather than a cache: usable without revalidation only inside the read whose
linearization interval covers them.

*Committed reductions could briefly vanish.* Round 7 correctly made lane
`MAX`es commutative payload rather than exclusive keys. But it also said
they are applied "at resolve time", while a transaction becomes visible at
its *decision* — so between a durable COMMIT and the reducer running, a
`stat()` could read a size older than a write that had already returned.
"Not a lock" was quietly reading as "not visible". Pending reduction intents
are now discoverable under the lane's key prefix and fold into the
authoritative lane read; materialization is background compaction of state
that is already visible.

*Truncate had two holes, and the second was a data-resurrection bug.* First,
nothing distributed the new content epoch to the lane leaders that are
supposed to reject stale-epoch publications — and ordinary writes must never
consult the inode shard. Truncate now pays: a bounded fence over the inode
row plus the active lanes, ≤65 authorities, for an operation that is
inherently a file-wide serialization event. Second, "chunks beyond the new
size become unreferenced and are reclaimed lazily" meant the chunk-map
entries survived, so a later sub-chunk write into one of those chunks would
take the pre-truncate generation as its RMW base and bring back bytes a
successful `truncate()` had removed. Remembering the latest `base_size` does
not fix it either: across shrink→extend→shrink cycles, whether an old entry
survives depends on the smallest size imposed by any *newer* truncate, not
the most recent one.

The reviewer proposed logical range tombstones. The merged fix is simpler
and exact: the same fence performs a **per-lane range delete**. Because the
KV is ordered and lane `i` holds chunks `i, i+64, i+128, …`, the entries
beyond the new size are a contiguous key suffix — 64 range deletes, not 8M
key removals. The O(1)-ish shrink survives *and* nothing can be resurrected,
with no per-chunk epoch bookkeeping and no unbounded truncate history.

*`O_APPEND` was correct only against other appenders.* `append_eof` was an
independent counter on the inode shard, but an ordinary extending `pwrite`
publishes on a *lane* and never touches it — so after a write to offset
1 GiB, the next append reserved offset 0 and overwrote the file. EOF
reservation must serialize against everything that can move EOF. It now
validates against the active-lane EOF vector using the read-set machinery
`stat()` already has: an extending publication bumps its lane's `lane_seq`,
which aborts a racing reservation. Ordinary writers pay nothing; the
appender absorbs the retry, which is consistent with `O_APPEND` being the
one accepted same-file hotspot.

*Two generations, or one guard in the right place.* The reviewer showed that
filtering lane mtime *and* ctime through a single `mtime_gen` makes mtime
move backwards: a write stamps gen 7, a `chmod` bumps to gen 8, and the
write's mtime is filtered out. Their fix was to split `mtime_gen` from
`ctime_gen`. The merged fix removes the bug at its source instead: a
generation guard is only ever needed for a timestamp that can be set
*backwards*, and exactly one operation can do that — `utimens`. Every other
source of both times is a monotone "now", so plain `MAX` is already correct.
Only `utimens` bumps `mtime_gen`, only lane mtimes are guarded, and ctime
needs no generation at all — giving it one would have created a
backwards-moving ctime rather than prevented one. The same bounded fence as
truncate distributes `mtime_gen` to the lanes, so a write racing a `utimens`
is rejected and retried instead of having its timestamp silently dropped.

*Online `f`/`k` change could not prove its own result.* Re-stripe, verify,
flip `effective_f` lets a live writer publish a new old-profile generation
behind the scanner, after which the cluster claims a protection level the
current data does not have. The profile cutover is now committed **before**
the scan, and publication validation (round 7) enforces the current profile
from that moment — so the set of old-profile current generations can only
shrink, and the scan covers a set that cannot grow.

*Two ABA gaps and one ownership error.* Round 7 scoped every data object by
`FileID`; inode-scoped *ephemeral* state — open leases, byte-range locks,
append reservations — was still keyed by bare `ino`, so a delayed CLOSE or
UNLOCK could release a reused inode's state. All of it is `FileID`-keyed
now, and any operation acting through an inode handle validates the
generation first. Separately, a single `(client_uuid, session_epoch,
process_id)` owner models only classic `fcntl` locks: on Linux both `flock`
and OFD `fcntl` locks belong to the **open file description** and are shared
by duplicated and inherited descriptors, so ownership is now per namespace.

*Elapsed time cannot prove a straggler is gone.* Dedup GC was gated on a
"retention window", but the failure model allows unbounded message delay, so
a timer asserts something the network never promised. Reclamation now
follows a client **response-ack watermark**. This also fixed a subtler
requirement: a retried `O_APPEND` reservation must recover the *same offset*
from the reply cache — knowing that "something completed" would let the
retry allocate a second range and leave a permanent hole.

*Positioning.* With strict per-read atime deliberately unsupported and
syscall-level atomicity above the FUSE boundary still open, "a POSIX
filesystem" claims more than the contract delivers. The spec now describes
efs as a high-performance parallel filesystem *targeting Linux/POSIX
semantics with explicitly documented deviations*, and §3 lists them.
#### Round 9 — protocol completion and kernel-interface reality

The verdict was that the architecture is stable and that what remained was
*protocol completion*, not redesign. Nothing about Raft sharding, fixed write
lanes, immutable generations, directory scatter/spread, session fencing or
the plane split was reopened.

*The normative matrix had gone stale against its own protocol section, and
§6 wins ties.* Round 8 made truncate a bounded fence over the inode row plus
active lanes, but §6 still described it as `content_epoch + base_size + times
on inode row, 1 shard, single Raft entry` — so by the spec's own precedence
rule the superseded design was the binding one. `SETATTR` had the same
problem in two ways: `utimens` now distributes `mtime_gen` through the lane
fence, and `SETATTR(size)` *is* truncate. This is the failure mode the
"never restate a normative table" rule exists to prevent, caught in the one
place a table is legitimately the single home for something.

*Read-set protocols were being used before they were defined.* Round 8 leaned
on read sets for `O_APPEND`, `RMDIR` emptiness, rename ancestry, and the
`stat`/read fallbacks, but §7.2 only had exclusive intents and commutative
reductions — so none of those had a stated mechanism. **Read/predicate
guards** are now the third transaction primitive: durable, shared, and held
*through the decision* rather than until the last check. `RMDIR` forced the
sharper form of the problem: emptiness is a claim about keys that do not
exist, and an insert into an observed-empty shard is a **phantom** that no
version check can detect. A per-shard `dentry_seq` bumped by every dentry
mutation gives the predicate a materialized witness — predicate isolation
without MVCC and without the distributed entry counter that would rebuild
the per-directory hotspot.

*Validated collects validated the wrong thing.* Checking key versions cannot
detect a transaction whose intents were already in place when the read began
and whose **decision** moved during it: resolve lane A as undecided (take the
old value), the transaction commits, resolve lane B as committed (take the
new value) — two states of one atomic transaction, with every version
unchanged. Collects now carry the set of txids they resolved as *undecided*
and re-check those. Because a decision is final, nothing already decided
needs re-checking, so the added cost is proportional to in-flight
transactions the read actually touched.

*`O_APPEND` was fixed only up to the reservation.* Round 8 closed the race
while determining EOF and left the interval after it open: an ordinary
extending `pwrite` could publish EOF 400 while an append's `[100,200)` was
still unresolved, a state no serialization explains. POSIX couples EOF
determination and the append write, so an **append barrier** now lives on the
active lanes while reservations are outstanding — taken once per append
burst, and constraining only publications that would push EOF past the
reservation watermark. Two smaller holes closed with it: the reserved length
is now defined as the length the client will attempt (reserving a *requested*
length only works if the append is length-faithful, or B appends after bytes
A never wrote), and a live client whose append fails resolves it itself as
`ABORTED_HOLE` — previously only a fenced client's reservation could resolve,
so a live failure blocked the frontier until someone killed the client.

*Truncate named its tail rewrite without composing it.* When the new size
falls inside a chunk, the bytes past it must be permanently forgotten and
must read as zeros if the file grows again. Publishing the tail after the
truncate commits leaves a window where the file is nominally shorter while
the old bytes remain readable. The zero-filled tail candidate is now written
first and **CAS-published inside the truncate transaction**; losing the CAS
retries the whole truncate.

*The profile cutover repeated the pre-round-7 fencing mistake.* A
control-plane Raft commit does not change what 4096 lane leaders believe, so
a leader still holding the old profile would keep accepting old-profile
publications after the "cutover" and the verification scan would be chasing a
growing set. The cutover is now a **barrier pushed to every publication
authority and durably ACKed**. Two related corrections: *lowering* f cannot
run the raise sequence backwards — the advertised guarantee must drop first,
or weaker data is written while the stronger guarantee is still promised —
and `u` is **protection debt, not a headcount**. A node returning restores
capacity, not the fragments it never received, so a degraded generation
consumes budget until repair actually rebuilds it; "unavailable" is likewise
a committed control-plane state, never a client's timeout.

*One factual Linux error.* Classic and OFD `fcntl` locks were described as
non-interacting namespaces. They are not: they differ in *ownership* and
release semantics but occupy **one record-lock conflict domain** and conflict
by byte range even within a single process on a single descriptor
(POSIX.1-2024 specifies this; Linux has behaved this way since 3.15).
`flock` is the genuinely separate domain. The round-8 ownership fix was
correct and is unchanged.

*The kernel is a serializer efs does not control.* Upstream Linux still takes
the inode lock exclusively for a direct write that extends `i_size` even with
`FOPEN_PARALLEL_DIRECT_WRITES`, forces it for `IOCB_APPEND`, and takes the
parent directory inode exclusively for `O_CREAT` — `FUSE_CAP_PARALLEL_DIROPS`
covers lookup and readdir only. These are *per-mount*, so 64 clients on 64
nodes are unaffected, but 64 ranks on one node sharing one mount serialize
before efs sees a request. It is now a second explicit unresolved
kernel-interface item next to syscall splitting, and §9 states the limit
rather than absorbing it into "efs scales".

*A hashed directory's fanout was not bounded.* Directory timestamp lanes were
claimed to be "bounded the same way a file's lanes are", but dentries hashed
over all 4096 shards, so a directory's used-shard set — and `stat(dir)` with
it — could reach 4096. A spread directory now uses the **same fixed 64-shard
permutation** as file lanes, which also makes its used-shard set a 64-bit
bitmap. Two consequences follow the file design exactly: first use of a lane
registers it on the parent shard (≤64 times per directory ever, so "the
parent shard is not involved" needed that exception), and directory
`utimens` needs its own `dir_mtime_gen` fence for the same backwards-time
reason a file's does.

*Smaller corrections.* "Only `utimens` can move a timestamp backwards"
assumed a monotone clock, which the failure model does not provide —
implicit updates are clamped by `MAX`, stated as a deliberate choice rather
than left as an assumption. Re-striping and repair must not touch
user-visible `size`/`mtime`/`ctime`; they change representation, not content.
The dedup ack watermark must be the highest **contiguous** acknowledged
reply, since out-of-order arrival would otherwise let reply 100 authorize
discarding 99's result — and a *fenced* session can never advance a
watermark at all, so its state is dropped wholesale after the revocation
barrier, with old-epoch requests rejected by epoch rather than by lookup.
L7 was broadened: truncate range-deletes and re-stripes create generations
that *were* published and are no longer reachable, which the old
"unpublished generations" wording did not cover.

### Sep 4 2026 — single-export decision

The old system supported multiple named exports (`efs-test` and `efs-s3`
coexisted on one cluster) because an export there was one slot in a static
array of in-memory tables. In the new architecture an export is a
replicated state-machine set (4096 Raft groups + a KV), so "create a
second export" is a real design decision, not an array index. The user
decided: **one export per cluster, hardcoded name `efs`**; the mount
target is `cluster:port:efs`; there is no create-export operation. The
escape hatch is preserved by construction: if a second filesystem is ever
required, it is one engine per export side by side — **never an
`export_id` in keys** — so the single-export key format is not a retrofit
trap. This closed the last open 10.5c design question. Production Raft
host in `efsd` (10.5c-9, env-gated `EFS_MD_RAFT`) is gated on a scratch
cluster; LOOKUP/GETATTR through ReadIndex + KV (10.5c-10) and file
CREATE as one Raft entry (10.5c-11) and MKDIR as a 2-shard txn
(10.5c-12) and last-link file UNLINK (10.5c-13) and mode/owner
SETATTR (10.5c-14) and empty LOCAL RMDIR (10.5c-15) and LINK
(10.5c-16) and nlink>1 UNLINK (10.5c-17) and utimens
(10.5c-18) and same-dir LOCAL file RENAME (10.5c-19) and
READDIR/LOOKUP_PATH (10.5c-20) and SETATTR SIZE / chunk-aligned
truncate (10.5c-21) and chunk publish + GETCHUNKS (10.5c-22) and
unaligned truncate tail CAS (10.5c-23) and cross-group propose
(10.5c-24, MKFS submit + inode bounce, no new opcode) and
O_APPEND reserve (10.5c-25, resolve-on-report) and SYMLINK as
CREATE S_IFLNK + publish (10.5c-26) and same-dir LOCAL directory
rename (10.5c-27) and HASHED dest CREATE (10.5c-28) and HOLD
open-unlinked leases (10.5c-29) and non-blocking FLOCK
grant/release (10.5c-30) and non-blocking whole-file fcntl
(10.5c-31) and non-blocking fcntl byte ranges (10.5c-32) and
F_GETLK as a leader read (10.5c-33) and blocking lock waits
(10.5c-34, FIFO leader queue, grant is the reply, leader loss
re-issues) are gated on the same
smoke. What remains is cutover of the live table — not new
design. Session fencing is a later host
item.

### Sep 8 2026 — Raft election livelock (pre-existing) fixed; a parked multi-leader finding

Gating 10.5c-34 surfaced a **pre-existing Raft election livelock**: a
3-voter group could fail to elect a leader indefinitely. Root cause was
deterministic, not a race — every node had a **fixed** election timeout,
and a behind-log candidate with a shorter timeout could livelock an
up-to-date follower by repeatedly resetting that follower's timer through
`maybe_step_down` faster than the longer timer could ever fire. The smoke
"kill the leader, re-elect, the blocking waiter re-issues and grants"
sequence wedged on it.

The fix is the textbook one, made determinism-preserving: the election
deadline is **redrawn at random from `[election_ticks, 2*election_ticks)`
on every reset** (`reset_election` in `raft.c`, a splitmix64 PRNG). The
PRNG is **injected, not `rand()`**, so the deterministic simulator stays
replayable: `efs_raft_cfg.rng_seed` carries the seed — the host seeds it
from entropy (`/dev/urandom` via `h->salt`, per `(node, group)`), the sim
uses a deterministic per-`(id, boot_id, group)` fallback. Production also
moved to a **uniform** base timeout (`HOST_ELECT_BASE`, stagger removed) —
with randomization, a stagger is unnecessary and a non-overlapping stagger
could not have recovered from the split-vote class anyway.

**Sim config kept staggered, and why.** The simulator was switched to the
same uniform base as production, and three cross-shard mkdir transaction
tests failed — **not** an I1/split-brain safety violation, but a
data-consistency failure (`efs_meta_apply_check` on the leader's KV) plus
lost dentry visibility. The trigger is novel: the old per-group stagger
(`4+i*4` / `5+i*3`) made **node 0 the leader of every Raft group**, so the
cross-shard transaction tests had *never* run with the two metadata groups
led by **different** nodes. Uniform timeouts elect different leaders and
expose the gap. This is a **separate, high-priority investigation** (a
cross-shard txn that is correct only when its groups share a leader is a
real protocol concern, or a sim-driving limitation — either way it must be
understood before the sim can run multi-leader). It is **parked**: the sim
keeps the stagger (node 0 leads every group, existing tests green) while
`raft.c` still exercises the randomized-deadline code path within each
band. Do not re-litigate the stagger removal until the multi-leader txn
consistency is root-caused.

Gate: full raft-affected unit set green on a node (`test_raft`,
`test_raft_store`, `test_meta_apply`, `test_sim`, `test_txn`,
`test_session`, `test_kv`, `test_kv_lsm`, `test_wire`, `test_data` OK);
`raft_host_smoke` PASS (`results/raft-smoke/34h.log`), including the
after-crash leader-kill + blocking-waiter re-issue that the livelock had
blocked. **Unrelated pre-existing bug found while gating (not from this
change, present on HEAD):** `test_lock` has 6 `efs_lock_getlk` (F_GETLK)
failures — a latent bug in the committed 10.5c-33 F_GETLK path; worth its
own fix.

### Sep 9 2026 — 10.5c-35a session record hosted

The session SM (`session.c`) and in-sim barrier were already done;
the production host only applied LEASE_OPEN/CLOSE/RECLAIM and
silently no-op'd CREATE/REGISTER/ESTABLISH. 10.5c-35a hosts those
three applies (same bytes as the sim) and a ReadIndex GET (sub=0,
not a log command) via the existing `EFS_MSG_RAFT_MKFS` submit.
No new opcode. The revocation barrier became 35c.

### Sep 9 2026 — 10.5c-35b real session identity on HOLD/FLOCK

HOLD/FLOCK carried a `uint64_t` owner stand-in (zero UUID + epoch 1).
10.5c-35b puts the real `(uuid, epoch)` on the wire as an optional
`EFS_SESS_WIRE_LEN` (20-byte) suffix on `efs_msg_inode_hold` /
`efs_msg_inode_flock` (after the struct, or after the range suffix);
absent keeps the stand-in, so pre-35b FUSE and the existing smoke
checks are untouched. The host runs `efs_session_accept` on the
inode shard before proposing (not GETLK — a read); a wrong or
not-established epoch is BUSY. mgmt `raft-hold` / `raft-flock` /
`raft-fcntl` take `[uuid-hex epoch]`. Gate: scratch smoke — an
established uuid is accepted, a wrong epoch is BUSY, the stand-in
path still works, and the session record survives the leader kill.
The revocation barrier became 35c (hosted below).

### Sep 9 2026 — 10.5c-35c revocation barrier hosted

The session SM already had BEGIN / FENCE_LOC / ACK / FINISH /
LEASE_DROP; the production host no-op'd them. 10.5c-35c hosts those
applies (same bytes as the sim) and a coordinator-driven walk:
mgmt `raft-session fence` BEGINs, reads the frozen `touched_shards`
bitmap (GET with shard≥4096 returns one 64-bit word), FENCE_LOCs
every set bit, ACKs, FINISH, then LEASE_DROP of the old epoch on
each touched shard. FENCE_LOC also dequeues in-memory lock waiters
keyed by `(uuid, epoch)` so a fenced waiter is never granted
(BUSY/STALE). Gate: scratch smoke — waiter of epoch 1 is BUSY after
fence, epoch 1 HOLD/FLOCK is rejected, epoch 2 is accepted after
establish, GET after crash is ACTIVE at epoch 2. Append-reservation
reclaim is 35d (hosted below).

### Sep 9 2026 — 10.5c-35d append-reservation reclaim on fence

A fenced session's OPEN `O_APPEND` reservations must resolve as
`FENCED_HOLE` (committed zero hole; the frontier advances) so a
later appender is not stuck behind a dead client's watermark.
10.5c-35d scans the shard's reservation prefix on `LEASE_DROP`
and resolves matching `(uuid, epoch)` OPEN rows. Production
`raft-append` takes an optional `(uuid, epoch)` wire suffix
(same layout as HOLD); seq stays 0 so tagging does not enable
the op-id window. Absent suffix keeps the zero-UUID stand-in.
Gate: scratch smoke — reserve 128 KiB under epoch 1, getattr
stays frontier 0, fence, getattr is 131072, old-epoch append
is BUSY.

