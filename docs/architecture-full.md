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
| 4K random updates in 128K chunks | full chunk image per span; disjoint spans do not CAS (§7.3) |

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
production adoption for the single export. **Status (Oct 1 2026): steps
0–12 are landed.** The 10.5c slices (listed one by one with their gates in
[arch/verification.md](arch/verification.md)) were gated behind an
`EFS_MD_RAFT` flag on a scratch cluster; step 11 (Sep 11) removed the flag
and deleted the old snapshot / root-2PC engine, so the Raft+KV engine is
the only metadata engine and every FUSE metadata op is a Raft proposal.
Current gates: posix suite 1 200/201 (one `MAP_SHARED` SKIP by spec),
posix2 63/63, 9-host suite 200/201 on every host. The KV engine is
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

**Step 1 had a hard prerequisite: the carve-up (Phase M, complete).** The
tree is `raft/ kv/ meta/ wire/ data/ client/ server/` behind interfaces
([development.md](arch/development.md)); the simulator reuses the same
state machines production runs. Build nothing as new monolith code; every
new component lands inside the carved boundaries.

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

**Where the project is (Oct 1 2026).** [architecture.md §10](#architecture)
steps 0–12 are landed and gated: simulator, KV, Raft, cross-shard txns,
sessions, directory spread, delete-2PC, FUSE A–D. The Raft+KV engine is
the only metadata engine (Step 11, Sep 11). Snapshots are by log bytes
(512 MiB, W22); L1 compaction is a background thread (W13); InstallSnapshot
is chunked (W11). There is no next §10 step. What remains is the work
queue in [§1a](#1a-the-work-queue): measured gaps, in order.

**How to pick work.** Take the lowest-numbered open item; correctness
before performance. Each item names what to change, how to measure it,
what proves it, and what is forbidden. If an item needs a decision the spec
does not contain, **stop and ask** (§4). Decisions already taken (D1–D13,
D18–D20, D24) are listed under "Decisions — taken and pending" in §1a and
are to be implemented, not re-asked. Two plans are not queue items until
asked: `efsd --bench` and `efs-fuse --bench` (§1a, "plan only"). The
encryption idea (§1a) is not a decision. **Asked Oct 1:** W26, the FUSE
`fallocate` handler.

**Before touching the cluster** run `tests/preflight.sh` (the deploy
rule's pre-flight as one command). Stop/start is
`tests/cluster.sh stop|start|restart [--clients] [--perf] [--strace]`;
a fresh table after a wipe is `cluster.sh start --fresh`. Runbooks for
the open measurement items are in [runbooks.md](runbooks.md)
(`tests/measure/*.sh`).

**The one live cluster is port 19810** on fcstor003–006 (`/data1/01–06/efs`,
`--quota 36T --direct-io`, RDMA), clients fcstor003–015 at `/tmp/efs-mount`,
the user's own client on fstor007 via `scripts/client.sh`. 19820 is retired.
Do not `wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs` without being
asked. The live state (build, leaders, what is deployed where) is the
cluster fact in `.cursor/rules/efs-project-state.mdc`, not this page.

#### 1b. In flight — finish this before taking a queue item

Whoever picks the project up next does **this first**. Update or delete
this block when done — an "in flight" block older than the last commit
is a bug in this page.

**Oct 1 18:00Z: committed `dc6b0af1` and rolled — `cluster.sh restart --clients --perf` (`~/efs-runs/restart705.log`, CLUSTER_OK): servers `dc6b0af19832-dirty` on fcstor003–006 with `--perf` (`/tmp/efs-perf/efsd.data`), clients fcstor003–015 remounted RDMA, fstor007 untouched. Post-check (`post706`): PREFLIGHT_OK, g0 term 17 leader fcstor003, g2 term 12 leader fcstor006, commit==applied, both idle; fcstor007 2 GiB write 1.4 GB/s, cold read 3.2 GB/s, tail cmp OK; server log stamped and `store: put O_DIRECT zero-copy` active; client `read reply zero-copy active`. The user does the heavy testing on this build.** Earlier that afternoon (15:55Z): W39 (RDMA zero-copy send), read path R1–R5, 64 get workers, the server's PUT bounce copy and the W37 mkfs guard were put in the tree, clients deployed first, servers later. The user asked for the performance items from the roadmap, implemented and smoke-tested, heavy testing his.
- **Client, deployed to fcstor003–015 (`dep680`) and fstor007's `/tmp/efs` (the user's running efs-fuse is untouched):** (1) **W39** — `efs_rdma_zc_region_add` registers the bufpool slabs lazily per HCA (`zc_lkey`, `ibv_reg_mr` once per slab per device, append-only region table), `efs_rdma_send_frame` posts a two-SGE send (pool header, slab payload ≥ `EFS_RDMA_ZC_MIN` 4 KiB) when the QP reports `max_send_sge ≥ 2`; the payload stays referenced until the send completes, which every caller already guarantees (a conn ends replied or `efs_conn_destroy`ed; `efs_rdma_send_quiesce` reaps for the destroy path). Log once `efs: RDMA zero-copy send active`. (2) **Read path R1–R5:** a prefetched chunk's buffer is handed to the rdcache instead of copied (`efs_rdcache_put_owned`); a whole-chunk demand read decodes into the caller's buffer (`chunk_get_job.ext`); the two data fragments are received directly into the chunk and `efs_decode_chunk` skips the self-copy; a chunk-aligned READ whose chunks are all in the rdcache is answered with `fuse_reply_iov` over pinned images (`efs_client_read_refs`, `efs_rdcache_pin/unpin`, `rdcache_ent.pins` excludes a pinned way from victim selection and from `rdcache_put`'s overwrite; `fuse_reply_data` with a multi-buffer bufvec copies in libfuse 3.10, `fuse_reply_iov` does not). Log once `efs: read reply zero-copy active`. (3) The get pool is 64 workers (`GET_POOL_N`), no longer tied to `EFS_WRITE_PIPELINE` — one synchronous fetch per worker is the client's in-flight read cap. (4) **`rmdir` no longer fails ENOTEMPTY from the client's local table** (`efs_client_unlink`): the local child list can hold a child whose removal committed elsewhere, and the refusal was the recurring one-host `dir_deep_nesting` 199/201 (fcstor012 Sep 30 `d50`, fcstor009 Oct 1 15:55Z `d25`, `results/posix/20261001-155455`; the server's row was empty both times). The client logs `efs: rmdir ino=… local table lists child '…' — asking the server` (first 16) and the server (`EFS_INODE_RPC_NOT_EMPTY`) decides. After it (`dep691`, all 13 clients + fstor007's tree): 9-host **200/201 on all nine** (`results/posix/20261001-160049`), jobs=1 200/201 (`-155932`).
- **Measured on fcstor007 (16 GiB random file, cold remount before every read, `agent-rd-20261001-145047` → `-154007-refs`):** write 1.4 → **1.5 GB/s** (client CPU 21.4 s), single cold read 2.5 → **3.6 GB/s** (client CPU 21.6 → **10.8 s**), four readers 3.7 → **6.5 GB/s** (14.9 s CPU); `EFS_READ_PREFETCH=32` is slower than the default 16 (2.9 GB/s) — leave it. cmp at 0/5/15 GiB, whole-file md5, unaligned and tail windows cold and warm all OK (`gate671`; gate670's `CMP_TAIL_BAD` was `dd skip=` on a pipe without `iflag=fullblock` — the harness, not the client). posix jobs=1 fcstor007 **200/201** (`results/posix/20261001-154333`); the D24 wedge gate again: 4 × 8 GiB dd+fsync 8.2 s, storm p50 5.6 / p99 70 / max 112 ms, 32 `report-landed`. The remaining user-space copies in the read profile (`agent-rd-20261001-153356-prof`) are RDMA recv buffer → chunk (zero-copy *receive* needs per-request posted receives — a transport change, **ask**) and the kernel's own copy out of the iov. Single-stream is in-flight bound (~1 ms per chunk, 8 demand + 16 prefetch), not CPU.
- **Server, rolled at 18:00Z (`dc6b0af1`):** `store.c` writes an aligned PUT with a digest as one `writev` (payload + the 4 KiB D20 tail page) — the D20 bounce memcpy per fragment is gone (`store: put O_DIRECT zero-copy` once-log); **W37** — `server_raft_host_mkfs` replicates the salt the committed MKFS recorded (read back with `efs_meta_apply_export_salt`), never its own; a node that holds no salt record answers BUSY; SALT verdicts ride the ring (a mismatch is PROTO, not an apply halt). The mkfs apply itself is unchanged (no-op on an existing root). Compiled with `test_sim` and `test_meta_apply` passing (`bld701`, 17:56Z). Plus the 14:00Z log timestamps. A fresh-table start after the next wipe is W37's gate.
- **Asks, unchanged:** W41 (`report_mu`), D23 (clean-image cache), W40 (FUSE write copy), the fragment-layout change (needs a wipe), RDMA zero-copy receive, D17, W36, W38.

Older handoff blocks (Sep 28 – Oct 1 14:00Z) were moved verbatim to
[project-history.md](../project-history.md) under "START-HERE handoff
archive" on Oct 1 2026. A `§1b block` reference inside the queue below
points there.

#### 1a. The work queue

Rules for this queue: **take the lowest-numbered open item.** Do not start a
later item to avoid a harder earlier one — the order encodes a dependency and
a severity: **correctness items come before performance items**, because a
parallel filesystem that returns wrong bytes has no throughput number worth
reporting. Do not batch two items into one change. Every item ends with
`Forbidden`, which is binding.

**What the hardware allows.** Every performance item is measured against
this, not against last week's number. Cluster traffic rides `ibs1f0`
(200 Gb/s IPoIB, `ip route get 172.16.223.57` on a client); servers have six
NVMe each.

| ceiling | value | derivation |
| --- | --- | --- |
| one client, logical write | **~16.7 GB/s** | 25 GB/s line rate ÷ 1.5 (2+1 EC sends 3 fragments per 2 data) |
| one client, logical read | ~25 GB/s | line rate; a read fetches k=2 fragments |
| cluster, logical write | **~44–57 GB/s** | 4 hosts × 16.7–21.4 GB/s NVMe (`results/nvme/`) ÷ 1.5 |
| 1-client honest write today (Oct 1) | ~1.3–1.5 GB/s | **~8–9 %** of the client's ceiling (8 GiB dd+fsync 1.3 GB/s; 16 GiB 1.5 GB/s) |
| 9-client honest write today (Sep 28) | 2551–2810 MiB/s aggregate | **~6 %** of 44 GB/s (8 GiB dd+fsync, 9 own files) |
| 1-client cold read today (Oct 1) | 3.6 GB/s; 4 readers 6.5 GB/s | ~14 % of the client's ceiling (16 GiB, remount before read) |

[architecture.md §1](#architecture) says: if a benchmark stops at a
mutex, one leader, one thread, FUSE serialization, one WAL or one
coordinator before a physical resource, *that is by definition an EFS bug*.
By that rule the write path is still a bug, not a tuning task.

Baselines, all honest (flush in the clock, reads after remount, `findmnt`
verified `fuse.efs-fuse`). The full history of these numbers is in
`.cursor/rules/efs-fio-honest.mdc`.

| measurement | value | where |
| --- | --- | --- |
| 1-client 8 GiB `dd bs=1M conv=fsync`, fcstor007 | **1.3 GB/s** (6.80 s) | §1b 13:20Z block (`dd539`), Oct 1 |
| 1-client 16 GiB write / cold read / 4 readers | **1.5 / 3.6 / 6.5 GB/s** | §1b 18:00Z block (`agent-rd-20261001-*`), Oct 1 |
| 9-client 8 GiB dd+fsync, aggregate | **2551** MiB/s (best 2810) | `results/measure/20260928-134637-dd-prof-r5b`, `-131651-dd-prof-r2b` |
| per-host local NVMe ceiling | 16.7–21.4 GB/s | `results/nvme/` |
| IO-500 9×4 debug, fresh table | easy-write **5.173 GiB/s**, hard-write 0.519, mdtest-easy-write 6.185 kIOPS, hard-read 1 error (W38) | `results/io500/20261001-074905-rdma/` |
| IO-500 9×4 debug, 0 errors | easy-write 4.563, hard-write 0.640, hard-read 1.034, cold hardscan bad=0 | `results/io500/20260930-183504-rdma/` |

**Never quote intra-job fio write samples or `dd` progress lines** — those are
pre-flush and read several GiB/s. The number is bytes ÷ wall with the flush
inside. `dd if=/dev/zero` is also invalid here: all-zero payloads skip PUTs.
Use a non-zero source file.

##### Single-node storage bench — plan only (Oct 1). Do not implement until asked.

**Why this goes before the client tests (Oct 1 04:50Z).** The cluster write wall is ~2.5–2.8 GB/s logical with nine clients (`results/measure/20260928-134637-dd-prof-r5b`): ×1.5 for 2+1 EC ≈ 4.2 GB/s of 64 KiB fragments ≈ **16 K fragment writes/s per server**, each a file create plus an `O_DIRECT` write on XFS. That is an ordinary filesystem-metadata rate, not the 16.7–21.4 GB/s the `results/nvme/` fio shape measures, and only a bench that calls the fragment store the way the PUT handler does can say whether it is the number. Same for metadata: 4.6 kIOPS mdtest-easy-write across two groups against a 0.4 ms Raft-log `fsync` is an inference that the disk is not the limit; `--bench-meta` makes it a measurement. The bench needs no cluster, no clients, no quorum, no network, no wipe, and none of the things that have confused every cluster measurement (strace on the daemon, elections, a dead mount). **What it will not find:** the ~1 GB/s single-client wall — every client profile shows the server near idle; that one needs the client-side ladder (04:30Z §1b block: one dd, Little's law on one reclaim thread, then a `/dev/shm` private cluster), which comes after this.

**What.** One server, its six NVMe, the code `efsd` already uses, driven until the disk is the limit. Two workloads: data-fragment read and write, and metadata read and write (KV segments, the KV WAL, the Raft log — metadata is not an EC fragment). The same run records the hot path, so the next edit is the symbol that is on CPU while the disk is not full.

**What exists.** `efsd --bench <path>` (`src/server/bench_local.c`) `pwrite`s zeroed `EFS_FRAGMENT_SIZE` buffers into one `.efs-bench` file, rotating 1024 offsets, then deletes the dir. No `fsync`, no read, one path, and it never calls the fragment store, the writer pool, the KV, or the Raft log. That number is not a storage-engine result. **Replace it; do not keep it as a second line next to the real one.** `results/nvme/` is the per-host ceiling (16.7–21.4 GB/s) and is the bar.

**What to add.**

1. **Ceiling, same host, first.** The `results/nvme/` fio numbers for that host are the ceiling; re-run only if the host is not in that dir. Four numbers: sequential write with the flush in the clock, sequential read, and the metadata shape (small synchronous writes, random reads of a segment block). Print them in the bench output. The engine run is judged against them.
2. **Data, through the engine.** `efsd --bench <kind> --storage <p1>[,<p2>,…]` — the same comma list `efsd` takes, so the bench runs on **1, 2, 3, … 6 paths from one invocation set** and the output shows how the engine scales from one drive to many (one drive at the fio ceiling and six at 1.3× is a serialization point in the writer pool or the store, not the disks). Kind `data`: call the fragment store and the writer pool exactly as the PUT handler does — fragment name, the probe/`path_hint` logic as shipped, create, `O_DIRECT` write of a **non-zero** payload, whatever `fsync` policy PUT applies, and the publish/rename step if the store has one — then read those fragments back the GET handler's way (`O_DIRECT`, so it is not the page cache). No cleaner path than production: the gap to fio is only meaningful if it is the engine's own.
3. **Report latency at fixed queue depth, not only GiB/s.** For each path count and for QD 1, 16, 64, 256 per path: fragment ops/s, p50/p99 latency per op, and aggregate GiB/s. A client has 16–32 chunks in flight; what it sees is the latency at that depth, and `in-flight × 64 KiB ÷ latency` on the server side either matches the 2.8 GB/s cluster number or does not. A GiB/s-only number at unlimited depth hides the problem the system actually has.
4. **Metadata, through the engine.** Kind `meta`, on `--meta-storage`. Drive `efs_kv_lsm_*` and `efs_raft_disk_*` with sync on (the simulator's `EFS_KV_LSM_NOSYNC` / `EFS_RAFT_DISK_NOSYNC` is the wrong tool): puts that cross the memtable and compact, Raft appends that `fsync` at QD 1 and batched, then point gets and a log reopen. Report MB/s, ops/s and p50/p99 next to the small-IO fio lines; include the compactor segment `fsync` time (the 100 ms mode of D11). Flush is in the clock; bytes must show up in `du`.
5. **Hot path, same process.** `--perf` on that run (attach or the existing efsd flag; do not start `efsd` under strace). One line in the bench output per run: the top symbols and whether the wall was disk (`iostat` util per device) or CPU. A change that moves neither the GiB/s nor that line is not a result.
6. **Where.** One fcstor, no quorum, no clients, no port 19810, no wipe. Bench dirs under the `/data1/0N/efs`-style paths of a node that is not serving 19810 are off limits while it serves; use an unused directory on the same devices or a node the user names. Screen on that host (`EFS_RUNNER=fcstor0NN.ib`). Build in `/tmp/efs` on the node.

**Done when.** One log has the four fio ceilings, then for 1 and 6 paths (and the steps between) the engine's data write and read ops/s, p50/p99 at QD 1/16/64/256, aggregate GiB/s, and the meta read/write lines, each beside its fio ceiling, with the top symbols and `iostat` util. The 1-path → 6-path curve is part of the result. Anything short of the ceiling names the symbol to change next. Do not tune from the current `--bench` line, and do not add an IO path the server does not already have.

##### Client bench `efs-fuse --bench` — plan only (Oct 1). Do not implement until asked; after `efsd --bench`.

**Why.** The single-client write wall (~1 GB/s whatever the transport, host, stream count or build; §1b 04:30Z) has never been located: no measurement separates the kernel FUSE path, the client pipeline, the per-fragment round trip and the client's own arithmetic. `ll_write` is a shim around `efs_fuse_write(NULL, buf, size, off, fi)`, and open/flush/fsync/release are the same kind of shim, so the whole client write path runs in-process with no kernel and no `/dev/fuse`. Four levels, one variable apart, replace the dd ladder:

| kind | what runs | what it isolates |
| --- | --- | --- |
| `cpu` | no network, no server: synthetic non-zero 128 KiB chunks → dcache store → blake3 + XOR parity + the RDMA bounce copy into a registered buffer, then drop | the client's arithmetic ceiling per thread and its scaling with threads (`memmove` + `blake3` + `xor_into` are ~11 % of today's profile; this says what they cost at full speed) |
| `put` | `efs_client_put_fragments_parallel` straight to the servers at a fixed queue depth; no FUSE, no dcache, no REPORT | the per-fragment round trip the client sees: fragments/s, p50/p99 at QD 1/16/64/256, GiB/s — the client side of Little's law. Against a `/dev/shm` private cluster it is network + handler + poller only; against NVMe it adds the store, which `efsd --bench` measured alone, so the difference is attributable |
| `write` | `efs_fuse_create` → `efs_fuse_write` 1 MiB at a time → `efs_fuse_fsync` → release, in-process, N files in parallel — the `ll_*` handlers minus the kernel | the full client pipeline (dcache, reclaim/put pool, dirty-byte throttle, REPORT) with the flush in the clock; the gap to `put` is the pipeline's own cost |
| dd through the mount | the number we have (fcstor007 1000 MiB/s, 32 GiB + fsync) | the gap to `write` is the kernel FUSE path: request dispatch, the `fuse_buf_copy`s, `max_write`, libfuse worker count — W15's splice/`max_write` questions become measurable |

Reading: `cpu` ≫ `put` ⇒ latency-bound (more in flight or a shorter RTT; not more poller threads); `put` ≈ `write` ≈ dd ⇒ the RTT is everything; `write` ≫ dd ⇒ the kernel FUSE path is the wall; `put` on `/dev/shm` ≈ `put` on NVMe ⇒ the store is not in the client's RTT.

**Rules, or it is another `--bench` that lies.**

1. **Production path only.** `efs_fuse_write` / `efs_fuse_fsync` and `efs_client_put_fragments_parallel` as they are: no bench-only shortcut, no skipped hash, no skipped REPORT in `write`. A hook the handlers do not have means the bench is measuring something else.
2. **Honest clock.** `write` counts bytes after fsync/REPORT returns; `put` counts a fragment on its reply. Non-zero payload (all-zero chunks skip the PUT).
3. **Fixed queue depth, latency reported** — ops/s, p50/p99, GiB/s per QD (1/16/64/256) and per thread count, like the server bench. The dd ceiling is `in-flight × 128 KiB ÷ latency`; print both factors.
4. **Never against 19810's export.** `put` makes fragments no row references; `write` makes real files. Target the private 3-node cluster on fcstor007 (`tests/rdma_first_inode.sh` layout, ports 19950–19952, storage on `/dev/shm` or a scratch dir) or a bench export the user names. No wipe, no roll.
5. **Untraced.** `perf record -g` on the bench process for the top symbols; never strace.
6. **One host, then two.** The same `put` / `write` from two hosts against the same servers separates the per-client wall from the cluster wall (4 clients = 578 MiB/s each, 9 = ~290 each today).

**Where in code.** A `--bench` entry in `efs_fuse.c`'s `main` before `fuse_session_new`; a driver loop per kind; a latency histogram; a non-zero chunk generator. The heavy lifting is existing functions.

**Done when.** One log per host count has the four levels side by side (dd from the honest-fio rule's run dir), each with ops/s, p50/p99 at the four depths, GiB/s, thread count, and the top symbols; the two largest gaps between adjacent levels name the next item. Forbidden: a cleaner code path than the handlers use, a GiB/s number without its queue depth and latency, zero payload, running against port 19810's export, tracing the bench process with strace.

##### Decisions — taken and pending (Sep 28 2026)

Each row is a choice the spec did not make. A row marked **decided**
was accepted by the user and is now part of the design; implement it
per the item it points at. A row marked **done** is history. Nothing
in an open row is implemented until the user asks. W13 is done; its
pinned segment view is the primitive W11's steps use.

**Decided Sep 28 2026 (user accepted the recommendations):**

| item | question | decision (Sep 28) | why, in one line |
| --- | --- | --- | --- |
| **D1 · W6.1 / W17** | what do N-1 shared-file writes do under conflict? | **A span publish commutes; no lock; no CAS on the base generation for spans.** A sub-range writer always publishes a span (even when it holds the base). `expected_gen` applies to full-image publishes only. The trailer keeps the folded spans' `candidate_gen`s (≤ `EFS_CHUNK_DELTA_MAX`) so a replay of a folded span is a no-op. The fold is done by the publisher that fills the last slot, or by a reader — never on another rank's `fsync`. | the code already has commuting non-overlapping spans; what collapses at nine ranks is the base-gen check *before* the span path (one fold STALEs every in-flight span) and the `have_base → full CAS` branch. This is §7.2's commutative reduction applied to the chunk map, and P1 (no serialization point). A distributed chunk lock stays forbidden. Gate in W17 |
| **D2 · W6.2** | may `open()` return before the chunk map is known; how big is the map window? | **Yes. `open()` adopts the inode row only.** Chunk maps are pulled per lane group, in parallel, by `pull_layout_miss` in a metadata window that runs one data window ahead of the prefetcher. Window size is internal and adaptive (start at the prefetch depth, grow while the read stays sequential). No mount option, no environment variable. | `performance.md` already says reads fetch chunk maps in per-lane windows; the code pulls the whole map sequentially at open and the server serializes 36 openers to 14.6 ms per GETCHUNKS. A row without a map is already the evictor's steady state (W9); a miss is pulled and an error is an error, never a zero-fill (I9) |
| **D3 · idle gate** | may the 5/s REAP_DONE gate be raised to remeasure ior-hard 1/9/36? | **No.** Measure after the reaper drains. If it does not drain, that is a bug to file. | the tail is the reaper clearing deleted IOR data, and a measurement started during it measures the reaper |

**Decided Sep 29 2026 (user accepted D4–D8 as written; source
`results/io500/20260929-023447-iorperf2/ana`). Implement per the item
each row points at; do not re-ask. D8 is a measurement whose result
comes back to the user before any budget changes.**

| item | question | decision (Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D4 · W22 step 1** | when does a group take a snapshot and truncate its log? | **By log bytes and follower reach, not every 256 entries.** Take a snapshot only when the log since the last one exceeds a byte budget (recommend 512 MiB — the export itself is 2.67 GB and takes 5–12 s, so anything smaller makes export the steady state), and keep a trailing window of log entries after the snapshot point so `send_ae` serves a follower that is behind by less than that window from the log. Send InstallSnapshot only when `next_index` is below the retained window | `HOST_SNAP_MIN` = 256 applied entries triggers a 2.67 GB export back to back on every node; `raft.c:672` sends the file to any peer more than 256 entries behind; three imports of 12–16 s happened in one 4-minute IOR and each one is a follower that answers nothing while its pump applies the diff |
| **D5 · W22 step 2** | may a follower answer heartbeats while its pump applies an InstallSnapshot diff? | **Yes — slice the diff.** Apply the import diff in bounded slices between pump cycles (one `HOST_TICK_US` worth of keys, then drain the inbox and answer the heartbeat), instead of one 12 s apply. The follower stays a follower; the leader keeps its term | this is W14 step 2's "heartbeat answered from a thread that is not importing", with the evidence: terms moved +45 / +105 across the run with `drop=0` on every outbox, so the outbox was not the trigger; the import-diff apply is the only multi-second pump hold left (`apply_max` 1.17 s on fcstor006, `pump_hold_max` 4–5 s on fcstor005 in the earlier run) |
| **D6 · W22 step 3** | may `mdraft/` (Raft log, KV WAL, segments) live on a root the fragment writers do not use? | **Yes, and measure first.** One `dd` on a quiet node: `fsync` of a 4 KiB file on `/data1/01` while the writer pool creates fragments there, versus on a root with no writers. If the shared root shows the 100 ms mode, give `efsd` a `--meta-storage <root>` (default: first root, today's layout) and point 19810 at a root the six `--storage` paths do not include (or a seventh partition) | the compactor's segment `fsync` was a flat 100 ms 97 times and `persist_max` reached 254 ms while twenty writer threads did `openat`/`write`/`close` on the same XFS; the Raft commit path and the data path share one journal |
| **D7 · W14 step 4** | may the client tell the server a PUT is the first write of `(ino, ci, fi, gen)` so the server skips the six-root probe? | **Yes, with a sentinel, not a guess.** `path_hint = 0xffffffff` means "the client has never PUT this fragment generation"; the server then creates on the writer's least-queue root with no `access()`. Any retry of the same generation (REPORT STALE replay, failed reply) sends the real hint or 0. A fragment name includes its generation, so a first write of a new generation cannot collide with an existing file | the hint from a previous PUT can only help a re-PUT; IOR-easy and a fresh `dd` write every chunk once, so 1.59M `access()` (192 s across six handler threads on fcstor004) survived W14.4 unchanged. The step-4 gate ("under 1 % on a single-client dd") cannot be met by the hint as specified |
| **D8 · W16 steps 2–3** | how does a 86 234-record REPORT reach the log? | **Measure the per-batch commit latency first, then bring the number.** `report-split` shows `push_ms=9869` for 337 publish batches — ~29 ms per batch through the one in-flight batch per peer — and `finish_ms=9227` waiting on the last apply. Add `pub_batch_ms` (p50/max) to `raft-obs`; if the per-batch commit is the ~2–6 ms a Raft round costs here, the 29 ms is queueing behind other clients' batches and the answer is fewer, larger entries per REPORT; if it is the 100 ms `fsync` mode, D6 is the fix. Pipelining past one in-flight batch stays forbidden | the client's fsync waited 19.4 s in one TCP `recvfrom` for a server that was still pushing; nine clients × ~80K records at the observed rate is minutes of leader time per stonewall |

**Pending Sep 29 2026 (from the 04:07–04:27Z trace, `results/measure/20260929-040800-idle-trace/ana`; §1b has the numbers). Recommendations, not decisions: bring them to the user. D9 and D10 are the two halves of one problem — the KV cannot absorb writes as fast as the Raft log commits them, and the wait lands on the pump.**

| item | question | recommended (D9–D11, Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D9 · W13 step 2, W23** | may the apply path (the pump) block on L0 back-pressure? | **No. The pump never waits for the compactor.** When L0 is within `KV_LSM_RANGE_MAX` of the cap, the apply keeps writing the memtable and lets it grow past `memtable_max` (memory, bounded by what the Raft window can commit — 512 MiB of log is the ceiling since W22.1); back-pressure moves to admission on the leader: `host_propose` for REPORT/publish batches returns BUSY while the local L0 is over the cap, so the client retries with its existing budget and the follower's pump is never the one that stalls. Heartbeats and AppendEntries replies do not depend on the KV | the pump waited 4.1 / 1.7 / **24.2** / 2.7 / 3.6 s in `kv_maybe_flush_locked` on fcstor004 in one 7-minute write, each wait one compaction long; every wait produced `apply-sleep` 400 ms timeouts → REPORT `rc=-13` → client fsync EIO, and the two term changes of the run. Same class as W13 (a lock hold the apply does not need) and W22.2 (a follower must answer while it imports) |
| **D10 · W23** | how does L0 reach L1 so that a 7-minute write does not rewrite the table thirty times? | **Compact by bytes, not by file count, and split range 0.** (a) Merge a range's L0 into its L1 only when that range's pending L0 bytes are at least a fraction of its L1 bytes (recommend 1/8: a 250 MB range waits for ~30 MB of L0, a 1.8 GB range for ~220 MB), otherwise let L0 files accumulate — the cap that matters is bytes in L0, not 64 files; reads already probe every L0 (`kv_seg_probe`), so make the L0 cap a byte budget (recommend 1 GiB) and drop the 64-file cap. (b) Partition on more than `key[0]` where a range is large: range 0 is 1.8 GB against 190–330 MB for the other fifteen; split it by the next key byte at flush and compaction time so no range exceeds ~256 MB. (c) The merge reads input blocks one `pread` at a time (3.7M of ~8 KB): read each input segment through a 1 MiB sequential buffer | 433 compactions wrote 159 GB to keep a 5.3 GB table current through one 7-minute write; the compactor is 48–55 % of two servers' samples; range 0 alone is 61 GB of the 159 and the 24 s pump hold. `inputs=` 5–11 files of ≤256 KB each per rewrite of 200–1800 MB is write amplification of several hundred. W13 step 5's partitioned flush bounded the rewrite to one range; it did not bound how often a range is rewritten |
| **D11 · W22 step 3 / D6** | is `mdraft/` sharing an XFS with the fragment writers the 100 ms `fsync` mode? | **No — close D6 as "not the sharing", skip the `dd` measurement, do not move `mdraft/`.** The pump's Raft-log `fsync` averaged 0.38–0.40 ms on all three servers measured (61 228 of 61 668 under 2 ms on 004) while twenty writers created 408K fragments; the 90–120 ms `fsync`s are the compactor's own 200–300 MB segments (299 of 695 on 004, 407 of 733 on 005). `--meta-storage` stays as an option; the compaction fix (D10) is what removes the 100 ms mode and the 130 MB/s the compactor puts on `/data1/01` | per-thread `fsync` histograms in `idle-detail2-fcstor00{3,4,5}.txt`; a segment written at ~2–3 GB/s and then `fsync`'d is ~100 ms by arithmetic, no journal needed |
| **D8 · answered** | is the per-batch commit ~3 ms or ~100 ms? | **~3 ms** (`pub_p50=3109us` in 17 of 23 samples with traffic); the 46 ms samples and `pub_max` 0.8–2.4 s are the pump holds above. No "fewer, larger entries per REPORT" wire change is indicated. Remeasure `pub_p50`/`pub_max` after D9/D10 in one 9-client IOR | the number the D8 row asked for, from the `raft-obs` line W16.2 added |

D9, D10, and D11 were rolled 05:31Z (`a53b253f2455-dirty`). The 12:02Z trace (§1b) is the measurement: D9 held (no pump wait, no `backpressure` line), D11 held (`fsync` max 91 ms), and D10's byte rule held (0.25 GB compacted, not 159 GB). D10's "drop the file cap" did not. That correction is D12.

**D12 was implemented and rolled 12:29Z** (`a53b253f2455-dirty`, no perf, no strace). On the 12:36Z IOR, fcstor004's compactor brought L0 from 7 files down to 3 (L1 stayed 397). The recommendation below is what was built. The 400 ms apply wait still returned BUSY (73 of 99 `report-split` lines on fcstor004).

| item | question | recommended (D12, Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D12 · W23 step 4** | may L0's file count grow without a bound while L0 bytes stay under 1 GiB? | **No. Keep the 1/8 byte rule, and put the file cap back as a backstop.** When `n_l0` is over `KV_LSM_L0_DEFAULT` (4), compact the range with the most L0 files even if its bytes are under 1/8 of its L1, and repeat until `n_l0` is under the cap. The 1/8 rule still applies when the count is already under the cap, so a fat range is not rewritten for a few KB. The pump still does not wait. D9's 1 GiB admission stays; it did not fire here | fcstor004 went 26 → 310 L0 files (L1 353) in one IOR while 34 compactions wrote 0.254 GB at 10–14 ms each; `lookup` then probes every file, the publish apply falls behind, and `host_wait_applied` returns BUSY at 400 ms (`pack_ms=0 rc=-13`, 54 of 60 reports). D10 said to drop the file cap because "reads already probe every L0" — that probe is the abort. **The backstop as built rewrote L1 for the whole of the 13:17Z copy (D13)** |

**D13 · W23 step 5 (from the 13:08Z trace, `results/measure/20260929-130800-ddposix/ana`). In tree 14:20Z, not rolled, not gated.** The shape below is what was built. A file-cap compact writes one L0 file and does not open L1. A compact that already meets 1/8, or the 1 GiB byte cap, still rewrites L1.

| item | question | recommended (D13, Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D13 · W23 step 5** | when the file cap forces a compact, may that compact read the range's L1? | **No. Merge that range's L0 files with each other into one L0 file, and do not read its L1.** Repeat until `n_l0` is under the cap. The 1/8 rule stays the only merge that rewrites L1. The pump still does not wait. D9's 1 GiB admission stays | D12 ran a full L0+L1 merge whenever the count was over 4, which was the entire 100 GiB copy: fcstor004 wrote 67.5 GB in 425 compacts (max 17 s, L0 peak 379) and `lookup` still binary-searched them (`kv_seg_probe` 8.3 % self). `pack_ms=0` on 172 of 184 BUSY reports; one `push_ms` was 206 s |

**Pending Sep 30 2026 04:20Z (from the post-fix ecopy, `results/measure/20260930-040600-postfix-review/SUMMARY.txt`). The two mechanical fixes (F1, F2 there) are in tree and rolled 05:06Z; these two still need the user. Ask; do not write either.**

| item | question | recommended (D15–D16, Sep 30) | why, in one line |
| --- | --- | --- | --- |
| **D15 · W14 step 2 / raft.c `on_vote_req`** | may a voter that heard from the current leader within the last election timeout refuse a higher-term VOTE_REQ and keep its leader? | **Yes — leader stickiness (Raft §4.2.3 / §9.6; Pre-Vote or CheckQuorum are the two standard shapes).** Recommend Pre-Vote: a candidate first asks "would you vote for me at term+1?" without bumping anyone's term; a voter answers no while its leader heartbeat is younger than the election timeout; only a majority of yes starts the real election. `maybe_step_down` then never fires from a peer whose only problem is that its own lane to the leader is stalled | 426 group-2 terms in 13 minutes: fcstor006, unable to get a reply from fcstor004/005 for 30 s at a time, campaigned every 0.5–0.9 s and each VOTE_REQ deposed the live leader (`LEADER->FOLLOWER leader=-1`, re-elected 0.6 s later); every cycle is 400 ms of BUSY reads for every client. F1 shortens each stall from 30 s to 250 ms; it does not stop the deposition |
| **D16 · peer transport** | should Raft peer frames share the RDMA device's one shared recv CQ / poller and the per-conn 36-credit scheme with ~400 client conns, or get their own class with reserved credits and their own poller? | **Measure first (stack-sample the sender and peer-conn threads in a storm), then decide.** If the lost RAFT_REPLY is the shared `recv_poller` behind client completions, peer conns need their own CQ/poller; if it is the peer's reply send waiting on a credit, reserved credits. Do not pick before the sample | the frames of a blocked batch reach the peer and are processed (fcstor006 won votes through a frozen lane); only the reply is late by up to 30 s, and only while ecopy drives hundreds of client conns per server. **Sep 30 06:17Z: the 30 s late reply was the sender reading the wrong channel (`85f5b31c`), not the transport; D15/D16 keep no motivating case** |

**Pending Sep 30 2026 07:10Z (from the user's perf dir, `results/measure/20260930-063500-perf-dir-review/SUMMARY.txt`). Ask; do not write either.**

| item | question | recommended (D17–D18, Sep 30) | why, in one line |
| --- | --- | --- | --- |
| **D17 · `st_blocks`** | where does a client get the allocated-block count for a file it did not write? `inode_allocated_bytes` counts the client-local present-chunk table (W20), empty since D2 for unopened files, so `du` sums 0 for every regular file (52K for 3.8 TB) and ecrawl calls 21150 of 21503 files sparse. `efs_meta_row` has no count; W20 forbids `st_blocks` from size alone | **A per-lane present-chunk count in the lane stamp** (each publish adds the chunks it made present, a truncate subtracts), reduced at getattr like `max_end`/`max_mtime` and returned in the row image; the client sums it. One more u64 per lane read, no new RPC. Alternative: define `st_blocks = 0` as "unknown" for files this client did not write and tell tools so | sparse detection, `du`, quota tooling and ecrawl all read `st_blocks`; only the server sees every lane's publishes |
| **D18 · client staging-table floor — DECIDED Oct 1 2026 (user): evict whole cold tabs; implemented `f073e136`, see §1b** | the staging estimate is dominated by the per-shard-tab floor (one 256-row slab + 16 × 1232 B chunk entries + 512-slot indexes ≈ 90–170 KB per tab, ×4096 tabs ≈ 360–700 MB) and exceeds `EFS_CLIENT_META_MB` (256 MB) with a few thousand rows staged (412 MB at 10780 rows, RSS 330 MB). The evictor then drops 64 hot rows a second forever and never reaches the cap. Which bound do you want? | **Evict whole cold tabs** (`shard_tick` already tracks tab age; a tab with no dirty/pinned/open ino is freed and rebuilt on demand), so eviction frees the floor it cannot otherwise reach; keep the row LRU for the rest. Alternatives: a smaller first slab and lazily sized indexes (the floor shrinks ~10×), or apply the cap above the floor (RSS then ≈ floor + cap) | the 22 ms/s stall from this churn is fixed mechanically (targeted `efs_export_evict_ino`), but the cache still cannot hold a du's rows, and the RSS bound the cap promises is not the one delivered. Sep 30 21:10Z: with the floor above the cap the evictor's LRU scan is **31.6 % of a 62 min client profile and 47.6 % (≈ 0.6 core) during an ecopy**, re-armed by every staging op (`results/measure/20260930-205300-ecopy-perf-review`) |

**Order for continued implementation (decided Sep 28 2026).** Correctness
and truthfulness first because they are small and they are what "easiest
to use" means; then the CPU items that need no decision and are most of
both profiles; then the two design items above; W12 whenever a run dir is
cited.

| # | item | what (order Sep 28) | why now |
| --- | --- | --- | --- |
| 1 | **W17 step 1** | `fsync`/`release` return in bounded time; a killed writer never leaves a D-state FUSE request | seven clients wedged for 20 min after `kill -9` |
| 2 | **W16 step 1** | RPC BUSY/NET/IO surface as EBUSY/EIO, never ENOENT | a busy filesystem must not claim a file is missing |
| 3 | **W18** | dcache reclaim from a dirty list; init once | 73 % of the client's CPU walks a table looking for dirty chunks |
| 4 | **W19** | encode each AppendEntries once; `try_commit` from the replying peer's match | 35 % of the leader is copying AEs on the pump |
| 5 | **W14 step 4** | fragment `path_index` hint on PUT | six path walks per PUT |
| 6 | **W15 step 3** | one `fuse_buf_copy` per whole chunk, for multi-chunk writes too | in tree only for `size == chunk`; `dd bs=1M` (eight chunks) still copies twice: 16 % + ~19 % in the 22:11 profile |
| 7' | **W14 step 2 (a)–(b)** | outbox coalesces replies/heartbeats instead of dropping them | `drop=72114 > sent=50695` to one peer; terms +59 / +26 in two hours; every client BUSY is `no-hint` |
| 7'' | **W15 step 4** | post the RDMA SEND and return; reap CQEs in `send_buf_pick` and the reply wait | 6.9 % of the client waits synchronously for its own send CQE; more send buffers did not change it |
| 6a | **W20** | `st_blocks` from a per-inode counter; `ll_setattr` reads `size` from the row | a `stat` walks every chunk under two locks; 8.9 % of the client under `ll_setattr` |
| 6b | **W21** | a kick does not wake a scan that cannot evict (1 s idle after a no-evict wake, cleared on unpin); cross-tab running total | in-tree cursor did not help: 12.8 % of the client on the 22:48 profile because every write over cap kicks a wake |
| 6c | **W15 step 5** | `FUSE_CAP_SPLICE_READ` so the write payload is read from the pipe straight into the dcache buffer | after step 3 one user copy per byte remains (~22 % `fuse_buf_copy`) plus the kernel copy into libfuse's buffer (~9.5 %) |
| 6d | **W23** | probe a pooled connection's liveness once per idle period, not per checkout; pool generation instead of `fstat` per send | 1.17M `fstat` + 1.17M `getsockopt` + 0.6M `MSG_PEEK` in 230 s (~15K syscalls/s) for 586K checkouts; ~2.5 % of the untraced client |
| 6e | **W24** | `open(O_TRUNC)` / `truncate` of a large file returns in bounded time; SETATTR under BUSY is EBUSY, not a hang | a `dd` onto an existing 1.7 GB file never got past `openat`; SETATTR is the most-exhausted RPC (136 of 424) |
| 2' | **W25** | `futimens` fences cross-group lanes (today EINVAL on every multi-lane file); atime keeps nanoseconds; BUSY → EBUSY everywhere | `ecopy --verify` fails `futimens` on every large file **and then deletes its copy**; the atime check fails on every small one; correctness, goes with row 2 |
| 7 | **W14 steps 2, 3, 5; W15 step 4** | election trigger under import; reap `snap-*.kvx.tmp`; `send_buf_pick` | smaller residuals from the same profiles |
| 8 | **D1 → W17 steps 2–3, W6.1** | commuting span publish | shared-file write rate must rise with ranks; the headline N-1 number |
| 9 | **D2 → W6.2** | open adopts the row; maps in windows | 1 GiB open 1.6–2.5 s under 36 openers |
| 10 | **W16 steps 2–3 (D8)** | `pub_batch_ms` in `raft-obs`, one 9-client IOR, bring the number; no budget change without the user | `push_ms=9869` for 337 batches is either queueing or the 100 ms `fsync` mode, and the two have different fixes |
| 11 | **W8, W10** | 9-host posix 201/201; RDMA faster than TCP | gates re-run once 3–9 land |

**Revised order (Sep 29 2026, after D4–D8).** The items below go
**before 9 and 10**: they are the cause of the two IOR aborts and
nothing downstream can be measured until an ior-easy-write completes.
9 (D2 windows) and 10 (D8) follow; 11 last.

| # | item | what (order Sep 29) | why now |
| --- | --- | --- | --- |
| 8a | **W22 step 1 (D4)** | snapshot by log bytes (512 MiB) with a retained log window; InstallSnapshot only below the window | a 2.67 GB export every 256 entries is the top user symbol on all four servers and turns any 257-entry lag into a 12–16 s import |
| 8b | **W22 step 2 (D5)** | the follower applies an import diff in per-tick slices and keeps answering heartbeats | terms +45 / +105 across one IOR with `drop=0`: the import is the election trigger left |
| 8c | **W14 step 4 (b) (D7)** | `path_hint = 0xffffffff` = first write of this fragment generation; server creates on the least-queue root with no probe | 1.59M `access()` / 192 s on one server per IOR; the re-PUT hint cannot reach a fresh chunk |
| 8d | **W22 step 3 (D6)** | measure `fsync` on the shared vs a quiet root; if the 100 ms mode is the sharing, `--meta-storage <root>` and redeploy 19810 with `mdraft/` off the fragment roots | compactor `fsync` flat 100 ms ×97, `persist_max` 254 ms; a redeploy is a cluster change, so it is last of the four and its measurement decides it. **Superseded by D11 (Sep 29 04:27Z trace): the Raft-log `fsync` is 0.4 ms; the 100 ms is the compactor's own segment. Flag is in tree; no move** |
| 8e | **W23 step 1 (D9)** | the pump never waits for the compactor; back-pressure is BUSY at `host_propose` on the leader while L0 bytes are over 1 GiB | **rolled 05:31Z, held on the 12:02Z trace:** no pump wait, no `backpressure` line. The BUSY in that IOR is not this admission |
| 8f | **W23 step 2 (D10)** | compact a range when its L0 bytes reach 1/8 of its L1, L0 cap in bytes, split range 0, 1 MiB merge reads | **rolled 05:31Z. Byte half held** (0.25 GB compacted, not 159 GB; `pread` over 1 MiB is in use). **File-cap half did not** — see 8i |
| 8g | **rolled 05:31Z** | `disk_log_new_bytes` O(1); `kv_seg_w_open` `setvbuf` 1 MiB | in the same roll as 8e |
| 8h | **W23 step 3** | `lookup` skips a segment whose first/last key span does not contain the key, before `block_for` | **rolled 12:29Z.** Last key is cached in `kv_seg_open`; `kv_seg_probe` returns NOT_FOUND when the key is before `idx[0]` or past the cached last key. Not separately profiled (this IOR had no perf). L1 was still 397 files and `report-split` was still mostly `rc=-13` |
| 8i | **W23 step 4 (D12)** | when `n_l0` is over the file cap, compact the range with the most L0 files even under the 1/8 ratio, and repeat until the count is under the cap | **rolled 12:29Z.** On the short IOR, fcstor004 ended at L0=3. On the 100 GiB copy the same rule rewrote L1 the whole time (67 GB, L0 peak 379). That shape is D13 |
| 8j | **W23 step 5 (D13)** | over the file cap, merge the range's L0 files with each other and do not read its L1; the 1/8 rule stays the only L1 rewrite | **in tree 14:20Z, not rolled, not gated.** The probe walks every in-span L0 file, and the full-L1 backstop both failed to keep the count down during the copy and put 67 GB on the same disk as the apply |

**Added Oct 1 2026 (correctness; both go before every row above —
details and the exact follow-up steps are the 04:30Z block in §1b).**

| # | item | follow-up steps | evidence and limits |
| --- | --- | --- | --- |
| 0a | **W26 · STALE replay that never converges** | (a) identify ino 116202 on 19810 from a KV copy and compare its chunk row with what the classifier (`write.c:880–968`) would replay; (b) repro: eight `dd bs=1M` into one mount, SIGINT mid-write, client stop, `EFS_DCACHE_TRACE=1`; (c) mechanical: the unmount drain names the inos and rc it abandons, and a `report-stale` round that replays the same single chunk > 16 times logs ino/ci/row gen/verdict once; (d) **ask** what a rec that STALEs identically on every replay should do (fail the next close/fsync after N? retry until unmount? spill locally?) | fstor007 Oct 1 00:05: 2768 rounds of `report-stale: chunks=1 … committed=0 replayed=1` and then `UNMOUNT DATA LOSS … rc=-14 after 60s`. Acknowledged writes were discarded; the log cannot say whose. Forbidden: widening the 60 s drain, dropping the STALE check, or publishing a rec the server rejected |
| 0e | **W38 · ior-hard: a client's full image folds its own published span without the span's bytes (4256 B of zeros, committed)** | the F3 block in §1b (Oct 1 08:05Z): one-client 4-rank IOR hard with `EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1`, `hardscan` cold, `raft-getchunks` on each bad chunk; fix on the client (the fold observation must come from a body that holds the span) | `results/io500/20261001-074905-rdma` (NOTE.txt, hardscan.txt, getchunks-118842-118844.txt): ino 10897 ci 118843 base 1774…2861 + len-0 tombstone 1838…0185 seq 1222; 1 of 747720 records; first IOR with W30 in the client. Data loss: goes before 0c |
| 0c | **W36 · rename-vs-unlink of one source both succeed, dangling dentry** (posix2 `peer_rename_vs_unlink_src`, 1 in 6) | the F1 block in §1b (Oct 1 07:45Z): trace the two txns with `APPLY_LOG`, decide between `apply_unlink_cmd`'s silent NOT_FOUND→OK and an EXCL DEL that passes on an absent key, fix that one | evidence `/tmp/efs-mount/posix-2c-r422-6/peer_rename_vs_unlink_src/b` on 19810 (`-?????????`), `~/efs-runs/p2r422.log`, `results/posix2/20261001-073048`. Correctness: goes before 1a–1h |
| 1j | **Read path R1–R5 — DONE (Oct 1 15:45Z, all clients 16:01Z); server PUT bounce copy — rolled 18:00Z** | R1 a prefetched chunk's buffer is handed to the rdcache (`efs_rdcache_put_owned`), R2 a whole-chunk demand read decodes into the caller's buffer (`chunk_get_job.ext`), R4 the two data fragments are received straight into the chunk (`efs_decode_chunk` skips the self-copy), R5 a chunk-aligned READ whose chunks are all cached is answered with `fuse_reply_iov` over pinned rdcache images (`efs_client_read_refs`, `efs_rdcache_pin/unpin`; `rdcache_ent.pins` keeps a pinned way from being a victim); the get pool is 64 workers (`GET_POOL_N`, was tied to `EFS_WRITE_PIPELINE`). Server: an aligned PUT with a digest is one `writev` (payload + 4 KiB tail page), no bounce memcpy. **Still a copy:** RDMA recv buffer → chunk (zero-copy *receive* is a transport change — ask), and the FUSE write copy (W40) | fcstor007 16 GiB cold read **2.5 → 3.6 GB/s** (client CPU 21.6 → 10.8 s), 4 readers **3.7 → 6.5 GB/s**, prefetch 32 is slower than 16 (2.9); cmp/md5 OK, posix jobs=1 200/201 (`results/posix/20261001-154333`). Dirs `agent-rd-20261001-145047` (base), `-153035-inplace`, `-154007-refs`. `perf record -a` is refused at `perf_event_paranoid=1`; attach `-p` after the pools exist |
| 0f | **D24 · the close-time REPORT of a long sequential write — DECIDED and in the tree (Oct 1 14:00Z, user: "implement all fixes that prevent the lock")** | `report_landed_note` (write.c): every 8192 landed PUTs (1 GiB, global) kick the flush thread's whole-set REPORT; the REPORT reply wait is sized to the record count (`EFS_IO_TIMEOUT_MS + 0.5 ms × recs`, `rpc_send_recv_dual(recv_ms)`) so a long REPORT is not re-sent while the server executes it; both daemons stamp every log line (`log_ts.c`). Gate: the 14:00Z block in §1b | the 13:41Z wedge on fstor007 (`~/efs-runs/rec-look55x.log`): 8 × 20 GiB closes → `retry type=67 why=recv rc=-6` → `slow-ok attempts=6 us=116 s`, 16 dd in D-state in the kernel's forced FLUSH, ecopy's closes queued on `report_mu` for minutes. Still open as **W41**: `report_mu` serializes every close on a client behind one REPORT's retry loop (up to 8 s on STALE); per-inode dirty-set extraction + a multi-slot `pub_ino` set would remove the serialization point. Ask before building it |
| 1i | **W39 · RDMA zero-copy fragment send — DONE (Oct 1 15:30Z, all clients 16:01Z); W40 · FUSE write copy (ask)** | W39: bufpool slabs are registered lazily per HCA (`efs_rdma_zc_region_add`, `zc_lkey`), `efs_rdma_send_frame` posts a two-SGE send (header + slab payload ≥ 4 KiB) when `max_send_sge ≥ 2`; the payload is referenced until the send CQE, which every caller already guarantees (reply or `efs_conn_destroy`). Log once `efs: RDMA zero-copy send active`. W40 (ask): the `fuse_buf_copy` into the pool is the other full write copy; libfuse 3.10.2 has no custom-buffer receive — an own `/dev/fuse` loop | 16 GiB random dd+fsync on fcstor007 1.4 → 1.5 GB/s, client CPU 21.6 s (`agent-rd-20261001-151704-new`); cmp at 0/5/15 GiB and whole-file md5 OK. CPU, not wall |
| 0d | **W37 · `raft-mkfs` on a second node forks the salt — fixed, rolled 18:00Z (`dc6b0af1`); gate = the next fresh-table start** | `server_raft_host_mkfs` reads the salt the committed MKFS recorded (`efs_meta_apply_export_salt`: root shard's export key, else group 2's anchor) and uses THAT for the SALT step, never its own (`table exists with salt …; own salt … not used`); a node holding neither record answers BUSY (`retry on a group-0 node`); `EFS_MD_CMD_SALT` is on the apply's ring-only list so a mismatching SALT answers PROTO instead of halting group 2's apply. `efs_meta_apply_mkfs` keeps its contract (no-op on an existing root, first salt kept — `test_meta_apply` `idempotent`, `test_sim` `idempotent mkfs`; an EXIST-returning variant broke both and was reverted). Not exercised on a fresh table yet — the next `cluster.sh start --fresh` is the gate | `rec-st402..404` (`apply salt rc=-7 index=3` forever), `start401`, `wipe405` |
| 1a–1h | **W28–W35 · the 1 GiB dd review (Oct 1 05:10Z)** | eight items from the fstor007 write/read straces, server straces and profile — the subsection "W28–W35: what the 1 GiB dd showed" below this table; W28 (layout window off by one group) and W29 (prefetch/demand duplicate GETs) are one-function fixes and go first; D19–D22 are the asks it raised | 1 GiB write 207 MB/s = 1.6 s of writes with zero PUTs then a 3.56 s close; 1 GiB read 184 MB/s = a serial 80 KB GETCHUNKS over TCP plus ~400 futex per MiB. Both numbers were taken with strace on both daemons and are tracer plateaus; the shapes are not |
| 2a | **W42 · `df` / `efs-mgmt status` report the 3-node capacity model on any node count** (Oct 1 2026, user) | mechanical: replace `total_logical = 2 × min_quota` (`efs_fuse_statfs`, `efs_fuse.c:3108–3120`) and `usable_cap = 2 × min_quota` / `usable_free = 2 × min_free` (`efs_mgmt.c:129–185`) with the 3-of-N placement bound: the largest `M` (chunks) with `Σ_i min(c_i, M) ≥ 3M`, `c_i` = node `i`'s quota (or free) in 64 KiB fragments, times 128 KiB; count only up nodes with a quota, as today. Reduces to `2 × min` on three nodes and to `Σ × 2/3` on N equal nodes. Also make `f_blocks` and the used figure come from the same model (statfs today derives used from `Σ phys × 2/3` and total from `2 × min`, so on four nodes used can exceed total and `avail` clamps to 0 while writes still succeed). Unit test with 3 equal, 4 equal, 3 unequal (100/100/1000 → 200, not 800). Then verify on 19810 (`df` vs `efs-mgmt status` vs `4 × 36T × 2/3`) | Both comments say "every chunk places one fragment on each node" — true for three nodes only. Four 500 GiB nodes show 1000 GiB instead of 1333. Not a data-path change; no decision needed. **Verify while there:** what a PUT does when exactly one stripe member answers `EFS_ERR_QUOTA` (`put_fragments_parallel_once`: `quota_errors >= 2` → QUOTA, `acks >= 2` → OK) — if the chunk publishes with two fragments, a full node creates protection debt silently (product-gaps §1.2); if `reroute_down_fragments` moves it, say so in the failure-tolerance table |
| 0b | **W27 · REPORT identity from the staging table** | (a) find the path that leaves a dirty chunk with neither a putid nor a dcache object (`write.c:1103`: putid table eviction, reclaim after `b6c1712d`'s pin release, or an irec-only threshold REPORT); one traced ecopy of a small-file tree on an idle cluster; (b) rerun on the current client first — if `putid miss` is 0 there, record and close; (c) only if (a) names the cause: a chunk with no PUT of ours is not ours to publish (keep dirty, replay from the row), as the `fragment_nodes[0] == 0` branch already does for span-only rows | ≥ 9016 recs (`n=8812…9016` in the last rate-limited second, all `ci=0`) reported with a mapping that "may be the server's row, not this client's PUT" — the Sep 30 (gen, off, len) / conflated-table class that lost ior-hard records. Forbidden: silencing the line, or "committing" such a rec client-side |

##### W28–W35: what the 1 GiB dd showed (Oct 1 05:10Z) and how to fix it

**Source.** fstor007 `dd bs=1M count=1024` write then read of `/tmp/efs-mount/001/dat17`, 01:06:20–01:07:20 EDT; client `strace -f` + `perf record`, servers `4b2844487831-dirty` with `--perf --strace` (restarted 01:03 EDT); straces and profile in `/home/erbmi1/orcd/scratch/efs/perf/efs-mount/`, the read-only passes in `~/efs-runs/ddana2.log`, `ddana3.log`, `ddana4.log`, `srvana2-fcstor00{3,4,5,6}.log`, `srvana3-fcstor004.log`. Both daemons were traced and both sat on the strace plateau (client 97–103 K lines/s during the read, servers 101–113 K/s during the write drain), so **207 MB/s write and 184 MB/s read are tracer numbers; every count below is real**. Untraced references: single dd write 918–1000 MB/s, single dd read 585 MB/s.

**Gate for the whole group (one script, run after each item, never with strace on a daemon):** on fcstor007, remounted, idle cluster: `dd bs=1M count=8192 conv=fsync` of a non-zero source to a fresh file, then remount, then `dd bs=1M` read of the same file; record MB/s for both, the per-`write`/`read` latency histogram from `strace -T` **on dd only**, and from the servers' `efsd.log` nothing (no recorders). Plus the counters each item names. The client bench (§1a, plan only) replaces this once it exists.

**Order:** W28, W29 (one function each, read side), W30, W31 (write side, mechanical), W32, W33 (both sides, mechanical), W34 (server, mechanical), W35 (measure), then the asks D19–D22. Correctness items (0a/0b above) still come first.

| # | item | what | measured | fix (mechanical unless marked ask) | done when | forbidden |
| --- | --- | --- | --- | --- | --- | --- |
| 1a | **W28 · layout window off by one group** | `efs_client_pull_layout_miss` (`ops.c:449`): `fresh` requires `seen.ci1 >= ahead` with `ahead = ci0 + 2·win`; `seen.ci1` is the previous call's `ahead`, which is one 8-chunk range short of this one's, so after the window ramps to 256 **every 1 MiB read issues a GETCHUNKS before its data GETs** | 94 `recvfrom … 79888` on the client in a 1024-read window (one per read per libfuse worker); each is the whole 64-entry group (64 × 1248 B), **> 72 KiB ⇒ TCP side channel**, ~1.3 ms request-to-reply, serial, ≈ 25 % of each read's 5.2 ms | freshness is "the **requested** range `[ci0, ci1)` is covered and the pull is < 200 ms old"; extend the window only when the covered range ends within `win` of `ci1` (pull `[seen.ci1, ci1 + 2·win)` then), not on every call. Keep the forced `have_ce = 0` re-pull semantics (peer spans, D1) and the 200 ms bound | a 1 GiB sequential read issues ≤ 40 GETCHUNKS (8192 chunks / 256-window steps), counted on the client (`recvfrom … 79888` or a counter); posix/posix2 unchanged; `peer_overlap_pwrite_chunk_straddle` and `basic_chunk_boundary` pass | dropping the per-batch re-pull, caching the map past 200 ms, or widening the 200 ms |
| 1b | **W29 · the read fetched every chunk it already had, and the prefetch fetched nothing** | **corrected 05:45Z from the server straces:** GET sequences per server 4096 / 4098 / 4098 / 4100 = 16392 ≈ 2 × 8192 — exactly two fragments per chunk, every path opened once, no parity, **no duplicate** (the earlier "2.6 per chunk" counted strace `unfinished` lines twice). So no prefetch GET landed: `prefetch_ahead` skips any chunk `efs_dcache_has` (`read.c:1213`) and this client had just written the file — every clean image was still in the dcache. The demand worker does the opposite: `efs_rdcache_get` miss → `efs_client_fetch_published_chunk` (2 GETs) → `efs_dcache_overlay`, which **copies the whole current dcache image over the fetched bytes** (`write.c:3168`, `dcache_image_current`). The 3 ms GET wave in every read was for data the client already had | 1032 GETCHUNKS and 4096 GETs on fcstor004 in the read window, one of each per MiB; servers at ~8 % of one core during the read (blake3 verify 16.6 % of that); client wave ~3 ms per MiB | (a) the demand worker asks the dcache first — `efs_dcache_copy` (image current and no spans), the same test the sub-chunk path already makes at `read.c:1364` — and fetches only on a miss; (b) on a remounted client (no dcache) the prefetch *does* run and, by code, a demand job refetches a chunk whose prefetch is in flight (`rdcache_hit` is false until it lands): give the rdcache a *pending* entry with a waiter so demand joins the in-flight job; (c) submit the next window's prefetch before waiting on the current batch, not after the copy-out | same-mount read-after-write of 1 GiB: server GETs = 0 (the dcache serves it; reads see unchanged bytes, sha256); **remounted** read: server GETs per 1 GiB = 16384 ± 1 %, rdcache hits ≥ 7 of every 8 demand chunks (counter under `EFS_DCACHE_TRACE`); the honest read number is the remounted one | a second cache, raising `EFS_READ_PREFETCH` to hide (b), zero-filling anything, serving a dcache image whose `base_gen` the map does not name |
| 1c | **W30 · flush starts at `close()`** | a file under the dirty-byte cap is never PUT while it is written: 1024 writes in 1.6 s with **zero PUTs** (poller 27 then 2 kicks/s), then `close()` = 3.1 s PUT drain + 0.36 s REPORT; wall = write + drain, not max | `dd_1m.strace.txt`: writes 0.5–1.3 ms each, `close(1) <3.563>`; `ddana2.log` close-phase kicks ~2000 per 100 ms from 22.4 s on | **D19, ask first (recommended yes):** a chunk whose 128 KiB are complete (sequential write moved past it) is PUT-eligible at once — the reclaim thread pool takes it from the per-shard dirty list without waiting for the cap; REPORT/publish stays at close/fsync/threshold (W2 unchanged: PUT is not publish). Cost: a chunk rewritten before close is PUT twice (new generation; the STALE/putid machinery already handles it) | 1 GiB dd+fsync wall ≈ max(write phase, drain) — the close phase under 0.5 s after a 1 GiB sequential write on the gate script; poller kick rate non-zero during the write phase; posix `concurrent_appends`, `mtime_monotonic_many_writes`, posix2 63/63 | publishing on write (forbidden by spec), raising the dirty cap, a per-file flusher thread |
| 1d | **W31 · one `mmap` per chunk in the write path** | `efs_fuse_write_buf` → `efs_buf_alloc(cs)` → pool empty → `malloc(128 KiB)` → glibc mmap threshold → fresh mapping + 32 page faults per chunk; the pool refills only when the flush returns buffers, which (W30) is at close | **8193 `mmap`** in the 1.6 s write phase; `clear_page_erms`, `do_anonymous_page`, `vma_alloc_folio`, `lru_add_fn`, `rmqueue_bulk`, `gup_*` ≈ 5 % of the client profile | the pool is sized to the dirty cap at startup (or grows in slabs of N chunks from one `mmap`), and `efs_buf_free` returns buffers to it instead of `free()`; alternatively `mallopt(M_MMAP_THRESHOLD, …)` once at start — pick the pool, it is already there | `mmap` count during a 1 GiB write phase < 64 (strace on the daemon for this count only, then off); the page-fault symbols leave the top 30 | growing the pool without bound (it must cap at the dirty cap + pipeline) |
| 1e | **W32 · flush concurrency = 16 chunks in flight** | each of the 16 `dcache_reclaim_main` threads PUTs one chunk (3 fragments) and waits for all three replies before taking the next; put-pool mutex `0x597680` 8450 WAKE / 8060 WAIT in 3 s | 8192 chunks / 3.1 s = 2.6 K chunks/s; Little: 16 × 128 KiB ÷ 5.5 ms ≈ 370 MB/s = the observed drain (the 5.5 ms is the server RTT under its tracer; the 16 is ours) | a reclaim thread keeps up to P chunks' PUTs in flight (send chunk N+1's fragments before chunk N's replies are back; reap in order) so in-flight = 16 × P, with P from `EFS_WRITE_PIPELINE` logic that `dcache_flush_keep` already has for the close path — one code path for both; shard the put-pool queue per thread or per node so the mutex is not shared by 16 threads × 3 fragments | at a fixed server RTT the drain rate scales with P on the gate script (P = 1 vs 4: ≥ 3×); `0x597680`-class contention gone from the strace (count only, then off); `test_data`/posix unchanged | more threads (D-class: the poller is not the lever), a second put pool, changing the fragment size |
| 1f | **W33 · ~400 `futex` per 1 MiB read, ~27 per fragment PUT on the server** | client get pool (`0x596960`: 34.7 K WAIT / 28.9 K WAKE per 1024 reads) and frag pool (`0x545540`: 27.2 K / 18.7 K) wake 32 + 128 workers through `pthread_cond_signal` chains, 8 chunk jobs + 16 fragment jobs + batch completion per read; server: a PUT is handler → `g_pool.lock` → `not_empty` → one of 20 `writer_thread`s → `run_job` → `done_cv` back to the handler (`writer.c:173–274`), plus ~3 eventfd kicks per PUT from the recv pollers (tids 951867/951947: 19.4 K `write` for 6172 PUTs); 86 K futex in the 6 s write window on fcstor004 | **398 futex + 78 read + 39 poll + 32 write ≈ 550 syscalls per MiB read**; at the tracer's ~100 K/s that is exactly 5.5 ms = 184 MB/s; untraced it is ~1 ms of CPU and scheduler per MiB and every read sampled shows a 2.2 ms tail before the last two completions | one wakeup per batch, not per job: submit the 8 jobs then signal once per idle worker needed (or hand jobs to workers through per-worker slots like `host_waiter_sleep`); the fragment fan-out of a chunk runs on the chunk's own worker (2 GETs issued back-to-back, one wait) instead of a second pool hop; server: same count on the writer pool, measured by `efsd --bench` first | futex per 1 MiB read ≤ 40 (count by strace, then off); the 2.2 ms tail is gone from an untraced dd read's per-`read` histogram (dd-side `strace -T`); server futex per fragment ≤ 6 | adding workers, spinning instead of sleeping, touching `recv_poller` |
| 1g | **W34 · server PUT = 3 `openat`, 2 inodes, and the drain is kernel time** | per fragment: `openat(O_WRONLY\|O_TRUNC\|O_DIRECT)` without `O_CREAT` **fails ENOENT** on every first write — **corrected 05:45Z:** this is the *quota* probe, not the path-hint one: `server_write_fragment_with_sum_sync` opens no-create first so an overwrite is not charged (`store.c:1259–1291`, `arg.no_create = 1`), on every PUT under `--quota` (19810 runs `--quota 36T`); W14.4b's `EFS_PATH_HINT_SKIP` only skips `server_find_fragment_root`'s `access()` walk — then `openat(O_CREAT\|O_DIRECT)`, `write` 64 KiB (50–60 µs p50), `close`, `openat(.sum, O_CREAT)`, `write` 32, `close`, 2 `fstat`; **no `fsync`**. Perf, write-drain bin on all four servers (`~/efs-runs/perfana2-fcstor00{3..6}.log`): **75 % kernel, 12–17 % `[xfs]`, 4–5 % efsd, 3–7 % libc**; of the kernel share roughly a third is the tracer (`ptrace_do_notify`, `ptrace_stop`, `__task_pid_nr_ns`, `syscall_trace_enter/exit_work`), the rest is file creation — `xfs_btree_lookup`, `xfs_cntbt_key_diff`, `xfs_buf_get_map`, `xfs_trans_read_buf_map` (inode + extent allocation for two new files) and the path walk (`__d_lookup_rcu`, `link_path_walk`, `inode_permission`, `step_into`: an 11-component path walked three times per fragment). efsd's own symbols: `server_handle_conn`, `efs_rdma_recv_wait`, `shard_io_thread`, `pick_write_path`, `server_write_fragment_with_sum` — none above 0.6 % | fcstor004 write window: 6144 no-`O_CREAT` opens (probe → create gap 176–188 µs p50 on all four), 6194 creates, 6150 `.sum` creates, 68 `mkdir`, 0 fsync; ≈ 55 syscalls per 64 KiB; ~2.4 cores per server during the drain, traced | (i) invert the quota probe: open `O_CREAT\|O_EXCL` first (new file ⇒ charge), and only on `EEXIST` take the no-create overwrite path — or skip the probe outright when `efs_tls_path_hint == EFS_PATH_HINT_SKIP` (a first write cannot exist); the common case pays one open; (ii) `mkdir` on ENOENT stays (68 per 6172 — D7 holds); (iii) **D20, ask:** the `.sum` sidecar as a second inode per fragment — options: a 32-byte trailer in the fragment file (read path `pread` one 4 KiB `O_DIRECT` block more), an xattr, or keep; on-disk format, so it is a decision; (iv) the path walk is 3 × 11 components per fragment — an `openat` relative to a per-thread parent-dir fd was tried once (Sep 28, "thread-local inode-dir fd", reverted because the slowest 9-client dd got worse) — do not retry it without the bench | no-`O_CREAT` `openat` count = 0 during a fresh-file write (strace on one server for the count, then off); fragment-creates per 64 KiB = 1 after D20; `efsd --bench` data kind prints the before/after ops/s at QD 64 and its `perf` shows `[xfs]` + path walk below 30 % | an `fsync` per fragment (spec: the Raft log is the durability boundary), a third file, a per-thread dir fd without the bench |
| 1h | **W35 · the 1248-byte chunk record** | REPORT for 1 GiB = `writev … 10,223,693` B = 8192 × 1248 B (1 % of the data) over TCP, reply 0.355 s, leader `pub_p50=227 ms` for that publish; GETCHUNKS reply = 64 × 1248 B = 79,888 B > `EFS_RDMA_BUFSZ` (73,728) ⇒ TCP for every window pull | the REPORT's 0.36 s is 10 % of the 1 GiB close; the GETCHUNKS frame size is why W28's RPC is a TCP round trip. **Server perf, the bin after the drain (fcstor004, g2 leader for this inode):** `__memmove`, `lookup`, `search_block`, `kv_mtab_pos`, `kv_seg_probe`, `kv_seg_iter_next`, `efs_crc32` — the publish apply is one KV point-get with a block CRC per chunk record, 8192 of them sequential ≈ 28 µs each = the 227 ms `pub_p50`; the server was at ~0.2 cores while doing it (latency, not CPU) | **measure first:** `tests/tools` dump of one encoded record — which fields are the 1248 B (3 × 32 B checksums, nodes, 8 × delta, generation, padding?); then **D21, ask:** a compact record for the common case (full image, no deltas: ~120 B) with the delta list only when `ndelta > 0`; and/or **D22, ask:** `EFS_RDMA_BUFSZ` ≥ 84 KiB so a 64-entry window and a 64 KiB fragment both fit RDMA (memory: 36 bufs × conns). Both are wire/pool constants with a spec | REPORT bytes per GiB ≤ 2 MB; GETCHUNKS and GET replies never take the TCP side channel on a healthy conn (`recvfrom … 79888` count 0 in a read); 9-client IOR hard-write fsync time falls with the REPORT size | splitting REPORT into several RPCs (forbidden), compressing on the wire |

**Not new, already queued, confirmed by this run:** the two user copies per written byte (`memcpy_erms` 5.7 % + `fuse_copy_*` 2.3 % kernel→libfuse, `memmove` 16.3 % libfuse→dcache) are W15 steps 3/5; blake3 4.3 % + `xor_into` 3.1 % are the arithmetic floor for the `cpu` kind of the client bench. The REPORT publish at 227 ms is D8's `pub_p50` with a 10 MB entry.

**Server perf and strace (reviewed 05:45Z, recorders stopped 05:27Z with the daemons):** `/tmp/efs-perf/efsd.data` on fcstor003–006 is `perf record -g -F 999` cycles, 1394 s, 24–31 K samples each — the daemon averaged ~2 % of one core; the write drain is the only hot bin (7279 samples / 10 s on fcstor004 ≈ 2.4 cores for 3 s), the read bin is 841 (≈ 8 % of one core, `blake3_hash_many_avx2` 16.6 % of it — the server-side verify on GET), and the bin after the drain is the publish apply (W35). Per-bin DSO split and the symbols are in W34; the strace counts in W29/W33/W34. The whole-run top is the tracer (`ptrace_do_notify` 5.4 %, `__task_pid_nr_ns` 2.9 %, `child_wait_callback` 2.7 %); the only background efsd symbol is `host_gc_frag_pass` → `lsm_scan_prefix` → `memcmp` at 0.7 %. **What the server data changes:** W29 (no duplicate GETs; the same-mount read refetched what the dcache held and the prefetch never ran), W34 (the dead probe is the quota overwrite probe, and the drain is XFS creation + path walk, not efsd), W33 (the 27 futex is the handler↔writer-pool hand-off). W28, W30, W31, W32, W35 stand. Nothing on the server side limits the read: the read ceiling is the client's serial per-MiB chain. Scripts: `~/efs-runs/perfana.sh`, `perfana2.sh`; logs `perfana{,2}-fcstor00{3..6}.log`. The data files stay on the nodes (`/tmp/efs-perf/`, 170–375 MB strace each) until the user removes them.

In tree and rolled (Sep 29 04:08Z, `54a500da9dc8-dirty`, RDMA, `--perf --strace`; the 02:35Z roll was `bbcbcb5ad779-dirty`), not gated by a suite: 1 (sync report loop: 8 s checked between REPORT attempts, plus whatever one in-flight RPC takes; dirty set merged back; fsync returns EIO), 2 (`fuse_stat_errno`), 3 (dirty list + `dcache_ensure` so `pthread_once` is not on the lock), 4 (`send_ae` reads the log into the wire buffer; `try_commit` starts at the quorum match), 5 (PUT reply carries the storage root), 6 (a 1 MiB write walks chunk-aligned pieces; each whole chunk is one `fuse_buf_copy` into a dcache-owned buffer; a partial head or tail bounces its own length), 6a, 6b, 7's snap-tmp reap and `send_buf_pick`, 7' (an AE reply replaces an older queued reply to that peer, a heartbeat replaces an older heartbeat, a vote is not evicted, and the sender drains that lane before entry AppendEntries; a full entry lane returns AGAIN and does not increment `st_drop`), 7'' (`efs_rdma_send_frame` posts and returns; the reply wait reaps send CQEs and treats a send still busy after 5 s as a dead QP), 8, 9 step (a), 8a (snapshot by 512 MiB with a retained window), 8b (import diff in 1024-key slices), 8c (`EFS_PATH_HINT_NEW`), and the `pub_p50`/`pub_max` line of 10. 8d's `--meta-storage` flag is in and defaults to the first `--storage` root; `mdraft/` was not moved and the shared-vs-quiet `fsync` measurement was not run. 8h and 8i / D12 were rolled 12:29Z. The 12:36Z IOR (`results/io500/20260929-123635-rdma`) finished ior-easy-write at 2.218 GiB/s and mdtest-easy-write at 1.765 kIOPS, then ior-hard-write aborted on fsync (31 failures, no bandwidth) and fcstor012's ranks stuck in D behind a reclaimer holding the shard I/O lock across a fragment GET. The 13:17Z copy failed a different way: `fsync` returned EIO because D12 rewrote L1 for the whole ingest (D13). 8j / D13 is in the tree (14:20Z), not rolled, not gated. The shard-lock drop is in the tree (14:50Z), not rolled. Next is 10's one IOR, 7 step 2 (c) (measure, then ask), 11. 8e, 8f, and 8g were rolled 05:31Z, including the 1 GiB L0 byte cap; the 12:02Z trace is their measurement (§1b). In tree and rolled with that same build: W23 client (pool generation instead of `fstat`; liveness probe only after 1 s idle), W24 step 2 (one 8 s deadline around truncate's flush, REPORT, and SETATTR), W25 (`futimens` proposes the utimens command on the other group for cross-group lanes before the inode-group entry — not `LANE_FENCE`, which would delete chunks; `atime_nsec` on the SETATTR wire; `EFS_ERR_BUSY` is EBUSY). The three `test_meta_apply` cases named under W17 step 3 are in `test_chunk_deltas` (Sep 29 15:40Z): replay of a folded span is a no-op, a span after the fold attaches to the new base without naming it, overlap with a live span is STALE (a tombstone's old range is free), and a full image after the fold must name the current base. The 04:08Z roll and the 02:35Z IOR analysis are in §1b.

Prior recommended answers, for the record:

| item | question | recommended (server perf, Oct 1) | why, in one line |
| --- | --- | --- | --- |
| W13 | full-L1 compaction holds the apply path 2.4 s and costs a term | **done Sep 26**, partitioned flush rolled Sep 27 on TCP | background compactor kept `apply_max` under 70 ms. Flush now writes one L0 file per `key[0]`; one compact leaves the other range's L1 file in place (`test_kv_lsm`) |
| W11 | the KV export is larger than one 4 MiB SNAP command, so the log never truncates | **done Sep 27** — chunked InstallSnapshot of a file; import is a sorted diff | logs under 5 KB; fcstor005 rejoined in 510 ms; `apply_max` 0 on a 386 MB export. 9-host posix 200/201 (`results/posix/20260927-123717`) |
| W9 | bound the client staging table | **done Sep 27** — in-flight pin, append-reservation pin, chunk maps before the row, and the evictor walks past a pinned oldest window | a cold stat of 1M files leveled at 233 MB RSS (`results/measure/20260927-w9-walk`). posix 2 is 63/63 (`results/posix2/20260927-123946`). Leaks are clean (`results/leaks/20260927-035622`) |
| W10 | RDMA needs an empty table to gate | **live Sep 28**, suites pass, not faster than TCP on posix or the 9-client write | 19810 is RDMA (`db2b88c4802a-dirty`). jobs=1 after user xattr is 200/201 (`results/posix/20260928-043918`); 9-host before that was 199/201 in 57–59 s; posix2 63/63; 9-client dd **1326.6** vs TCP 1478 |

---

##### W1 — Shared-file (N-1) writes from two clients silently lose data — DONE

**Done Sep 18 2026** (working tree on leftover-1 19810, TCP). I12 CAS is
live: `efs_chunk_rec.base_gen` + `chunk_generation` on the wire, leader
checks the writer's base before propose, apply STALE is audible
(`EFS_INODE_RPC_STALE=11` / `EFS_ERR_STALE=-14`) and does **not** stall
`last_applied`, client refetch+overlay+PUT retries. Fragments are
`{ci}.{fi}.{gen}`; GET uses `fragment_path_at` and mints the export shell.
Report identity comes from the last PUT (`putid`), not a GETCHUNKS stub.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

##### W2 — `write()` is specified as durable-and-visible; the code buffers — DONE

**Done Sep 18 2026**, option (i): the spec moved. [architecture.md §3](#architecture)
now lists three deviations; a returned `write()` is client-buffered;
durable + cross-client visible at `fsync` / last `close` / `O_SYNC`.
`O_SYNC`/`O_DSYNC`/`-o sync` is specified write-through and is **not
wired**. Do not implement publish-on-every-`write()` — that is the
rejected 10× throughput change.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

##### W3 — Split the single-client fsync tail, then remove the larger half — DONE

**Done Sep 18 2026** (leftover-1 19810, TCP). Measure said REPORT owned
73 % of the 8 GiB `fsync` (flush 5.7 s / report 15.0 s / wall 25.8 s).
Cuts, in order: `HOST_PUB_BATCH_N=2048` + skip `get_chunk` when
`base_gen==0`; raft-log `sync_hold` across the report's proposes;
client flush pipeline (`dcache_steal` + put pool); PUBLISH follower
forward is `host_propose` not `host_propose_wait`; O_APPEND fetches a
published mapping before sparse RMW; truncate/symlink report only that
ino; FUSE `flush` waits `report_dirty_ino(ino, 1)` (W2 close is
durable — kick-only left `i_size` 0 so the next O_APPEND wiped the
prefix). N=4096 did not win. AE 1 MiB is ~5168 pubs; deeper batching
is exhausted.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

##### W4 — 4-client and 9-client honest fio and dd — DONE

**Done Sep 18 2026.** Writes share a ceiling and more clients make it
worse. Gate: `results/perf/20260918-w4-honest4/gate.txt`.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

##### W5 — Re-measure `sw-50g` after W3 — DONE

**Done Sep 18 2026** as the W4 morning 50g row:
`results/perf/20260918-w4-honest/` sw-50g **341** / sr-50g **2203**,
`FUSE_OK` `err=0`. W1 was 373. Later loaded reruns laid 50 GiB then
`end_fsync` EIO (the same REPORT tail). Completes when the path is
healthy; do not treat EIO as a reason to raise `EFS_IO_TIMEOUT_MS`.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

##### W6 — Run IO-500 (IOR easy, IOR hard, mdtest) — CORRECTNESS DONE, perf residuals open

**Correctness gate met Sep 20 2026** (commit `cc828d8`, servers
`708b350`+, TCP, 9 clients × 4 ranks): 9×4 debug
`results/io500/20260920-debug-9x4/` — **every phase finished, ior-easy-read
and ior-hard-read 0 verification errors, every unlink OK**. The path from
the Sep 18 numbers (hard-write DNF in 2 h 18 m, `-W` 4244 errors, 76108
easy-read errors, 27 undeletable files) to this is in
[../project-history.md](../project-history.md) "W6"; the fixes were: client
STALE retry cost + server partial-commit on STALE (hard-write livelock);
`dcache_image_current` / `snap_seq` ordering / forwarded-cmd reply index
(read coherency, `concurrent_appends`); Raft follower dedupe + leader AE
flow control (005 catch-up); **duplicate ino on concurrent CREATE**
(`alloc_hint_or_next` — apply is the allocator); reaper `lane-sweep`
batch-full misread as error (no file >8 MiB/lane was ever reclaimed).

| phase | Sep 30 18:35Z 9×4 RDMA | Sep 20 9×4 | Sep 18 9×1 |
| --- | --- | --- | --- |
| ior-easy-write | **4.563 GiB/s** | 0.814 | 0.263 |
| ior-hard-write | **0.640 GiB/s** (52.6 s, 0 fsync fail) | 0.044 (495 s) | 0.025 (9×4: DNF) |
| ior-easy-read | 16.8 GiB/s same-mount, 0 errors | 1.80, 0 errors | 0.60 (`-R` 512 errors) |
| ior-hard-read | **1.034 GiB/s, 0 errors; cold hardscan bad=0** | 3.55, 0 errors | 1.26 (`-R` 15982 errors) |
| mdtest-easy-write | **4.632 kIOPS** | 0.050 | 0.053 |
| mdtest-hard-write | 2.612 kIOPS | — | — |
| mdtest-easy-stat | 16.478 kIOPS | — | — |

Sep 30 row: `results/io500/20260930-183504-rdma/NOTE.txt` (§1b top).
Not a list submission (stonewall 1 s). Do not quote the Sep 19 easy-read
4.0 GiB/s — it was zero-fill. Do not quote stonewall intra GiB/s.

**Open under this item (performance, not correctness):**

1. **ior-hard-write** — **spans are the data path (Sep 27).**
   NP=4, SEGS=3000, 47008 B, one file, cold remount: write
   **481.56 MiB/s** (1.12 s), read **91.35 MiB/s** (5.89 s), pattern
   12000 records bad 0. posix2 **63/63**
   (`results/posix2/20260927-190509`). Prior curve
   `results/measure/20260921-162514-ior-hard-scaling`: **372 / 33 /
   69 / 82 MiB/s at 1 / 4 / 9 / 36**, about half of REPORTs
   `EFS_ERR_STALE`. The 4-rank point moved 33 → 482. 1/9/36 were not
   remeasured: group 2's REAP_DONE tail stayed ~11–16 entries/s
   (distinct inodes) and preflight fails above 5/s. Do not raise it
   (**D3**). Do not add a chunk lock. Do not tune 47008.
   **Sep 28, NP=9 via `run.sh ior`: STALE storm, fsync EIO, D-state
   ranks — W17.** **Decision D1 (Sep 28): the span publish commutes
   (no base-gen CAS for spans; sub-range writers always publish spans;
   fold by the chain-filler or a reader). Implement as W17 step 3.**
2. **1 GiB open costs 20 s of a 22 s easy-read** — 128 sequential
   GETCHUNKS + a 64-lane stat per open. Spec §8 per-lane range fetch
   ([performance.md](performance.md)) is the fix. **Decision D2 (Sep
   28): `open()` adopts the inode row only; `pull_layout_miss` is the
   one pull path, one GETCHUNKS per lane group in parallel, a metadata
   window one data window ahead of the prefetcher, size adaptive and
   internal (no knob).** Steps: (a) remove `pull_file_layout` from
   adopt for regular files; (b) make `pull_layout_miss` issue the
   window's lane-group GETCHUNKS concurrently and record the covered
   range per ino; (c) have the read prefetcher request the next
   metadata window when the data window advances; (d) keep the 64-lane
   size stat at open (size authority is not the map). Gate: 1 GiB cold
   open under 10 ms with 36 concurrent openers
   (`tests/measure/` open-cost script); easy-read not worse than
   `results/io500/20260928-150609-rdma`; `-R` errors 0 on both reads;
   posix 1 jobs=1 200/201. Forbidden: a whole-map pull anywhere;
   zero-fill on a miss; a mount option or env var for the window.
3. ~~mdtest `rmdir` ENOTEMPTY/EIO under load~~ — **FIXED Sep 21** (it was
   the parent-row lost update, not transient; §7.2 reductions). **Rate
   fixed Sep 27.** The flat 138 / 134 / 159 ops/s
   (`results/measure/20260921-161931-samedir-rate`) was one AppendEntries
   per create: a proposer that arrived while a fsync hold was open
   broadcast that single entry under the raft lock. Those proposers now
   append locally; the hold owner fsyncs the burst and marks every
   covered index durable; the pump sends one batch.
   `results/measure/20260927-211953-samedir-rate` (ROUNDS=100, storm
   PASS, parent `children=0 nlink=2`): **333 / 1385 / 1241 ops/s at
   1 / 9 / 36 procs**, `busy_n` 0 / 0 / 1. Idle mkdir median 3.1 ms.
   Do not change the backoff or the txn protocol. Do not lower
   `EFS_DIR_SPREAD_MIN`.

Harness (`tests/perf/io500/run.sh`): `SLOTS=4 NP=36 … debug` detaches
`prterun` and logs to `$IO500_DIR/last-run.log` (the ssh timeout used to
kill it); `ior-easy-write|verify <mb>` and `ior-hard-write|verify <segs>`
run IOR directly so a cold verify is possible (the io500 driver deletes
its data at the end of a run).

- **Read:** `tests/perf/io500/README.md`; the fio rule's FUSE check
  applies to every rank.
- **Gate (met):** 9×4 debug with 0 `-R` errors on both reads. For the
  three open sub-items the gate is the number in the table moving with
  the same harness and 0 errors kept.
- **Forbidden:** quoting a rank that fell back to local disk; tuning
  IOR's transfer size (47008 is the point); a chunk lock (W1); `pkill -f`
  (matches the agent). Kill hung `io500` with `pkill -9 -x io500` then
  remount FUSE (D-state `request_wait_answer` ignores SIGKILL until
  `efs-fuse` dies). Never gdb-attach an MPI rank through a timeout'd ssh
  (left a rank T-stopped, job unrecoverable).

##### W7 — Two POSIX suite-1 tests exceed the 15 s budget even in isolation — DONE

**Done Sep 21 2026.** The 15 s failures were the metadata wakeup floor
(fixed Sep 20), not a remaining per-test bug. Isolated on an idle cluster,
`results/measure/20260921-133437-posix-isolated` (budget 15 s, jobs=1):

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

##### W8 — 9-node POSIX suite 1 (gate: 201 rows, 0 NOTRUN, every host)

**State (Sep 21 22:15): 191–195 / 201 on every host, 0 NOTRUN, all nine
finish in ~62 s** (`results/posix/20260922-020950`). History and the
three fixes that got here are in the §1b progress log (harness clock at
submit; `h->mu` contention after `read_mu`; KV WAL fsync per apply).
Earlier symptoms — nine hosts at the 385 s cap with `[None]` rows
(`results/posix/20260917-191430`), the 1.2 s / 1.03–1.08 s root mkdir,
`rmdir` ENOENT on a just-created name, `EBUSY` after the 10.4 s backoff —
are closed: whole-shard txn scans (`165e779`), stranded txn records
(`9534e53`, `3291c6d`), `read_mu` (`223da15`), peer-pool starvation
(`f10fec0`), harness (`a683def`), view (`4eb1419`), WAL hold (`84a2a55`).

What still fails, in order (details and instructions in §1b):
0. Half-applied cross-shard txns (I17) — **fixed `46d54e6` and gated**
   (Sep 22): freeze both leaders during `same_parent_storm`. The parent
   row stayed consistent (`nlink=5 nents=3` with three real children on
   the run that left names behind; the other run removed the parent).
   `arc_term_miss` moved. Details in §1b.
1. Retry of a committed non-idempotent op after a BUSY (EEXIST on a
   fresh LINK name, EIO, empty read) → I16 op-id dedup for
   LINK/UNLINK/MKDIR/RENAME. Mechanical, spec §7.9.
2. The synchronous full-L1 compaction: 2.4 s under `h->mu`+`l->mu` on
   every replica at once, costs the leader its term. **Decision.**
3. Six many-op tests at 144 concurrent jobs (mkdir p50 21 ms under
   load; apply 0.3 ms/entry of KV reads). Measure, then decide.

Run it as `bash tests/measure/w8_stall_timeline.sh` (runs the suite,
gives the raft/latency timeline) and read `raft-obs:` from the four
`efsd.log`s. Do not raise the 70 s warmup, the 15 s per-test budget,
or the 400 s suite timeout; do not point the suite at a subdirectory;
do not raise the election timeout.

- **Forbidden:** reporting `[None]` rows as failures, or as passes;
  the clock-at-submit bug coming back (a queued test cannot time out).

##### W9 — The client staging table is unbounded

`g_client.export` in `efs-fuse` keeps one row per inode this client has ever
touched and one entry per chunk it has written or pulled, and evicts nothing —
so a client that walks a large namespace holds the whole tree in RAM and the
server-side memory wall reappears per client. This is step 12 part A.
The evictor and the pin rules are in the client as of Sep 27.

The plan, the pin rules that make eviction safe (a report builds its records
out of this table, so evicting a dirty row is data loss), and the gate are in
[../client-cache-design.md](../client-cache-design.md). The Sep 23
recommendation below is in the client as of Sep 27. Posix 1 is
200/201, posix 2 is 59/63, the leak gate is clean, and a cold stat
of 1M files leveled at 233 MB RSS
(`results/measure/20260927-w9-walk`). Done.

**Recommendation (Sep 23), implemented Sep 27:** (1) The pin rules —
dirty / publishing / dirty dcache, ghost with an open fd, in-flight op pin,
live append reservation or lock — are exactly the set of rows whose absence
a report or an open fd could observe; anything narrower loses a write,
anything wider is not a bound. (2) `EFS_CLIENT_META_MB` = 256 MB default,
soft: at ~200–300 B per row plus chunk maps that is on the order of a
million rows, above any FUSE working set we have measured (IO-500 9×4,
the posix suites, `find` over the 410-name root), and a table where
everything is pinned grows and logs once rather than evicting work. Pick
a different number only if a client RSS measurement says so; the cap is
an env var, not a protocol.

Steps, once ratified:
1. Pin bookkeeping: a per-row pin count set by the dirty/publishing sets,
   `efs_open_note`/`close_note` for ghosts, and a scoped pin in every
   dual-apply window (create, rename, link, unlink). Unit test: a report
   built while eviction runs never skips a dirty row.
2. LRU by last-touch tick over unpinned rows; evict chunk maps of clean
   closed files first, then rows. Ghost reclaim at last close.
3. `statfs` from server numbers; `efs_export_fits_page_cap` becomes a
   best-effort early-out (the doc's §4).
4. Gate: `make test`; posix 1 (jobs=1 and 9-host) and posix 2 with no new
   failures; the walk-RSS gate — one client walks a multi-million-file
   tree and RSS stays near the cap where it grew linearly before; the
   valgrind leak gate (eviction is a new free path).

- **Gate:** the walk-RSS gate in that doc, plus posix 1 + 2 and `leaks`.
- **Forbidden:** evicting a row that is dirty, publishing, a ghost with an open
  fd, or pinned by an in-flight op. Bounding it by dropping records instead of
  refetching them.

##### W10 — RDMA — live on 19810 Sep 28, not faster than TCP

Sep 28 gate on `db2b88c4802a-dirty` (`EFS_TRANSPORT=rdma`,
`EFS_RAFT_OBS=1`). posix jobs=1 199/201 in 90.1 s
(`results/posix/20260928-033823`); 9-host 199/201 on all nine,
56.7–59.1 s, 0 fail, mmap + xattr SKIP
(`results/posix/20260928-034049`); posix2 63/63 in 76.8 s
(`results/posix2/20260928-034350`). After user xattr, jobs=1 is
**200/201** (`mmap_write_read` SKIP only,
`results/posix/20260928-043918`); 9-host was not remeasured.
9-client 8 GiB dd+fsync
**1326.6** MiB/s, walls 28.55–55.57 s, every file 8589934592
(`results/measure/20260928-033420-dd-wall`). TCP bars are posix
9-host ~13–15 s and 9-client dd 1478. The outbox fix that let
fcstor004 catch up is in this build. The speed bar in step 3
below is still open. Do not roll back to TCP unless a suite fails.

The Sep 27 live switch below was rolled back the same hour. It is
history, not the current cluster.

Private gate passed 5/5 (`tests/rdma_first_inode.sh` on fcstor007).
The live switch (`EFSD_ENV='EFS_TRANSPORT=rdma EFS_RAFT_OBS=1'`,
`roll_efsd.sh --all`, clients remounted `EFS_TRANSPORT=rdma`) did not
match TCP. 9-host posix jobs=1 was 193–196/201, skip
`mmap_write_read`, 0 not-run, duration 385 s on every host
(`results/posix/20260927-044348`). The TCP bar on this tree is
200/201 in 30.4–31.3 s. What failed that passes on TCP: the many-op
tests (`dir_deep_nesting`, `dir_deep_nesting_beyond_64`,
`names_crazy_dirs`, and on some hosts `concurrent_creates_same_dir`
and `mtime_monotonic_many_writes`) hit the 15 s cap, plus a few
EIO/EEXIST one-offs. During that run `apply_max` on fcstor003 stayed
under 1 ms. Step 5 rolled 19810 back to
`EFS_TRANSPORT=tcp EFS_RAFT_OBS=1` the same hour; clients
fcstor007–015 are TCP and a mkdir/rmdir on fcstor007 returned
immediately. Freeze, idle `md_latency`, and the dd rebaseline were
not run on RDMA. Do not debug this on the live cluster.

The private-cluster gap (100 mkdirs, fcstor007, ports 19950–19952)
was two bugs in `src/common/rdma.c`, both fixed in tree and not
rolled. The shared recv poller acked the completion-channel event
and then slept 100 ms, so a work completion already in the CQ
stalled every conn (11 of 100 mkdirs; 9-host posix then hit the
15 s cap). It now harvests again before a 1 ms backstop. Every
SEND also called `ibv_query_qp` and read a port counter, which made
a raft AppendEntries ~380 µs against ~15 µs on TCP; those reads
happen only after a send has already failed. With both fixes, one
clean run was 642 ms RDMA vs 507 ms TCP and raft RTT ~50 µs
(`~/efs-runs/rdmaprof7.log`). The Sep 28 live gate is the paragraph
above this section. 19810 is RDMA. The "same numbers as TCP or
better" speed bar is not met.

The steps below are the procedure that was followed, kept so the
gate stays findable.

The client connection-pool lifecycle fix is in tree and unit-gated
(`test_conn_fd`): a pooled conn pins its socket identity, checkout evicts on
mismatch, destroy refuses to close a recycled fd. The **live** repro was never
re-run, because it only reproduces on a freshly `mkfs`'d / effectively empty
table, and 19810 is populated. A remount there is *not* this gate.

19810's transport is RDMA as of the Sep 28 gate in §1b. Suites pass.
Posix and the 9-client write are still slower than TCP, so the speed
bar in step 3 is open. Every ceiling in the table above assumes the
data path can use the fabric; TCP over IPoIB will not reach it.

**Recommendation (Sep 23): do not wipe 19810 for this.** The repro needs an
*empty* table, not *the* table: `tests/rdma_first_inode.sh` stands up a
private 3-node efsd on one host (fcstor007, ports 19950–19952, storage
under `/tmp/efs-rdma-first`, `EFS_TRANSPORT=auto`) and runs the first
`mkdir`. That is the gate, and it touches nothing on 19810. The live
switch of 19810 to RDMA then needs no fresh table either — the project
state rule records that a populated table drove millions of creates over
RDMA with 0 errors; the empty-table hang was the only open defect. A wipe
buys nothing here that the private cluster does not; keep it for W11
(below), whose gate is easier on a table that grows from zero.

Steps:
1. `efs-bg.sh start w10-first 'bash tests/rdma_first_inode.sh'` with
   `EFS_RUNNER=fcstor007.ib`, ×5 (the tree must be built on 007 first:
   `efsd`, `efs-mgmt`, `efs-fuse`). Pass = every run prints
   `MKDIR_RC=0 WRITE_RC=0` and exits 0 (the first `mkdir` and a write
   inside it each have an 8 s `timeout`). Fail = any `MKDIR_RC=124`, and
   the fix is not in; stop and bring `/tmp/efs-rdma-first/fuse.log`'s
   `RDMA transport|WAIT TIMEOUT|SOCKET CLOSED` lines.
2. Switch the live cluster: `tests/roll_efsd.sh --all` with
   `EFSD_ENV="EFS_TRANSPORT=rdma EFS_RAFT_OBS=1"`, then remount the
   clients with `EFS_TRANSPORT=rdma`. Wait for `commit` flat on both
   groups (post-roll election churn is ~2–5 min).
3. Gate on RDMA, same numbers as TCP or better: posix jobs=1 (200/201),
   9-host posix (≥185/201, 0 not-run), `i17_leader_freeze.sh` ×2 with
   0 worker errors, idle `md_latency.py` within the TCP reference.
4. Re-baseline the write wall (`efs-fio-honest`: 8 GiB dd+fsync 1/4/9
   clients) and record it in §1a's ceiling table. Then TCP is no longer
   the default in the deploy rule and START-HERE.
5. If step 3 fails on anything that passes on TCP, roll back to TCP with
   the same `roll_efsd.sh --all` and bring the failure; do not debug
   RDMA on the live cluster with the suites down.

- **Read:** the root cause and what was already disproven is in the project
  state rule — the RNR-NAK/recv-buffer hypothesis is **dead**, do not re-chase
  it.
- **Forbidden:** re-deriving the diagnosis; wiping 19810 without being asked
  (this item no longer needs it).

##### W11 — chunked InstallSnapshot — DONE Sep 27

Running build `612ee9ba9202-dirty`, TCP. The log truncates. A follower
install is a sorted diff (`src/kv/kv_snap.c`). fcstor005 rejoined in
510 ms. 9-host posix is 200/201, skip `mmap_write_read`, 30.4–31.3 s
(`results/posix/20260927-033723`). The earlier 199–200 run is
`results/posix/20260927-015719`. Gate numbers are in
`results/measure/20260927-w11-gate` and `docs/project-history.md`
(Sep 27). The original recommendation, kept so the steps stay
findable:

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

##### W13 — Synchronous full-L1 compaction is the remaining election trigger — DONE Sep 26 2026

Gate is at the top of §1b. Steps 1–4 landed: pinned view, background
compactor, flush no longer compacts, and the 9-host suite kept
`apply_max` under 100 ms with no term change. Step 5 (partitioned
flush) is in the TCP build rolled Sep 27 05:07 UTC. A flush writes
one L0 file per `key[0]` (at most 16 on efs keys, shard in the first
two bytes). Compaction merges one of those ranges plus the L1 files
that overlap it, and cuts an output file when the range changes, so
the next cycle does not rewrite the other ranges. `make test` on
fcstor014 passed, including `test_partitioned_flush`. The live table's
existing L1 files are still wide until the next compaction rewrites
them; the byte bound is the steady state after that.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

##### W14 — Server: snapshot install and the fragment probe are on the write path

**Status (Sep 28 evening).** Step 1 is done in `7eecf1d` (the diff is
prepared on the GC thread; the pump applies it on the ack) and
`bbcbcb5` (the pump's `host_snap_open` no longer flushes a memtable it
cannot place; a no-progress snapshot ack waits one heartbeat). The
`send_snap` and `kv_flush_locked` stacks are gone from the run-3
profiles. Step 3's reap runs at `efsd` start. Step 4 carries
`path_hint` on the next PUT and probes the other roots only on a
miss. Step 5 sizes the send-buffer pool to `EFS_WRITE_PIPELINE`.
Step 2 (the election trigger) is still open. The
L1 cap that kept fcstor004/005 from compacting is removed in
`bbcbcb5` (growable L1 list; L0 still 64).

**Source (Sep 28 2026, 13:30–13:45 EDT).** `perf record -F 499 -g` attached
to all four `efsd` on 19810 (RDMA, `2f086f5a0ef8-dirty`) while fstor007 ran
`ecopy` and then a `dd` that did not start. Reports:
`~/orcd/scratch/efs/perf/efsd-19810-fcstor00{3,4,5,6}/{flat,by_thread,callers}.txt`.
The recorded binary was replaced on disk by the roll, so user frames are
raw addresses in the reports; resolve them with
`addr2line -f -C -e /tmp/efs/efsd <addr>` on that host (the file there is the
same build). fcstor005, 265K samples:

| share | stack | what |
| --- | --- | --- |
| 16% | pump → `drain_inbox` → `on_snap_req` → `host_snap_chunk` → `efs_kv_group_import` → `efs_kv_scan` (`lsm_scan_from`), `memcmp` 12% self | a follower installing a snapshot: full local KV scan under `l->mu`, then sort + diff of every key, **on the pump thread under `h->mu`** |
| 12% | `writer_thread` → `run_job` → `server_write_fragment_with_sum_sync` → `pwrite` | fragment writes (the useful work) |
| 10.6% | `server_handle_conn` → `recv` (`copyout`) | PUT payloads over TCP |
| 10.4% | `server_handle_conn` → `server_find_fragment_root` → `access()` ×6 (`__d_lookup_rcu`, `link_path_walk`) | six path walks per PUT to find which of the six roots has the fragment |
| 8.6% | `host_gc_thread` → `host_snap_export_pass` → `vx_pop` / `msort` | a leader exporting a snapshot |
| 7.5% | `compactor_main` → `kv_compact_locked` → `cm_push` | L1 compaction |

`raft-obs` in the same window: group 0 changed term at 13:31, 13:37, 13:40,
13:43–13:44; group 2 at 13:39–13:40. Every leader change re-exports and
re-ships a snapshot. Every server holds 5–12 abandoned
`/data1/01/efs/mdraft/snap-*.kvx.tmp` files (34 MB–960 MB each, from 00:41
through 11:24 that day); nothing removes them.

**Why the `dd` did not start.** `create` is a group-0 commit that waits in
`host_wait_settled` for the pump. The pump was inside
`efs_kv_group_import` under `h->mu`, and group 0 was re-electing. The
client's 16 BUSY/NOT_PRIMARY retries back off to 800 ms each, so the
`open()` sat for over ten seconds with nothing printed.

Steps, in this order; each is its own change with its own gate:

1. **Take `efs_kv_group_import` off the pump thread.** The scan and the sort
   run on the GC thread (the export already does). Only the resulting diff
   batch is applied under `h->mu`. `on_snap_req` hands the finished `.part`
   file to that thread and answers the leader when the diff has been
   applied. Gate: `perf` on a follower during a forced InstallSnapshot
   (`tests/measure/i17_leader_freeze.sh` or a bounce of one voter) shows
   `efs_kv_scan` off the pump, and `raft-obs` `apply_max` stays under
   70 ms while the install runs.
2. **Find the election trigger and stop it.** Run
   `tests/tools/raft_log_tail.py` on fcstor004's `raft.log` for the
   13:37 and 13:43 windows. If step 1 removes the term changes under the
   same load, this is done. If not, the follower that cannot answer
   AppendEntries during an import needs the heartbeat answered from a
   thread that is not importing. Do not raise the election timeout.

   **Measured cause candidate (Sep 28 22:19 EDT, after step 1 landed in
   `bbcbcb5`).** Terms still move: group 0 9309 → 9368 and group 2
   2763 → 2789 between 00:39Z and 02:19Z with no roll in between.
   fcstor004 (`raft_id` 1; follower in group 2 whose leader is
   fcstor006 = `raft_id` 3) prints
   `raft-obs: tx->3 enq=50698 drop=72114 sent=50695 hi=2048`: the
   per-peer outbox to the group 2 leader hit `HOST_OUTBOX_MAX` (2048)
   and **dropped more frames than it sent**. `outbox_must_keep` protects
   only `SNAP_REQ` and entry-carrying `AE_REQ`; the victims are
   AppendEntries **replies**, heartbeats, and vote traffic. A leader
   whose follower's replies are dropped re-sends after the `ae_inflight`
   timeout and never sees the match advance; a follower whose heartbeats
   are dropped on the way in campaigns. `rtt_avg=239us` on that link and
   a synchronous request/reply `host_sender` bound one peer link to
   roughly 4 000 frames/s, which the leader's AE rate under 36 writers
   exceeds; the excess is dropped, not queued. `wait_timeouts=366`
   on the same node are the client-visible BUSYs (W16).

   Fixed implementation, in this order:

   **(a)–(b) are in tree and rolled 02:35Z Sep 29.** Gate result: `drop=0`
   on every `raft-obs tx->` line on all four nodes over a 9-client
   `run.sh ior` (was `drop=72114 > sent`), but terms still moved
   (group 0 +45, group 2 +105). The drops were not the trigger; the
   follower's InstallSnapshot import is — see **W22** / D5. `hi` still
   reaches 2048 on fcstor003→1 and fcstor004→2 (entry lane full → AGAIN
   to `send_ae`, no drop), which is (c)'s measurement.

   a. **Coalesce instead of drop.** An AE reply to peer P supersedes any
      older AE reply to P still queued (only the latest `match`/`term`
      matters); a heartbeat (empty `AE_REQ`) supersedes an older queued
      heartbeat to P; a vote/pre-vote message is never dropped. When the
      outbox is full, replace the superseded item in place; only if
      nothing is replaceable return `EFS_ERR_AGAIN` to the caller (as
      `send_ae` already handles for entry AEs). Gate: `drop=0` in every
      `raft-obs tx->` line over a 9-client `run.sh ior`; both groups'
      terms unchanged across the run.
   b. **Do not let one peer's backlog hide a heartbeat.** If (a) leaves
      `hi` at the cap, the sender needs the heartbeat ahead of queued
      entry AEs: dequeue heartbeats and replies before entry AEs
      (two-lane outbox), keeping entry AEs in order among themselves.
   c. **The per-link rate is a design question.** `host_sender` waits
      for `RAFT_REPLY` per frame. Letting more than the one in-flight
      batch ride a link is pipelining and is forbidden below; batching
      several reply frames into one RPC is not, but it changes the wire.
      Measure after (a)+(b): if `hi` still reaches the cap with
      `drop=0`, bring the frames/s and RTT numbers to the user.
3. **Reap dead `snap-*.kvx.tmp`.** At `efsd` start, and whenever an export
   is abandoned (`send_snap` BUSY path, leader step-down), unlink every
   `snap-<group>-*.kvx.tmp` that is not the one in progress. Gate: after
   a roll, `ls /data1/01/efs/mdraft/*.tmp` is empty on all four.
4. **Drop the six `access()` calls per PUT.** **Review after the Sep 29
   02:35Z run:** the hint as specified only helps a re-PUT of the same
   fragment; a first write has no previous PUT to carry a hint from, so
   IOR-easy and a fresh `dd` still walk all six roots (fcstor004: 1.59M
   `access()`, 192 s across six handler threads). **Step 4 (b), decided
   D7 (Sep 29):** the client sends `path_hint = 0xffffffff` when its
   dcache slot has never PUT this fragment generation (no recorded hint,
   not a STALE replay, not a retry of a failed reply); the server then
   skips `server_find_fragment_root` and creates on the writer's
   least-queue root. A fragment name is `{ci}.{fi}.{gen}`, so a first
   write of a new generation cannot collide with an existing file; the
   quota charge stays on the create path. Any re-PUT sends the recorded
   root or 0. Gate: the step-4 gate below (under 1 % on a single-client
   dd) plus `peer_shared_pwrite` / `concurrent_appends` (re-PUT paths
   still probe). Order-table row 8c. A global fd cache hung the
   9-client dd; a thread-local fd cache removed the sample and made the
   slowest client worse (both reverted, see `docs/project-history.md`
   Sep 28). Do not retry either. The PUT reply already tells the client
   which storage path took the fragment; carry that `path_index` back on
   the next PUT of the same `(ino, ci)` as a hint and probe only on a
   miss. Gate: `server_find_fragment_root` under 1% in a single-client
   8 GiB dd profile, and the 9-client dd slowest wall not worse than
   `results/measure/20260928-134637-dd-prof-r5b` (2551.5 MiB/s).
5. **`send_buf_pick` spin.** `pthread_spin_lock` inside
   `efs_rdma_send_frame` is 2.3% of the client and has a server
   counterpart. Size the send-buffer pool per QP so a pick does not
   contend, or hand each sender its own ring. Gate: the spin is gone from
   `flat.txt` on both sides.

- **Gate:** items above, plus posix 1 jobs=1 and 9-host, posix 2, and a
  1-client and 9-client 8 GiB dd with the flush in the clock, none worse
  than the Sep 28 numbers in `.cursor/rules/efs-fio-honest.mdc`.
- **Forbidden:** raising the election timeout or `HOST_TICK_US`;
  chunking InstallSnapshot differently (W11 is done); the global or
  thread-local fd cache; changing `EFS_RAFT_SNAP_CHUNK`, `HOST_PUB_BATCH_N`
  or `EFS_RAFT_AE_BYTES` (all three were measured worse on Sep 28).

##### W15 — Client: copies and busy-waits are the write CPU

**Status (Sep 29).** Step 2 is done in `bbcbcb5`
(`efs_rdma_reply_ready_us` pauses at most 16 times, then the caller
blocks on the CQ fd); on the run-3 profile the vDSO is 0.87% and the
symbol is under the 0.5% floor. Step 1's fresh profile is run 2 /
run 3 in §1b: `ll_write_buf` `memmove` 7.5%, RDMA send `memmove`
6.3% (step 3 stands), and no `memmove` caller under
`dcache_flush_slot_inner` above the 0.5% floor (the body-drop path is
taken). Run 2's client report files were overwritten by run 3 (same
`efs-mount` directory); those three numbers survive only here. The
larger client item was the reclaim walk, W18, which is in tree.
Step 3 hands a whole chunk to the dcache after one `fuse_buf_copy`
(the request buffer does not outlive the reply). Step 4 is the
same send-buffer pool as W14 step 5.

**Source.** `client.sh --perf` on fstor007 during the same `ecopy`
(`~/orcd/scratch/efs/perf/efs-mount/{flat,by_thread,callers}.txt`,
371K samples). This profile is of the binary **before** the Sep 28
dcache changes (full-chunk body drop, no snapshot copy, running
`staged_bytes`); take a fresh one first and strike whatever those already
removed.

| share | where |
| --- | --- |
| 36% | `memmove`: 15.4% in `ll_write_buf` (FUSE copy-in + dcache patch), 3.5% into the RDMA send buffer, the rest under `dcache_flush_slot_inner` (snapshot copy + `dcache_install_image` copy-back) |
| 7.5% + 5.8% | vDSO `clock_gettime` and `efs_rdma_reply_ready_us` from `efs_conn_reply_watch_us`: busy-waiting for the PUT reply |
| 7.1% | `blake3_hash_many_avx512` |
| 5.2% | `efs_export_staged_bytes` (the evictor walk; replaced by a running total Sep 28) |
| 4.6% | `xor_into` (parity) |
| 2.3% | `pthread_spin_lock` in `efs_rdma_send_frame` (`send_buf_pick`) |
| 2.2% | `__lll_lock_wait` from `dcache_put_now` |

Kernel futex + schedule under 3%: the client is not lock-bound. It is
copying and spinning. `ecopy` and `dd` on the same mount were not blocked
by each other; the `dd` was blocked on the server (W14).

**Re-profile, Sep 28 20:36–20:39 EDT (fstor007 `client.sh --perf`,
214K samples, same report dir, binary with the body-drop and
`dcache_ensure` changes).** `memmove` is still 35%. Resolved by call
address (`perf report --no-children --symbol-filter=memmove -g
caller,address` on fstor007): 14.6% under `ll_write_buf`
efs_fuse.c:2569 (the `efs_dcache_try_patch` copy bounce → dcache),
3.6% into the RDMA send buffer, and **~17% with no caller at all**.
That last share is `fuse_buf_copy` in `efs_fuse_write_buf` (libfuse
is built without frame pointers, so the chain stops at `memmove`).
Every written byte is copied kernel → libfuse buffer (`memcpy_erms`
1.6%), libfuse buffer → bounce (`fuse_buf_copy`, ~17%), bounce →
dcache (14.6%), dcache → RDMA send buffer (3.6%). Step 3's gate counts
the bounce copy too, not only the `ll_write_buf` line. `memmove` under
`dcache_flush_slot_inner` is gone (body drop landed). The vDSO share is
back at 5.7%, but from a different place: 2.5% `now_us()` in
`send_buf_pick` (inlined into `efs_rdma_send_frame`) plus 1.7%
`pthread_spin_lock` there — waiting for one of `EFS_RDMA_NSEND` = **2**
pool send buffers per QP; a third 64 KiB PUT on the same conn spins.
That is step 4 / W14 step 5, with its cause named. Two things in this
profile are not on any item and are now **W20** and **W21** below:
`ll_setattr` → `efs_fuse_getattr_ino` → `fill_stat_from_inode` is 8.9%
(a stat walks every chunk of the file), and `efs_export_staged_bytes`
is 6.6% self on the evictor thread (it was not replaced by a running
total; the per-call loop over `shard_tabs` and up to 1024 passes per
wake are still there).

Steps:

1. **Re-profile on the current binary** (one client, 8 GiB `dd bs=1M
   conv=fsync`, non-zero source, `client.sh --perf`, `stop` writes the
   reports). Record the three shares above again. If `memmove` under
   `dcache_flush_slot_inner` is still above 5%, the body-drop path is not
   being taken for the sequential write; find out why before anything
   else.
2. **Replace the reply busy-wait.** `efs_conn_reply_watch_us` +
   `efs_rdma_reply_ready_us` spin on `clock_gettime`. A PUT reply is
   behind a disk write, tens of milliseconds. Spin for at most ~50 µs,
   then block on the CQ event fd (`rc->efd`) the way `efs_rdma_recv_wait`
   already does. Gate: the two symbols together under 2% on the dd
   profile; the 1-client dd wall not worse.
3. **Cut the FUSE copy-in for whole chunks.** `ll_write_buf` copies the
   libfuse buffer into the dcache. When one write covers a whole 128 KiB
   chunk, hand the libfuse buffer to the slot as the body (the body-drop
   path then sends it and frees it). Check `fuse_buf_copy` ownership in
   libfuse 3.10.2 before assuming the buffer outlives the reply. Gate:
   `ll_write_buf` `memmove` under 5%.

   **Review of the in-tree version (Sep 28 22:00, `efs_fuse_write_buf`
   "W15.3").** It copies once only when `size == cs && offset % cs == 0`,
   i.e. a write of exactly one chunk. `max_write` is
   `EFS_WRITE_PIPELINE × cs` (≥ 1 MiB), so `dd bs=1M`, IOR `-t 1m`, and
   every `cp` deliver 1 MiB writes = eight chunks, and those all fall
   through to the bounce + `efs_dcache_try_patch` path. The 22:11
   profile of this binary shows it: `fuse_buf_copy` ~19% + `ll_write_buf`
   16%. Fixed implementation: walk the write in chunk-aligned pieces.
   For every whole chunk inside `[offset, offset+size)` allocate the
   dcache-owned buffer (`efs_buf_alloc(cs)`), set `dst` to that one
   buffer and call `fuse_buf_copy(&dst, buf, FUSE_BUF_NO_SPLICE)`;
   `fuse_bufvec` keeps the source position (`buf->idx`, `buf->off`), so
   successive calls consume the request buffer in order without a
   bounce; store each with `efs_dcache_store_full_owned`. Only a partial
   head or tail chunk goes through a bounce of its own length into
   `efs_dcache_try_patch`. Keep the O_APPEND reservation and the
   `stage_unpin` exactly where they are. If a mid-write store fails,
   fall back to the old path for the rest of the request (no partial
   success visible to the caller). Gate: on a 1-client `dd bs=1M`
   profile, `memmove` under `ll_write_buf` under 2% and total `memmove`
   with no caller under 5% (that is the single remaining copy); posix
   2 `peer_shared_pwrite` / `concurrent_appends` unchanged.
4. **`send_buf_pick` spin** — same as W14 step 5, one change for both
   sides.

   **Review (22:11 profile, `NSEND = EFS_WRITE_PIPELINE` in the binary).**
   The vDSO + `pthread_spin_lock` share did not move (6.9%). The
   callers are `now_us()` and the spin at `efs_rdma_send_frame`
   lines ~1257–1301: after `post_send` the sender **waits for its own
   send CQE** (spin 200 µs, then `sched_yield` until `send_busy[idx]`
   clears). More pool buffers cannot help; each PUT fragment still costs
   the thread one HCA round trip before it can post the next. The
   comment says the wait exists so a failed first SEND after upgrade
   surfaces as EIO instead of hanging in recv. On the 22:48 single-`dd`
   profile the vDSO share was 0.8%, so the wait costs CPU mainly when
   several writers share one QP; the gate below still applies to the
   seven-`dd` shape. Fixed implementation:
   post and return; reap send CQEs where they are already reaped
   (`send_buf_pick` on the next send, and in the reply wait —
   `efs_rdma_recv_wait` / `efs_rdma_reply_ready*` call `reap_sends`
   before blocking, and a send CQE with an error status marks
   `rc->broken` and makes the pending reply wait return `EFS_ERR_NET`).
   That preserves the "failed send becomes EIO on the reply path"
   property without a synchronous wait. Keep the 5 s
   `EFS_RDMA_SEND_WAIT_US` as the reply-wait's dead-QP bound. Gate:
   vDSO + `pthread_spin_lock` under 1% on the dd profile;
   `tests/test_rdma_xprt` and `tests/rdma_first_inode.sh` pass
   (the first-SEND-after-upgrade case is the one the wait was added
   for); 9-client dd not worse than 2551.5.
5. **The last user-space copy: let the kernel fill the dcache buffer.**

   **Checked Sep 29 15:33Z on fstor007 (`~/efs-runs/rec-splice1.log`):
   cannot take effect on this cluster as configured.** libfuse
   3.10.2's `fuse_session_receive_buf_int` uses the pipe only if it
   can grow it to `se->bufsize` = `max_write` + 4 KiB; `efs_fuse_init`
   sets `max_write` to `EFS_WRITE_PIPELINE × cs` = 4 MiB, and
   `/proc/sys/fs/pipe-max-size` is 1048576, so `F_SETPIPE_SZ` fails
   for a non-root process and libfuse falls back to the buffer path
   for good (`can_grow = 0`). **Resolved 15:45Z: the user set
   `fs.pipe-max-size=8388608` on node9901, fstor007, fcstor003–015
   (`sysctl -w`, runtime only — it reverts on reboot like
   `ptrace_scope`; ask the user to re-apply, do not edit
   `/etc/sysctl.d`).** `efs_fuse_init` now sets `FUSE_CAP_SPLICE_READ`
   when libfuse offers it. Rolled 16:09Z on the servers; clients are
   not mounted, so this is not gated. The gate
   below stands; the `fuse_copy_page` share is the number to compare.
   Lowering `max_write` under 1 MiB was the other route and was not
   taken (every `dd bs=1M` write would become two requests).

   After step 3 (verified 22:48: `ll_write_buf` `memmove` gone) every
   written byte is still copied twice: kernel → libfuse request buffer
   (`fuse_copy_page`/`memcpy_erms`, ~7% + 2.5% kernel) and libfuse
   buffer → dcache (`fuse_buf_copy`, ~22% user, caller-less in perf
   because libfuse has no frame pointers). libfuse 3 can receive a
   write's payload through a pipe instead of its buffer
   (`FUSE_CAP_SPLICE_READ`; `fuse_session_receive_buf` marks the
   payload `FUSE_BUF_IS_FD`), and `fuse_buf_copy` from an fd buffer
   into a memory buffer is one `read()` from the pipe into the
   destination — the dcache buffer step 3 already allocates. Net: one
   kernel copy per byte, none in user space. Do this as its own
   change: in `efs_fuse_init` set `conn->want |= FUSE_CAP_SPLICE_READ`
   when `conn->capable` has it; keep `FUSE_BUF_NO_SPLICE` on the
   `fuse_buf_copy` calls (it only forbids `splice()` for fd→fd, which
   we never do). Verify first, on fstor007, three things libfuse
   3.10.2 decides at runtime: that a 1 MiB write is actually delivered
   as an fd buffer (libfuse copies small requests to memory; check the
   threshold in `fuse_session_receive_buf_int`), that the pipe can hold
   `max_write` + header (`F_SETPIPE_SZ` vs `/proc/sys/fs/pipe-max-size`,
   1 MiB default — if the cap is below `max_write` + 4 KiB the request
   falls back to the buffer path and nothing changes), and that the
   `write_buf` path is the only consumer (`efs_fuse_write` with a plain
   pointer must not be reachable for fd buffers). Gate: on the 1-client
   `dd bs=1M` profile, caller-less `memmove` under 3% and total
   `memmove` under 12%; `fuse_copy_page` unchanged or lower; posix 1
   jobs=1 200/201, posix 2 63/63 (`peer_shared_pwrite`, appends,
   O_DIRECT tests); 1-client dd wall not worse than 947 RDMA.

- **Gate:** posix 1 jobs=1 (200/201, `mmap_write_read` SKIP only), posix 2
  63/63, 1-client 8 GiB dd ≥ 977 MiB/s TCP / 947 RDMA (the Sep 27–28
  numbers), 9-client dd not worse than 2551.5.
- **Forbidden:** clearing `FOPEN_DIRECT_IO`; touching `entry/attr_timeout`;
  replacing blake3 (the hash is not the wall; W3); putting the
  `sched_yield` loop back in `recv_poller` or the 200 µs clock spin back
  in `efs_rdma_recv_wait`.

##### W16 — Under a write flood, a metadata read fails after 10 s and surfaces as ENOENT

**Source (Sep 28 2026, ~18:30 EDT).** fstor007-mgmt mounted 19810 with
`client.sh --perf` while the 9×4 IOR was writing. Mount succeeded
(`fuse serving`, RDMA up). `df -h /tmp/efs-mount/` printed
`No such file or directory`. The fuse log has one line for it:
`inode-rpc: shard=0 type=47 exhausted 16 BUSY/STALE retries (10.3 s) -> EBUSY`.
Type 47 is `EFS_MSG_INODE_GETATTR`; shard 0 is the root's shard (even →
group 2). The mount was alive; `client.sh stop` unmounted cleanly. Same
class as the `INODE_LOOKUP` on shard 3745 that aborted IOR's `stat` in the
22:00Z run (§1b). Not data loss, not a dead mount, not RDMA.

Two defects, one visible and one underneath:

1. **Error mapping.** `efs_client_stat_ino` and `efs_client_stat_refresh`
   (`src/client/ops.c`) return `EFS_ERR_NOT_FOUND` for *any* RPC failure;
   `ll_getattr` (`src/client/efs_fuse.c`) maps every non-ACCES failure to
   `-ENOENT`. An overloaded cluster therefore looks like a missing
   directory to `df`, `ls`, and every path walk. The comment above the
   `no-hint` retry in `rpc_send_recv_shard` already names this.
2. **The read starves.** A GETATTR is a linearizable read:
   `host_read_index` records `commit` on arrival and waits for a read
   round and `applied ≥ read_index`; `host_wait_applied` has the same
   budget. Both give up after `HOST_READ_TRIES × HOST_TICK_US` = 400 ms
   with BUSY (`obs_wait_timeouts` counts it). The client
   (`rpc_send_recv_shard`) retries 16 times with `50 ms << min(attempt,4)`
   backoff, ~10.35 s total, then returns `EFS_ERR_BUSY`. So the observed
   line means group 2 could not satisfy a read for ten seconds straight:
   either `applied` trailed `commit` by more than 400 ms the whole time
   (36 ranks publishing; REPORT/publish applies on the pump; L1 pressure
   from this same IOR is recorded in §1b), or group 2 was re-electing
   (a hintless NOT_PRIMARY lands in the same BUSY/STALE bucket). A fresh
   client is the victim because its first op is the root GETATTR and it
   has no cached row; the IOR ranks mostly write and read their own
   dcache.

Steps, in this order:

1. **Fix the mapping first (client, small).** Carry the RPC rc out of
   `efs_client_stat_ino` / `efs_client_stat_refresh` (return it, do not
   fold to NOT_FOUND). In `ll_getattr` and the `lookup_fill` path map
   `EFS_ERR_BUSY` and `EFS_ERR_NOT_PRIMARY` to `-EBUSY` (rmdir/unlink
   already use EBUSY for `EFS_ERR_BUSY`), `EFS_ERR_NET`/`EFS_ERR_IO` to
   `-EIO`, and only a real `EFS_INODE_RPC_NOT_FOUND` to `-ENOENT`. Grep
   every caller of both functions (there are ~10 in `efs_fuse.c`) —
   some legitimately treat "cannot read parent" as ENOENT for a *child*
   lookup; those need the same split. Gate: posix 1 jobs=1 200/201,
   posix 2 63/63, and a forced 10 s BUSY (step 2's repro) makes `stat`
   return EBUSY, never ENOENT. If the user prefers EIO over EBUSY for
   stat, that is a one-line choice; ask before choosing EIO.
2. **Reproduce and attribute, then decide which server fix applies.**
   Script under `tests/measure/` (runbook in runbooks.md): start the 9×4
   IOR (`run.sh debug`), mount a 10th client on node9901 or fstor007
   (TCP or RDMA, whichever the cluster runs), and once a second run
   `stat /tmp/efs-mount/` recording errno + wall, while sampling
   `efs-mgmt raft-status` (`commit − applied`, `leader`, `term` per
   group) and grepping the leaders' `efsd.log` for
   `raft-host: read-sleep` / `apply-sleep us=` and `raft-obs`
   `obs_wait_timeouts`. Outcome A: `commit − applied` on group 2 stays
   above what 400 ms of apply covers for the whole IOR → the read is the
   messenger and the fix is apply throughput (W14 step 1 plus the cost of
   publish/REPORT apply under load; measure `apply_max` and per-cycle
   apply count first). Outcome B: term changes line up with the BUSY
   window → it is the W14 step 2 election trigger. Outcome C: lag is
   bursty (compaction/flush stalls of ≥ 400 ms) → it is L1/L0 pressure
   (§1b, W13's follow-on), not the read path. Record which; do not
   guess.
3. **Only after step 2:** if the lag is steady-state and apply throughput
   cannot close it, bring the trade-off to the user: readers currently
   wait for `applied ≥ commit-at-arrival`, which is the linearizable
   contract; a read that gives up after 400 ms and a client that gives
   up after 10 s are policy numbers the spec does not set. Do not change
   either number on your own.

**Step 2 outcome, first sample (Sep 28 22:11–22:19 EDT, no script yet).**
Seven parallel `dd bs=1M` from fstor007 into new files in one directory;
one `dd` got `open: Device or resource busy` (step 1's mapping is in
the binary, so this is the same failure that printed ENOENT before).
The fuse log for that mount holds **249** `exhausted 16 BUSY/STALE`
lines: LOOKUP 98, SETATTR 50, GETATTR 47, RENAME 31, CREATE 23. Every
`inode-rpc: retry … why=no-hint` line is a NOT_PRIMARY with no leader
hint. `raft-status`: group 0 term 9368 (was 9309 at 00:39Z), group 2
term 2789 (was 2763), both `commit == applied` when sampled;
fcstor004 `raft-obs`: `wait_timeouts=366`, `apply_max=30us`,
`apply-sleep` up to 57 ms on single entries, and the outbox
`drop=72114 > sent=50695` to the group 2 leader. **That is Outcome B:
elections, not apply lag.** The fix is W14 step 2 (a)–(c); W16 step 3's
policy question does not arise until W14 step 2 is done and this is
re-sampled with the script. Still write the script: it is the gate.

**Sep 29 02:35Z re-sample, and D8 (decided).** With W14 step 2 (a)–(b)
rolled, `drop=0` on every outbox and the terms still moved (+45 /
+105) — the trigger is the InstallSnapshot import (W22, D5). The
write-side wait now has a number: `report-split nrec=86234
pack_ms=1431 push_ms=9869 finish_ms=9227` — 337 publish batches at
~29 ms each through the one in-flight batch. **D8:** before any
read/wait policy or budget question, add `pub_batch_ms` (p50 / max
per group, the time from a batch's propose to its apply) to
`raft-obs`, run one 9-client `run.sh ior` after W22 steps 1–2 are
rolled, and bring the number: ~2–6 ms per batch means the 29 ms is
queueing behind the other eight clients' batches and the lever is
fewer, larger entries per REPORT (a wire question — ask); ~100 ms
means the `fsync` mode and D6 is the lever. Pipelining past one
in-flight batch stays forbidden. Order-table row 10.

- **Gate:** step 1's mapping gate; step 2's script committed with one
  attributed run in `results/measure/`; §1b updated with the outcome.
- **Forbidden:** raising `HOST_READ_TRIES`, `HOST_TICK_US`, or the
  16-attempt client budget to make the symptom go away; serving a
  GETATTR from the follower's or client's local state without the read
  round (that is the linearizability I2/I10 guarantee); touching
  `entry/attr_timeout`; clearing `FOPEN_DIRECT_IO`.

##### W17 — Nine writers on one file: publish STALE storm, fsync EIO, and a FUSE request that outlives its process

**Source (Sep 28 2026, 22:14–22:56Z, `bbcbcb5`).** ior-hard, 9 ranks,
one file (ino 1166063), 47008-byte records, `run.sh ior`. Evidence:

- fcstor004 `efsd.log`: a run of `raft-host: apply publish rc=-14
  index=… ino=1166063 ci=…` with distinct chunk indexes (512867,
  617329, 566269, 856797, …). `-14` is `EFS_ERR_STALE`: the CAS base
  the client reported is no longer the published generation because
  another rank published that chunk in between.
- Every client `fuse.log`: `inode-rpc: retry type=67 … why=recv rc=-6`
  then `inode-rpc: dual type=67 exhausted 16 BUSY/STALE retries
  (10.3 s) -> EBUSY`, several times per client. Type 67 is
  `EFS_MSG_REPORT_CHUNKS`. The BUSY is the server's
  `host_wait_applied` 400 ms budget inside `host_pub_batch_wait`
  (group 2 apply lagging under nine clients' publish batches), and the
  STALE is `host_pub_batch_wait` reporting any STALE verdict for the
  batch.
- fcstor013: `efs-fuse fsync: resource busy (efs_rc=-13) ino=1166063`
  → `efs_fuse_fsync_ino` returns `-EIO`. IOR: `WARNING: fsync(19)
  failed` ×8, `ERROR: close(20) failed`, rank 8 `MPI_ABORT`.
- After the abort, `io500` on fcstor007–010 and 012–014 sat in
  D-state `request_wait_answer` for 20+ min (SIGKILLed, still waiting
  on a FUSE reply). The reply they wait for is a `flush`/`release`/
  `fsync` whose `efs_client_report_dirty_ino(ino, sync=1)` is still in
  its loop: up to 64 STALE rounds, each a `stale_repull_replay` (GET +
  PUT of every moved chunk) plus a REPORT that can itself take the full
  16-retry 10.3 s, and up to 8 BUSY rounds on top. The loop is bounded
  in code and unbounded in practice.

This is the W1 N-1 CAS shape (36-way sub-chunk CAS on Sep 21 gave
hard-write 0.046 GiB/s) with nine whole ranks instead of sub-chunk
writers. The 4-rank shared-file IOR passed on Sep 27 (481.56 MiB/s,
`results/posix2/20260927-190509`); nine did not.

What to implement, in this order:

1. **A FUSE request returns.** `efs_client_report_dirty_ino` with
   `sync=1` must have a wall-clock bound that is shorter than what the
   kernel will wait, and the bound must cover the whole loop (STALE
   rounds × REPORT retries × BUSY retries), not one leg. When the bound
   is hit, `fsync` returns EIO with the dirty set merged back (the data
   is not lost; the next fsync retries). Measure first: add a counter
   line at loop exit (`report-loop ino= rounds= stale= busy= ms= rc=`)
   and run ior-hard NP=9 once to record the distribution. Then bring the
   number to the user — the 64-round and 16-retry budgets are policy
   the spec does not set. Gate: after `pkill -9 -x io500` mid-ior-hard,
   `pgrep -x io500` is empty within that bound on all nine clients
   and `efs-fuse` is still serving (`stat` OK).
2. **Attribute the STALE per chunk.** Log, on the server, how many
   publishes of one `(ino, ci)` lost the CAS within one fsync window
   (`raft-obs` counter `pub_stale` per group is enough), and on the
   client how many bytes each `stale_repull_replay` re-PUT versus how
   many bytes it owned. If a 47 KB record costs a 128 KiB GET + PUT +
   re-REPORT per losing rank, that ratio is the number to show the user.
   The `delta_off`/`delta_len` fields in `efs_chunk_rec` exist for a
   sub-chunk span publish; check whether the replay path fills them or
   always re-publishes the whole chunk, and record which.
3. **Decided (D1, Sep 28): make the span publish commute.** Three
   changes, in `efs_meta_apply_publish` and `dcache_flush_slot_inner`:
   - **Server:** for `delta_len > 0`, drop the `expected_gen != committed
     → STALE` check; a span attaches to whatever base is current. Keep
     the check for full-image publishes (`delta_len == 0`), which still
     require the live delta list to match exactly. When a fold replaces
     the base, write the folded spans' `candidate_gen`s into the new
     trailer (≤ `EFS_CHUNK_DELTA_MAX` entries); a span whose
     `candidate_gen` is in that list is a replay → OK, no-op. Overlap
     with a live span stays STALE (the client re-pulls that span's
     bytes only). A full chain still returns STALE to the publisher,
     who folds — that publisher, not the others.
   - **Client:** a slot whose dirty ranges cover less than the chunk
     publishes a span **even when `have_base` is set**
     (`span_of` regardless of `have_base`); the full-image path is for
     `dcache_full_overwrite` and for the fold. `stale_repull_replay`
     on an overlap STALE re-pulls and re-publishes the overlapping span,
     not the chunk.
   - **Reader:** `efs_meta_apply_get_chunk_deltas` and the client's
     base+spans overlay are unchanged; a reader that sees a full chain
     may fold as a background PUT, off the read.
   Gate: `test_meta_apply` gains "span after fold is OK",
   "replay of folded span is no-op", "overlapping span is STALE";
   `peer_shared_pwrite` and `concurrent_appends` pass; ior-hard NP=9
   and NP=36 SEGS=3000 cold verify bad 0 with `pub_stale` per fsync
   at 0 in steady state and the write rate rising from NP=4's 481.56
   MiB/s.

- **Gate:** step 1's bound + kill test; step 2's numbers in
  `results/measure/` with the script under `tests/measure/`; step 3's
  gate; ior-hard NP=4 SEGS=3000 cold verify still 12000 records bad 0;
  posix 1 jobs=1 200/201; posix 2 63/63.
- **Forbidden:** a distributed chunk lock; splitting REPORT into
  several RPCs; widening the 16-attempt RPC budget or `HOST_READ_TRIES`;
  making `fsync` return success on a merged-back (unpublished) dirty
  set; `hard_remove`; raising `EFS_CHUNK_DELTA_MAX` to avoid the fold;
  folding on a rank's `fsync` other than the one that filled the chain.

##### W18 — Client: the dcache reclaim is a table walk, and it is most of the client's CPU under a long write

**Source (Sep 28 2026, run 3, fcstor007 `efs-fuse`, 917K samples over
~40 min).** `by_thread.txt`: `dcache_flush_slot_inner` 43.8% **self**,
`pthread_once@GLIBC` 25.0% self, `pthread_mutex_lock` 8.3% +
`unlock` 3.5%, blake3 3.3%, `memmove` 2.4%. `callers.txt`:
`dcache_reclaim_main` is 73.5% of the process. The 5-minute profile
from run 2 (36K samples) had blake3 at 27% and this function's self
time under the floor; the long capture is the one that shows the
steady state.

Why it looks like this (`src/client/write.c`):

- `DCACHE_SLOTS` is 65536 hash buckets of chained `dcache_ent`;
  `DCACHE_RECLAIM_THREADS` is 16; each wake walks
  `DCACHE_RECLAIM_SCAN` = 64 slots via a shared cursor and, in
  `dcache_flush_slot_inner`, walks each slot's whole chain under the
  shard mutex skipping clean entries (`!e->dirty || !e->data`). With
  the staging table "over EFS_CLIENT_META_MB (256 MB) with nothing
  evictable; growing" (every client logged this), the chains are long
  and mostly clean. The self time is the chain walk.
- `dcache_mu()` and `dcache_io_mu()` call
  `pthread_once(&g_dcache_once, dcache_init)` on every call; the walk
  calls them per slot. 25% of the client is that check.
- 64 shard mutexes for 65536 slots, 16 walkers plus the FUSE write
  threads (`efs_dcache_try_patch`) on the same locks: the 12% in
  lock/unlock.

What to implement:

1. **Reclaim from a dirty list, not a table scan.** When an entry
   becomes dirty (`dcache_note_dirty_bytes(+len)` call sites), put it on
   a per-shard dirty list; reclaim pops from that list. A popped entry
   that is no longer dirty is skipped in O(1). The chain walk in
   `dcache_flush_slot_inner` stays only for `have_only=1` (close/fsync
   of one inode), which can also use a per-inode index if the profile
   still shows it. Gate: `dcache_flush_slot_inner` self under 5% and
   `dcache_reclaim_main` under 15% of a 30 s-stonewall easy-write
   profile; `dirty_bytes` accounting unchanged (posix 2 63/63;
   `concurrent_appends` and `peer_shared_pwrite` pass).
2. **Initialize the dcache once.** Call `dcache_init` from client
   startup (the export mount path) and drop the `pthread_once` from
   `dcache_mu` / `dcache_io_mu`. Gate: `pthread_once` gone from
   `flat.txt`.
3. **Re-measure W15 steps 1 and 3** on the same profile: `memmove` in
   `ll_write_buf` (7.5% in run 2) and in the RDMA send (6.3%). Those
   items stand; this one goes first because it is the larger share.

- **Gate:** the two profile gates; 9-client 8 GiB dd not worse than
  2551.5 MiB/s; 1-client dd not worse than 947 RDMA; posix 1 jobs=1
  200/201, posix 2 63/63.
- **Forbidden:** changing the 2 GiB reclaim limit or the 2× inline-help
  cap to hide the walk; dropping the `have_base` guard for reclaim
  (the `concurrent_appends` NULs); clearing `FOPEN_DIRECT_IO`.

##### W19 — Server: the Raft pump copies every AppendEntries twice, and `try_commit` re-walks the log per reply

**Source (Sep 28 2026, run 3, fcstor003 = group 0 leader, 1M
samples).** `flat.txt`: `memmove` 18.5%, kernel `rep_movs_alternative`
17.0%, `try_commit` 4.0% self, `__d_lookup_rcu` 2.6%. `callers.txt`:

| share | stack |
| --- | --- |
| 12.6% | `host_pump` → `efs_raft_recv`/`efs_raft_flush` → `send_ae` → `host_send` → `memmove` |
| 5.2% | same → `send_ae` → `memmove` (the arena fill) |
| 6.9% | `host_sender` → `efs_conn_send_msg` → `writev` → `tcp_sendmsg` → `copyin` |
| 10.5% | `host_pump` → `efs_raft_recv` → `try_commit` (4.0% self, `log_term` → `disk_get` → mutex) |

On fcstor004 the receiving side is `server_handle_conn` → `recv` →
`copyout` 15.4%. The peer connections are TCP for frames above the
72 KiB RDMA buffer (`conn_pick_send_chan`), which is every catch-up
AppendEntries batch.

What the code does (`src/raft/raft.c`, `src/server/raft_host.c`):

- `send_ae` mallocs an `EFS_RAFT_AE_BYTES` arena and `store->get`s each
  entry into it (copy 1, `disk_get`). `host_send` encodes the message
  into a stack buffer or a heap buffer (copy 2) and, when the stack
  buffer sufficed, `malloc`+`memcpy`s it again (copy 3) before queueing
  it for `host_sender`, which `writev`s it (kernel copy 4). All but the
  last run on the pump thread, under whatever the pump holds.
- `try_commit` runs on every AE reply and walks `n = last_i` down to
  `commit_index`, calling `log_term` (a `disk_get` under the store
  mutex) for each `n`, until it finds a quorum. When a follower is
  behind, `last_i − commit_index` is large and the walk repeats per
  reply.

What to implement:

1. **One user-space copy per AppendEntries.** Encode the wire frame
   directly into the buffer `host_sender` will write: `send_ae` asks
   `host_send` (or a new `net->alloc`) for a frame buffer sized from
   the entries' `clen`, fills header + entries in place, and hands
   ownership to the outbox. Drop the arena and the stack-then-heap
   re-copy. Gate: `memmove` under `host_send` + `send_ae` under 3% of
   the leader's profile during a 9-client write; `test_raft` OK.
2. **`try_commit` from the reply's match index.** The only index whose
   match changed is the replying peer's; a commit can only advance to
   an `n` in `(commit_index, match_index[from]]` whose term is current.
   Start the walk at `min(last_i, match_index[from])` and stop at the
   first quorum; cache the term of the highest entry so the common case
   is one `log_term`. Gate: `try_commit` self under 0.5%; `test_raft`
   commit/term tests OK; no change in `commit_index` sequence on a
   replayed unit log.
3. **Measure, do not change, the transport choice.** Record how many
   AE frames per second exceed 72 KiB and their size distribution
   (`raft-obs` line). `EFS_RAFT_AE_BYTES` and the RDMA buffer size are
   not to be tuned here; if the numbers say the peer path should ride
   RDMA with a different framing, that is a design question for the
   user.

- **Gate:** the two profile gates; both groups commit==applied and no
  term change during a 9-client 8 GiB dd; posix 1 9-host not worse
  than `results/posix/20260928-034049`.
- **Forbidden:** changing `EFS_RAFT_AE_BYTES`, `EFS_RAFT_AE_MAX`,
  `HOST_PUB_BATCH_N`, `HOST_TICK_US`, or the heartbeat/election
  timeouts; taking `h->mu` in `host_sender`; pipelining more than the
  one in-flight batch per behind peer.

##### W20 — Client: `stat` is O(chunks) — `st_blocks` walks every chunk of the file under two locks

**Status (Sep 29, in tree, not measured).** `efs_inode_mem` keeps
`present_chunks` (table inserts and removals) and `present_extra`
(a dirty dcache slot that is not in the table). `st_blocks` reads
that sum and probes only a partial tail. `ll_setattr` reads `size`
from the row.

**Source (Sep 28 2026, 20:36–20:39 EDT, fstor007 `client.sh --perf`,
214K samples, `~/orcd/scratch/efs/perf/efs-mount/callers.txt`).**
`ll_setattr` 8.9% of the process, all of it
`efs_fuse_getattr_ino` → `fill_stat_from_inode` → `inode_allocated_bytes`
(`src/client/efs_fuse.c`). Inside that: `efs_dcache_has` 3.7% self,
`pthread_mutex_lock`/`unlock` 2.5%, `__lll_lock_wait` 2.0% (the futex
sleeps under `fill_stat_from_inode` are 1.4% of the kernel time; the
flush path holds `idx_mu` and the dcache shard mutexes).

Why: `inode_allocated_bytes` computes `st_blocks` as "present chunks,
not holes" by looping `ci = 0 .. size/128 KiB` and, per chunk, taking
`efs_client_lock_dir` + `idx_mu` for `efs_export_get_chunk`, then the
dcache shard mutex for `efs_dcache_has`. A 16 GiB IOR file is 131072
chunks, so one `stat` is 262144 lock/unlock pairs and hash probes, and
`ll_setattr` calls `efs_fuse_getattr_ino` **twice** (before, for
`old_size`; after, for the reply). `ls -l`, `du`, `cp -p`/`ecopy`
(utimes + chmod after each file), IOR's `stat`, and every `getattr`
the kernel issues with `attr_timeout=0` pay this per file. It was ~0%
when added (project-history: small files) and is now a scaling bug by
the §1 rule: `stat` cost grows with file size.

What to implement:

1. **Make `st_blocks` O(1).** Keep a per-inode "present bytes" (or
   present-chunk count) in the staged inode row, maintained where chunks
   are inserted/removed in the client table (`efs_export_*chunk*` add /
   remove / truncate) and where the dcache admits a dirty chunk that is
   not yet in the table (`dcache_note_dirty_bytes` call sites already
   count bytes per entry). `inode_allocated_bytes` reads that counter.
   Holes stay holes: a chunk that is neither in the table nor dirty in
   the dcache is not counted, same as today.
2. **`ll_setattr` asks for `old_size` from the row, not a full
   `getattr`.** Only `FUSE_SET_ATTR_SIZE` needs the old size, and it
   needs `size`, not `st_blocks`.
3. **Gate:** a `stat` of a 16 GiB file on an idle mount under 1 ms
   (measure with `tests/measure/md_latency.py`'s stat row on a large
   file, add that row); `ll_setattr`/`fill_stat_from_inode` under 0.5%
   in the W15 re-profile; `st_blocks` unchanged for the posix suite
   (`size_and_mtime`, sparse and truncate tests) — posix 1 jobs=1
   200/201, posix 2 63/63.

- **Forbidden:** reporting `st_blocks` from `size` alone (sparse files
  and truncate stubs must still show holes); touching
  `entry/attr_timeout`; caching a stale `st_blocks` across a peer's
  REPORT (the counter is updated when the table adopts chunks, not
  time-based).

##### W21 — Client: the staging evictor recomputes the table size up to 1024 times per second and evicts nothing

**Status (Sep 29 16:09Z, steps 1 and 2 rolled with the servers,
not measured; clients are not mounted).** Step 1: a wake that evicts nothing while over the cap
sets a 1 s idle deadline that `stage_evict_kick` honours; an unpin
clears it. Step 2: `efs_export_staged_bytes` returns the root's
`staged_total`, a running sum of every table's `staged_est`;
`staged_refresh` re-derives one table's estimate wherever a capacity
or arena changes (slab and name arenas via `staged_slab_add`, the
slab directory, chunk array growth and shrink, the ino/name/chunk/icnt
index rebuilds, the child index and vectors, `compact_one_tab`, tab
attach and free). `test_stage_evict` checks the total across a
compaction. Step 3 stays behind a measurement. The earlier in-tree
state, for the record: one `efs_export_staged_bytes` per wake, a
wake walks at most four oldest bands, a fully pinned band advances a
cursor for the next wake instead of looping 1024 times.

**Re-review on the 22:48–22:55 profile (single `dd bs=1M`, 260K
samples).** `efs_export_staged_bytes` **7.4% self** and
`stage_evict_main` **5.4% self** (the inlined LRU scan in
`evict_pass`) — 12.8% of the client, more than before. The per-wake
cap works; the wake *rate* is the problem. `efs_client_stage_evict_kick`
is called from the staging paths on every write and is gated only on
`g_stage_bytes_seen > cap`. Once the table is over the cap with every
entry pinned (the "growing" state every writer reaches), that gate is
permanently true, so each 1 MiB write kicks the evictor, which wakes,
walks up to 4096 `shard_tabs` in `efs_export_staged_bytes`, scans the
LRU array up to four times, evicts nothing, goes back to
`pthread_cond_timedwait`, and is kicked again by the next write. The
1 s timer never gets to run because the kicks arrive faster.

Fixed implementation (replaces steps 1–2 above):

1. **A kick must not wake a scan that cannot evict.** Keep a
   `g_evict_idle_until` (monotonic) set to now + 1 s whenever a wake
   ends with `bytes > cap && evicted_total == 0`; `stage_evict_kick`
   returns without signalling while now < that deadline. Clear the
   deadline (set 0) where an entry becomes evictable — the unpin paths
   (`efs_client_stage_unpin`, last close, dcache clean-and-unpinned
   transitions) — so a real opportunity wakes the evictor at once.
   Gate: with one writer over cap, the evictor wakes at most once per
   second (count wakes in `EFS_STAGE_DBG` output).
2. **`efs_export_staged_bytes` in O(1).** Maintain one cross-tab
   running total in the root export, updated by `staged_slab_add` and
   the child/name-arena accounting where each tab's counters change
   (they already update per-tab totals); the shard-tab loop goes.
   `efs_fuse.c:5047` (the mount log line) reads the same total.
   Gate: `efs_export_staged_bytes` under 0.2% on any profile.
3. **The LRU scan is the remaining cost per real wake** (5.4% here
   because of the wake rate; with step 1 it is 1 scan/s). If it still
   shows above 0.5% after step 1, keep the LRU in tick order (a
   min-heap or a circular array indexed by tick) so "the 256 oldest
   above a cursor" is a slice, not a pass over `g_lru_mask + 1` slots.
   Do not do this before measuring after step 1.
4. **Gate:** as below, plus `stage_evict_main` self + children under
   0.5% on a 1-client `dd bs=1M` profile with the table over cap.

**Source (same profile as W20).** `efs_export_staged_bytes` 6.6%
**self**, caller `stage_evict_main` (`src/client/stage_evict.c`).
`stage_evict_main` itself 2.4%. W15's table said this was "replaced by a
running total Sep 28"; it was not. `export_staged_bytes_one` reads
running totals for one tab, but `efs_export_staged_bytes` still loops
`s = 1 .. shard_tab_cap` over `shard_tabs` (up to 4096) per call, and
`evict_pass` calls it once per pass.

Why the call count: when `bytes > cap` and the SCAN_BATCH (256) oldest
LRU entries are all pinned, `evict_pass` returns `evicted=0,
band_full=1`; `stage_evict_main` sets `min_tick = band_max` and loops,
up to 1024 rounds per wake, each round recomputing staged bytes and
scanning the **whole** LRU array (`for i in 0..g_lru_mask`) to find the
next 256 candidates. Every client logged "staging table over
EFS_CLIENT_META_MB (256 MB) with nothing evictable; growing" during the
IOR, so this is the steady state under a write flood: O(rounds × (tabs
+ LRU)) per second, no eviction, on a core the writers wanted.

What to implement:

1. **One `staged_bytes` per wake.** Compute it once in
   `stage_evict_main` before the round loop and adjust by what each
   round actually freed (`evict_pass` knows the bytes it dropped), or
   keep a true running total across tabs (`staged_slab_add` already
   exists per tab; add the cross-tab sum there). Gate:
   `efs_export_staged_bytes` under 0.5% in the W15 re-profile.
2. **Stop the pinned-band loop from rescanning the LRU.** Walk the LRU
   once per wake in tick order (or keep it ordered) so advancing past a
   pinned band is a cursor move, not a fresh O(N) scan. If the whole
   table is pinned, log the "growing" line once (already done) and
   sleep until the next kick or 1 s — do not spend 1024 rounds proving
   it. Gate: `stage_evict_main` + children under 1% while a client is
   over cap with all entries pinned (the IOR steady state).
3. **Gate:** W9's RSS result still holds (cold stat of 1M files levels
   near 233 MB, `results/measure/20260927-w9-walk`); posix 1 jobs=1
   200/201; posix 2 63/63.

- **Forbidden:** raising `EFS_CLIENT_META_MB` to hide it; evicting
  pinned (dirty or open) inodes; changing the 1 s wake period to
  something longer so the walk is merely rarer.

##### W23 — Client: five liveness syscalls per connection checkout, plus an `fstat` per send

**Source (Sep 29 2026 00:18–00:22 EDT, `client.sh --perf --strace` on
fstor007, `~/orcd/scratch/efs/perf/efs-mount/strace-summary.txt`,
230 s window, `ecopy --verify` + one `dd`).** Syscall counts:
`fstat` 1 171 561, `getsockopt` 1 172 007 (586 035 `SO_ERROR`,
585 972 `TCP_INFO`), `recvfrom` 598 781 (all `MSG_PEEK`), `poll`
1 121 932, `read` 1 195 618 (297K on the FUSE fd, the rest eventfds and
sockets), `write` 583 579 (`recv_poller` eventfd wakes). In the
untraced 22:11 profile the same path was `__fstat64` 1.2% +
`__getsockopt` 1.2% + `efs_client_conn_get` 0.6%; under ptrace it is
`efs_client_conn_get` 5% and `raft_voter_conn` 8%.

Why: `efs_client_conn_get` (`src/client/node_cache.c`) calls
`conn_fd_is_dead` on **every** checkout: `efs_conn_fd_matches` (an
`fstat`), `getsockopt(TCP_INFO)`, `poll(0)`, `recv(MSG_PEEK)`,
`getsockopt(SO_ERROR)`. Then `efs_conn_send_msg_parts`
(`src/common/protocol.c`) does `efs_conn_fd_matches` again before the
RDMA send. Six syscalls per checkout, three checkouts per chunk PUT,
one per metadata RPC — ~586K checkouts in this window, ~15K
syscalls/s. The probe exists for a real reason (the comment: a pooled
fd in CLOSE-WAIT after a peer FIN looked like a live checkout and every
PUT then failed; IPoIB CLOSE-WAIT sometimes reports no POLLIN/HUP), and
`fd_matches` exists because a recycled fd number once sent into a dead
QP. Neither reason needs a probe per checkout.

What to implement:

1. **Pool generation instead of `fstat`.** The pool is the only closer
   of pool fds. Give each slot a `gen` that `efs_client_conn_drop` /
   destroy bumps, store it in `struct efs_conn` at checkout, and make
   `efs_conn_fd_matches` compare integers. Keep the `fstat` form only
   for conns not owned by the pool (server side, tests) behind the
   existing `fd_id_ok` flag. Gate: `fstat` count in an
   `EFS_STRACE_EXPR='trace=fstat'` summary under 1% of the PUT count.
2. **Probe once per idle period.** Record `last_ok_ms` on a conn at
   every successful send/recv. In `efs_client_conn_get`, run
   `conn_fd_is_dead` only when `now - last_ok_ms > 1000` (the CLOSE-WAIT
   case is a conn that sat idle while the peer went away; a conn that
   completed an RPC a few milliseconds ago is not in CLOSE-WAIT). A
   checkout that skipped the probe and then fails in send/recv already
   drops the conn and marks the node (`efs_client_conn_drop`,
   `note_fail`); that path stays. Gate: `getsockopt` + `recvfrom(MSG_PEEK)`
   under 1% of the PUT count on the same summary; the Sep 20 CLOSE-WAIT
   repro (bounce one `efsd` while a client is idle for 30 s, then write)
   still recovers without an EIO.
3. **Gate:** posix 1 jobs=1 200/201, posix 2 63/63, 9-client dd not
   worse than 2551.5; `efs_client_conn_get` + `__fstat64` +
   `__getsockopt` together under 0.5% of an untraced dd profile.

- **Forbidden:** removing the dead-conn detection (the probe's reasons
  are real); probing on a timer thread (it would need the pool lock the
  hot path uses); changing `EFS_NODE_DOWN_FAILS` / `EFS_NODE_DOWN_MS`.

##### W24 — Client: `open(O_TRUNC)` of a large existing file did not return; SETATTR is the most-exhausted RPC

**Source (same window).** `dd.trace.txt`:
`openat(AT_FDCWD, "/tmp/efs-mount/001/dat08", O_WRONLY|O_CREAT|O_TRUNC, 0666) = ?`
followed by `+++ killed by SIGKILL +++` — the create-with-truncate of
the 1.7 GB file the previous `dd` had left never returned before the
user killed it. The fuse log's `exhausted 16 BUSY/STALE` count by type
went from 50 → **136** for SETATTR (type 63) between 22:19 and 00:27,
the largest of any type (RENAME 106, LOOKUP 98, GETATTR 47, CREATE
37). `clock_nanosleep`: 564 sleeps of 0.8 s in 230 s — the tail of
the BUSY backoff — plus `report-loop … busy=1 ms=8435..24182 rc=-13`
lines.

Why an `open` can outlive every budget in the tree:

- Kernel `open(O_TRUNC)` on an existing inode becomes FUSE `setattr(size)`
  → `efs_fuse_truncate_ino` (`src/client/efs_fuse.c`) →
  `efs_dcache_flush_ino` (flush every dirty chunk of the ino; each PUT
  waits on the writer pool), then `efs_client_truncate`
  (`src/client/ops.c`) → `efs_client_report_dirty_ino` (a REPORT loop
  — `report-loop … ms=24182` is one of those), then the SETATTR RPC,
  then `ll_setattr`'s getattr. Three to four RPC chains in sequence.
- Each chain is 16 attempts, but an attempt is bounded by the socket
  `EFS_IO_TIMEOUT_MS` = **30 s**, not by the 400 ms server read
  deadline: when the server holds the RPC (a REPORT was measured
  holding 20.5 s in W22's source; `host_truncate` in `raft_host.c`
  does a `host_read_index`, then proposes a LANE_FENCE per cross-group
  lane, then the truncate entry, each a commit wait), one chain can be
  minutes. The 10.3 s figure in the log line is the sum of the sleeps,
  not the wall.
- With W16 step 1 the final answer is EBUSY; the caller still waits
  through all of it first. The seven D-state `io500` processes in W17
  are the same shape seen from the other end.

What to implement:

1. **Measure one truncate, idle and loaded.** `truncate -s 0` of a
   2 GiB file on an idle cluster and under a 9-client write, with
   `strace -f -tt -T` on the fuse daemon filtered to that worker, and
   the `inode-rpc: slow-recv` / `report-loop` lines. Record per-RPC wall
   (`REPORT`, `SETATTR`, `GETATTR`) and the server-side
   `raft-host: setattr … rc=` pair. This says whether the time is the
   REPORT loop, the SETATTR's commit chain, or the retry sleeps.
   `results/measure/<stamp>-truncate-wall/`.
2. **Bound the whole operation, not each RPC.** `efs_fuse_truncate_ino`
   gets one deadline (the same 8 s W17 step 1 gave `fsync`, measured
   from entry); `efs_dcache_flush_ino`, `efs_client_report_dirty_ino`,
   and the SETATTR RPC each receive the remaining budget and return
   `EFS_ERR_BUSY` when it is gone. `fuse_stat_errno` maps that to EBUSY
   (already). Do not retry a SETATTR whose reply was lost: the server's
   op-id window (I16) answers the replay. Gate: under a 9-client write,
   `truncate -s 0` of a 2 GiB file returns (EBUSY allowed) within 10 s;
   idle it completes in under 1 s; posix 1 `truncate*` and
   `peer_o_trunc_visible` unchanged.
3. **`host_truncate` should not need a read round for the size path.**
   The `host_read_index` at the top is what pays the 400 ms BUSY under
   election churn before the proposal even starts; the proposal itself
   is the linearization point. Check whether the pre-read is only
   fetching `row` for lane splitting; if so, read the row from the
   local KV without a read round and let the truncate entry's apply
   re-validate (it already CASes the tail). This is a server change; if
   the pre-read is load-bearing for a fence ordering, stop and say so.

- **Gate:** step 1's measurement committed; step 2's bounds; posix 1
  jobs=1 200/201 (the truncate tests), posix 2 63/63,
  `posix_persist` shrink/remount cases once they exist.
- **Forbidden:** lowering `EFS_IO_TIMEOUT_MS` to make the chain shorter
  (a slow server reply is not a dead peer); returning success for a
  truncate whose SETATTR did not commit; touching the 16-attempt budget
  (W16 forbids it).

##### W25 — `futimens` fails with EINVAL on any file with a lane in the other Raft group; atime loses its nanoseconds; `ftruncate` says EAGAIN where `stat` says EBUSY

**Source (Sep 29 2026 00:1x EDT, `ecopy --verify /data1/erbmi1/knouse/
→ /tmp/efs-mount/knouse/` and the `software/` copy, operator's
terminal).** Three error strings, thousands of times:

- `<path>: futimens: Invalid argument` — only on large files (R and boost
  tarballs, cuDNN `.a`/`.so`, a MATLAB tar).
- `ecopy: verification metadata mismatch: <path> (atime)` — only on small
  files (conda-meta `.json`, headers).
- `ftruncate: Resource temporarily unavailable` — during the same window
  in which the fuse log's SETATTR `exhausted` count went 50 → 136.

**Confirmed from `ecopy.strace.txt` (00:35 EDT, `strace -f`, 585
threads, 255 931 lines, the `software/` copy, ended by Ctrl-C).**
Non-ENOENT failures: `utimensat` EINVAL **40** (516 succeeded),
`ftruncate` EAGAIN **19** (43 succeeded), `fallocate` EOPNOTSUPP 62,
`copy_file_range` EXDEV 423; `pwrite64` 29 905 and `pread64` 4 635 with
**no short or failed call**, no `close` errors, no `fsync` at all
(ecopy does not call it). Every EINVAL `utimensat` is on an fd that had
just had `fallocate` → `ftruncate` to tens or hundreds of MB (e.g. fd 74:
`ftruncate(74, 104082560)` then `utimensat(74, …) = EINVAL`); every
successful `utimensat` is on a file that went through the small-file
`copy_file_range` fallback. That is the lane split below, seen from
userspace. **Consequence:** after `futimens: Invalid argument` and after
`ftruncate: Resource temporarily unavailable`, ecopy `close`s and
**`unlinkat`s the `.ecopy.tmp.*` file** (56 unlinks in the trace): each
of those files was *not copied*. On this tree that is every file larger
than a few MB. The `ftruncate` that failed for `fd 47`
(923 946 888 bytes) was issued at line 6 123 and returned at line
137 107 of the trace — it blocked for most of the run before failing
(W24's chain; no `-tt` in this trace, so no wall figure). The 303
`verification metadata mismatch` lines are all `(atime)` and all on
files whose `utimensat` succeeded, i.e. item 2. `fallocate`
EOPNOTSUPP and `copy_file_range` EXDEV are the kernel's answers for a
FUSE mount without those ops and a cross-device copy; ecopy falls back
correctly and they cost one syscall each — not defects, noted so nobody
chases them. **`posix_fallocate` returning `EINVAL` is the same gap,
seen through glibc (Oct 1).** `efs_ll_ops` has no `fallocate`; libfuse
replies `ENOSYS` and the kernel turns that into `EOPNOTSUPP` for the
`fallocate` syscall. glibc's `posix_fallocate` does not return that: on
`EOPNOTSUPP` it writes zeros itself, and it does **not** emulate
`EINVAL`. On an `O_DIRECT` fd the fallback `pwrite` is not 4 KiB-aligned,
so it returns `EINVAL` (`fuse_odirect_unaligned` does the same for an
application `O_DIRECT` write). A normal fd's `posix_fallocate(0, 4096)`
succeeds only because of that zero-fill — `opt_fallocate` PASS is not
the filesystem reserving blocks. There is no preallocation. A tool that
treats the `EINVAL` as fatal writes nothing. **Implement the handler:
W26.**

Each is a code path, read from the tree, not from a profile:

1. **EINVAL on `futimens` = cross-group lanes.** `host_utimens`
   (`src/server/raft_host.c` ~9100): when the mask has MTIME it walks
   `row.active_lanes` and, for every lane whose shard is in the other
   Raft group, sets `rc = EFS_ERR_INVAL` with the comment
   `/* cross-group lane fence later */`. Lane shards straddle both
   groups (every odd lane of an inode is in the other group, project
   state), so any file that has written into two or more lanes — any
   file past the first few chunks — cannot have its mtime set.
   `efs_rc_to_errno` turns that into EINVAL. Small files (one lane, or
   all lanes in the row's group) work, which is exactly the split in the
   ecopy output. `host_truncate` already handles the same case (it
   proposes a LANE_FENCE per cross-group lane, then the entry);
   `host_utimens` was left as a stub.
2. **atime mismatch = seconds only.** `efs_fuse_utimens_ino`
   (`src/client/efs_fuse.c` 3228) keeps `tv[0].tv_sec` and drops
   `tv[0].tv_nsec`; `efs_client_set_atime` / `efs_client_utimens_both`
   carry `atime` as seconds; `host_utimens` stores
   `u.atime = atime * 1000000000`. mtime keeps its nanoseconds
   (`msec`, `nsec`), atime does not. ecopy compares full timespecs, so
   every file whose `futimens` *succeeded* then fails the atime check.
   Reads do not touch atime anywhere in the tree, so this is the whole
   cause.
3. **`ftruncate` EAGAIN = the SETATTR BUSY budget (W24), mapped
   differently.** `efs_fuse_truncate_ino` returns
   `efs_rc_to_errno(EFS_ERR_BUSY)` = **EAGAIN**; W16 step 1's
   `fuse_stat_errno` maps the same BUSY to **EBUSY** for
   getattr/lookup/create. Two errnos for one condition; ecopy treats
   EAGAIN as a failure and moves on. The cause is W24/W22; the mapping
   is this item.

What to implement:

1. **`host_utimens` fences cross-group lanes the way `host_truncate`
   does** (propose the LANE_FENCE entries on the other group first, then
   the UTIMENS entry; the same helper). If the fence is unnecessary for
   a pure mtime set — the fence exists so an in-flight REPORT on another
   lane cannot re-stamp "now" over the set time — then the UTIMENS entry
   needs the `mtime_gen` bump the apply already checks, per lane group;
   decide by reading `efs_meta_apply` UTIMENS/REPORT ordering, and say
   which. Gate: `futimens` on a 2 GiB file returns 0 and `stat` shows
   the set mtime; posix 1 `utimens*` and posix 2 `peer_utimens_visible`
   unchanged; `cp -p` / `ecopy` of a 1 GiB file reports no `futimens`
   error.
2. **Carry `atime_nsec` end to end**: `efs_fuse_utimens_ino` →
   `efs_client_set_atime` / `_utimens_both` → `efs_msg_inode_setattr`
   (add the field; wire change, bump nothing else) → `host_utimens`
   `u.atime = atime*1e9 + atime_nsec` → `efs_meta_setattr` → row. Gate:
   `touch -a -d '@1500000000.123456789'` then `stat` shows
   `.123456789`; ecopy's atime check passes on the small-file tree.
3. **One mapping for BUSY.** `efs_rc_to_errno` returns `-EBUSY` for
   `EFS_ERR_BUSY` (keep EAGAIN for `EFS_ERR_AGAIN` only), so truncate,
   utimens, chmod, and the stat path agree. Gate: posix 1 jobs=1
   200/201; grep the suite for tests that assert EAGAIN on BUSY (there
   should be none; if one exists, that test is wrong about the
   contract, not this change).

- **Gate:** the three above plus posix 1 9-host not worse than the
  last run.
- **Forbidden:** setting mtime without fencing or bumping `mtime_gen`
  (a later REPORT would restore "now" — the exact bug
  `setattr_rpc_dual_apply`'s comment describes); rounding atime on the
  server to hide step 2; mapping BUSY to EIO.

##### W26 — FUSE `fallocate` handler — ASKED Oct 1 2026

`posix_fallocate` on an `O_DIRECT` file returns `EINVAL` because
`efs_ll_ops` has no `fallocate` (W25). The user asked for the handler.
`/home/erbmi1/git/direct_rw/ewrite.c` opens `O_DIRECT`, calls
`posix_fallocate(fd, 0, size)`, and falls back to `ftruncate` only for
`EOPNOTSUPP`/`ENOSYS`. `EINVAL` aborts the job with 0 bytes written.

**What.** Add `ll_fallocate`. Mode 0 (what `posix_fallocate` sends)
extends the file to `offset + length` the way `ftruncate` already does:
one size SETATTR, no fragments. A read of the new range is a hole and
returns zeros (a missing chunk already does). `FALLOC_FL_KEEP_SIZE`
leaves the size unchanged and succeeds when the range is inside the
file. Any other mode (`PUNCH_HOLE`, `ZERO_RANGE`, `COLLAPSE_RANGE`,
`INSERT_RANGE`, `UNSHARE_RANGE`) returns `EOPNOTSUPP`, so a caller can
fall back. This does not reserve NVMe blocks; a later write can still
fail if a disk or the quota is full.

- **Gate:** `fallocate(fd, 0, 0, size)` on an `O_DIRECT` fd returns 0;
  `stat` size is `size`; a read of the new range is zeros; no chunk
  object was stored for that range. `ewrite` from `direct_rw` gets past
  `prepare_file_space` and writes. posix jobs=1 stays **200/201**
  (`opt_fallocate` still PASS; it must no longer depend on glibc
  writing the 4 KiB). posix2 63/63.
- **Forbidden:** writing zero fragments to satisfy the call; a quota
  reservation; implementing punch-hole or zero-range in this item;
  treating glibc's `posix_fallocate` success on a normal fd as proof
  (that success is the zero-fill fallback until this handler exists).

##### W12 — Repo hygiene

`results/` holds only runs that a live document cites (this page, the
rules, `docs/`, `tests/`; `project-history.md` and `design-history.md` are
archives and do not count). Commit a run directory when it is cited as a
gate; otherwise delete it. Pruned to that rule on Oct 1 2026; `git log --
results/` keeps the rest.

Run `python3 docs/gen-architecture-full.py` (or `make docs-check`) after any doc edit: it regenerates
`architecture-full.md` **and** `architecture.html` from the markdown sources
and validates links plus every `I1..I25` reference. Never edit either
generated file. `make test` must be fully green — there is no
accepted-failure list.

---

**Idea checked Oct 1 2026 — synchronous encryption. Not a decision. Do not implement.**

The user asked how transport and at-rest encryption could work, and whether a key created with the cluster would let only the Linux user who mounts with that key see the contents. Conclusions, for a later design pass:

- Nothing encrypts today. `EFS_MSG_HELLO` checks the build id. Any host that can reach 19810 is a peer. Modes are enforced in the FUSE process; the client tells the server its uid (`docs/product-gaps.md`).
- One key, created at `raft-mkfs` and shown once. The cluster stores a verifier, not the key. A mount proves it knows the key. The key file is mode `0600` and the mount is not `allow_other`, so other uids on that machine see nothing at the mount point. Root on that machine can still read the key (ptrace is open here). Any other host that has the key can mount. Every holder of the key sees every byte it encrypts. Per-user secrecy is a different key per user.
- Fragments, on the client, after parity. Split the chunk, XOR the parity, then encrypt each of the three fragments with its own IV stored beside the fragment. `PUT` and the disk see only ciphertext. A read decrypts, then XORs if it must rebuild one fragment. XOR of ciphertext does not reconstruct, so encryption sits outside the parity. The object name stays a hash of the plaintext, so two clients still agree on one object; the IV is not part of that name. The server stores opaque fragments and does not need the key to hold them.
- Metadata is the open choice. Names and directory updates are applied by the servers. Without the key they cannot apply encrypted names, so either the namespace stays plaintext on disk (a pulled NVMe reveals names, not file bytes) or `efsd` is unlocked with the key at start and the KV and Raft log are encrypted on the way to disk. The key must not live in the Raft log, or the disk contains the key that decrypts the disk. A server that was not unlocked cannot serve.
- Transport is the same idea on the frame: encrypt before send, decrypt before the handler, on the RDMA path and on the TCP side channel large frames already use. A traffic key derived from the cluster key at connect. Ciphertext fragments on the wire still show sizes and which object was touched; the frame hides names and the access pattern.
- "Synchronous" means the `pwrite` and the wire only ever see ciphertext. There is no later encryption pass.

**Decisions only the user can make — bring the evidence, do not start.**
These came out of reviewing efs as an HPC parallel filesystem. Each one
changes what efs *is*, so an agent must not pick a side; but each has a
cheap measurement an agent can produce first, named here:

- **Per-file layout control.** Chunk size and the EC profile are per export;
  there is no per-file or per-directory equivalent of Lustre's `lfs
  setstripe`. A 4 KiB-record checkpoint and a 1 GiB-per-rank dump therefore
  get the same 128 KiB geometry, and the declared 32× small-write
  amplification ([architecture.md §9](#architecture)) has no opt-out.
  Evidence to bring: W6's IOR-hard/IOR-easy ratio and rw-4k from W4.
- **An interface beyond FUSE.** FUSE is the only client. libfuse 3.10.2 cannot
  negotiate `FUSE_MAX_PAGES`, so every request is ≤128 KiB regardless of
  `max_write`; it cannot emit `FOPEN_PARALLEL_DIRECT_WRITES`; and Linux takes
  the inode lock exclusively for extending direct writes and the parent
  directory lock for `O_CREAT`, **per mount** (already in
  [architecture.md §9](#architecture) as an open kernel-interface item).
  So 64 ranks on one node writing one file serialize in the kernel before efs
  is called. Every production PFS has a kernel client, a user-space library,
  or an MPI-IO ADIO driver. Evidence to bring: W4's per-client scaling and a
  1-node 8-rank IOR-easy vs 8-node 1-rank IOR-easy comparison from W6.
- **Whether `write()` is durable** — W2 closed: spec says `fsync`/`close`/
  `O_SYNC`. Do not reopen as publish-on-write. `O_SYNC` wiring is a later
  item, not a silent side-cut.
- Already listed before this review: C1 relaxed coherence; a pressure-triggered
  directory-spread bound (unspecified); cutover of a 36T `efs-test`; any new
  REPORT or SNAP wire shape.

**Bigger than this queue.** [product-gaps.md](../product-gaps.md) inventories
what is missing before efs is a filesystem anyone could run — including the
things that contradict a guarantee the spec already makes (no fragment
repair, no protection-debt tracking, no session/fencing on the client, and
W1/W2 above). Those are not queue items beyond what W1–W2 say; each needs a
design decision first. Do not start one without asking, and do not treat the
queue above as the whole distance to a product.

##### W22 — Server: the snapshot cadence makes InstallSnapshot the steady state, and a follower inside an import campaigns

**Status (Sep 29 2026, 04:27Z).** Steps 1 and 2 are in tree and rolled
(04:08Z, `54a500da9dc8-dirty`): a group snapshots at 512 MiB of log
commands past `snap_idx` (`EFS_RAFT_SNAP_BYTES`), `raft_group_snap`
keeps that window (`log_base`, recorded in the SNAP record so a rotated
log replays it), `send_ae` InstallSnapshots only below
`efs_raft_log_floor`; the import diff is applied 1024 keys per pump
cycle (`HOST_SNAP_SLICE`, state `SNAP_IMP_APPLY`) and the last-chunk
retry stays BUSY until the cursor finishes. First 20 minutes on the
cluster, with a 7-minute write in them: zero `raft-snap: start` on any
node, no import. The byte counter's first version walked the log on
every tick (1–1.25 % of every server); the O(1) version is in tree, not
rolled. Step 3's flag `--meta-storage` is in tree with the default
layout; its measurement is **closed by D11** — the per-thread `fsync`
histograms from that trace put the Raft log at 0.4 ms and the 100 ms
mode on the compactor's own segments. What the trace found instead is
W23. Order-table rows 8a, 8b done; 8d closed.

**Source (Sep 29 2026 02:35–02:41Z, `results/io500/20260929-023447-iorperf2/ana`,
9-client `run.sh ior`, perf + `strace -f -tt -T` on every daemon).**

- `host_maybe_snapshot` (`raft_host.c`, `HOST_SNAP_MIN` = 256) starts an
  export as soon as 256 entries have applied since the last one and the
  previous export has finished. The export is the whole KV: 2.67 GB,
  4.6–12 s each, so every node exports back to back for the whole run
  (`raft-snap: start/end` pairs 256 entries apart; fcstor005 twelve of
  them in six minutes). `efs_kv_lsm_view_export` `memcmp` +
  `vx_sift_down` is the top user symbol on all four servers (14 % of
  fcstor004's samples on the GC thread).
- `send_ae` (`raft.c:672`) sends InstallSnapshot to a peer whose
  `next_index ≤ snap_idx`; with the snap point moving every 256
  entries, a follower 257 entries behind gets the 2.67 GB file instead
  of 257 log entries. fcstor006 imported group 2 twice (`import diff
  n=685534 ms=12132`, `n=130398 ms=16467`), fcstor003 imported group 0
  once (`n=380716 ms=12041`).
- The import diff is applied on the follower's pump on the ack (W14
  step 1 moved the scan and sort off the pump; the apply stayed). For
  those 12–16 s the follower answers no heartbeat. Group 0 term moved
  9370 → 9415 and group 2 2792 → 2897 across the run with `drop=0` on
  every `raft-obs tx->` line on all four nodes — the outbox coalesce
  (W14.2 (a)–(b)) removed the drops and the terms still moved. The
  group-2 leader answered 15 REPORTs NOT_PRIMARY and fcstor005 answered
  32 as a transient leader.
- fcstor004's busiest thread (22 % of samples) is the compactor:
  sequential 8050-byte `pread` of one segment, 4 KiB `write`s of the
  new one, and a segment `fsync` at a flat 100 ms, 97 times.
  `persist_max` reached 254 ms. The twenty writer-pool threads create
  fragments on the same XFS (`/data1/01`, ~14 s of `openat`/`write`/
  `close` each). The Raft log, the KV WAL, the segments, and the data
  share one journal.
- Net effect on a client: one REPORT of 86 234 records held the RPC for
  20.5 s (`pack_ms=1431 push_ms=9869 finish_ms=9227 rc=-13`), which is
  the client's `recvfrom` of 19.36 s and the `rounds=1 rc=-13` fsync EIO
  that aborted IOR. The pump itself was not held: `pump_hold_max` 59 ms,
  `apply_max` 69 µs on the leader in this run.

**Steps, each its own change with its own gate (D4–D6 decided):**

1. **Snapshot by log bytes; keep a log window (D4).** In
   `host_maybe_snapshot`: replace `applied ≥ snap + HOST_SNAP_MIN` with
   "bytes appended to this group's log since `snap_idx` ≥ 512 MiB"
   (the store knows its size; count entry `clen` at append if it does
   not). In the store's `save_snap`: keep the log entries after the
   snapshot point instead of truncating at it — retain a window of the
   last `W` entries (start with the same 512 MiB worth; the window is
   internal, no knob) and drop only what falls out of it. In `send_ae`
   (`raft.c:672`): send InstallSnapshot only when `next_index` is below
   the retained window's first index, not when it is `≤ snap_idx`.
   `snap_ensure` / `send_snap` keep re-exporting on demand for a peer
   that is genuinely below the window (fresh node, wiped `mdraft/`).
   Gate: over a 9-client `run.sh ior`, `raft-snap: start` per group at
   most once per 512 MiB of log; `import start` count 0 on every node
   unless one was restarted; `efs_kv_lsm_view_export` under 2 % of
   every server's samples; `test_raft` OK; fcstor005 bounce-and-rejoin
   still catches up (from the log now, not a file).
2. **Slice the import-diff apply (D5).** Today the pump applies the whole
   diff batch on the ack. Make the import state machine resumable: the
   pump applies at most one `HOST_TICK_US` of diff keys per cycle
   (measure keys/µs once; a fixed count per cycle is fine), releases,
   drains the inbox and answers AppendEntries as a follower, and
   continues on the next cycle; the SNAP_REP that acks the last chunk
   goes out when the final slice is in (until then the retry of that
   chunk keeps getting BUSY, as it does now). Gate: force an
   InstallSnapshot under load (bounce one voter after wiping its
   `mdraft/`, with a 9-client write running); `apply_max` under 70 ms
   throughout, no term change on the importing group during the
   import, `tests/measure/i17_leader_freeze.sh` still passes; the
   follower's `commit == applied` catches up to the leader after the
   import.
3. **Metadata root off the fragment root (D6).** Measure first, on one
   server, with the cluster serving a 9-client write: `dd if=/dev/urandom
   of=<root>/fsync-probe bs=4k count=1 conv=fsync` 200 times on
   `/data1/01` (shared) and on a root with no writers (stop one path
   from `--storage` on a scratch efsd, or use a directory on `/` for the
   comparison only), p50/p99 of each into
   `results/measure/<stamp>-fsync-root/`. If the shared root shows the
   100 ms mode and the quiet one does not: add `--meta-storage <root>`
   to `efsd` (default the first `--storage` root — today's layout, no
   migration of existing `mdraft/`), then redeploy 19810 with `mdraft/`
   on a root none of the six `--storage` paths use (ask which device;
   this is a cluster layout change). Gate: compactor and pump `fsync`
   p99 under 10 ms in an `EFS_STRACE_EXPR='trace=fsync,fdatasync'`
   trace of a 9-client write; `persist_max` under 20 ms. If the quiet
   root shows the same 100 ms mode, this step is closed as "not the
   sharing" and the number goes to the user.

- **Gate:** the three above; posix 1 jobs=1 200/201 and 9-host, posix 2
  63/63; a 9-client `run.sh ior` that completes ior-easy-write with zero
  `report-loop … rc=-13` lines; 9-client 8 GiB dd not worse than 2551.5.
- **Forbidden:** raising the election timeout or `HOST_TICK_US`; widening
  the 400 ms apply budget or the 16-attempt RPC budget to hide the wait;
  putting the import scan/sort back on the pump (W14 step 1); changing
  `EFS_RAFT_SNAP_CHUNK`; a global or thread-local fd cache (W14 step 4).

**Two measurement notes from this run.** Both perf profiles carry the
concurrent strace (`ptrace_do_notify` 4–5 % on every host; ~40 % of the
client's `children` view is ptrace stops), so shares are inflated toward
syscall-heavy threads — profile with one recorder at a time when a share
matters. And an un-narrowed server `--strace` is ~2 GB per 6 minutes;
use `EFS_STRACE_EXPR`.

##### W23 — Server: the apply path blocks on L0 back-pressure, and compaction rewrites the table to absorb a few MiB

**Status (Sep 29 2026, 05:11Z).** In tree and rolled
(`a53b253f2455-dirty`, RDMA, `--perf --strace`). Step 1: the apply path
does not wait in `kv_maybe_flush_locked`; a publish batch returns BUSY
from `host_pub_batch_propose` while L0 is within one flush of the cap.
Step 2: a range is compacted when its L0 bytes are at least 1/8 of its
L1 bytes (or when L0 is at the file cap, whichever comes first);
`key[0]==0` is 16 subranges (`key[1]`'s top nibble) at flush and at
compaction output; compaction iterators read 1 MiB ahead. Not gated.
The 1 GiB L0 byte cap in place of the 64-file array is not in this
change — the file cap still forces a compaction, but of one subrange.

**Source (Sep 29 2026 04:07–04:27Z, `results/measure/20260929-040800-idle-trace/ana`,
perf + `strace -f -tt -T` on every daemon, a user `ecopy` 04:15–04:22Z
writing 408K fragments per server; §1b has the full list).**

- `kv_maybe_flush_locked` (`kv_compact.c:711`) waits on `l->cv` while
  `n_l0 + KV_LSM_RANGE_MAX > KV_LSM_MAX_SEGS` (i.e. L0 ≥ 48 files). The
  caller is the apply path on the pump under `h->mu`. On fcstor004 the
  pump's `futex` waits were 4.14, 1.68, **24.23**, 2.70, 3.57 s, each the
  length of one `kv-compact: end ms=`; `pump_hold_max` 4.4 s (history
  6.4 s), `apply_max` 4.4 s. fcstor005 2.14 s. W13 step 2 documented
  this wait as "the only stall left" and logged it (`kv-compact:
  backpressure n_l0=`); the log shows it continuously through the write.
- While the pump is held: `host_wait_applied` hits its 400 ms deadline
  (`raft-host: apply-sleep us=400xxx`, dozens per hold) → the REPORT
  answers BUSY (`report-split nrec=76757 … finish_ms=1881 rc=-13`; 12 of
  116 on 004) → the client's `fsync` returns EIO (W17.1) — the same
  abort the last two IORs died of. No heartbeat leaves either: group 0
  9418→9421 (04:15:43Z), group 2 2902→2904 (04:16:17Z), inside the
  holds. With W22.1–2 rolled and zero snapshot activity, this is the
  multi-second pump hold that is left.
- Why L0 is always near the cap: a flush writes one L0 file per
  `key[0]` range (W13 step 5, ≤ 16 files of ~256 KB from a 4 MiB
  memtable); compaction takes one range's L0 files (`inputs=` 5–11) and
  rewrites that range's whole L1 — 190–330 MB for ranges 1–15, **1.8 GB
  for range 0**. 433 compactions in 20 minutes on 004 (441 / 648 / 656
  on 005 / 003 / 006) summed to **159 GB** of `bytes=` against a 5.3 GB
  table; range 0's 34 rewrites are 61 GB of that and the 24 s hold. The
  compactor thread is 48 % / 55 % of fcstor004 / fcstor005's samples
  (`cm_sift_up`, `memcmp`, `cm_pop`, `kv_seg_probe`), with 5.25M 4 KiB
  `write`s (stdio default buffer) and 3.7M ~8 KB `pread`s (one per
  input block).
- The compactor's segment `fsync` is the 100 ms mode (299 of 695 on 004
  in 90–120 ms, avg 51 ms; 407 of 733 on 005); the pump's Raft-log
  `fsync` averages 0.38–0.40 ms. That closes D6 (D11): not the sharing.

**Steps (D9–D10 pending; write them as decided once the user answers):**

1. **The pump never waits for the compactor (D9).** In
   `kv_maybe_flush_locked`, when the caller is the apply path (pass a
   flag, or make the apply's flush a separate entry point), do not wait:
   flush what can be placed, or let the memtable grow past
   `memtable_max` and return OK; the memtable is bounded by what the log
   can commit ahead of the KV (512 MiB since W22.1). Back-pressure moves
   to admission: `host_propose` (or the REPORT handler before it)
   returns BUSY when the local L0 bytes are over the cap, so the client
   retries on its existing 16-attempt / 10 s budget and no follower's
   pump ever stalls. `kv_compact_locked` for tests and `--compact` keeps
   the synchronous path. Gate: a 9-client `run.sh ior` with `pump_hold_max`
   under 100 ms on every node throughout (`raft-obs`), zero
   `apply-sleep` lines above 100 ms, zero `report-split … rc=-13`, no
   term change on either group; `make test`.
2. **Compaction by bytes, and a bounded range (D10).** (a) A range is
   compacted when its pending L0 bytes reach 1/8 of its L1 bytes
   (`kv-compact: start` logs both); the L0 cap becomes a byte budget
   (1 GiB) and the 64-file cap goes — the read path already probes every
   L0 file, so the file count is not the constraint. (b) A range whose
   L1 exceeds ~256 MB is split on the next key byte at flush and at
   compaction output, so no single rewrite exceeds that. (c) The merge
   reads each input segment through a 1 MiB sequential buffer instead of
   one `pread` per block. Gate: over the same IOR, `kv-compact: end`
   `bytes=` sum under 10× the KV's growth for the run; no compaction over
   1 s; compactor under 15 % of any server's samples; `test_kv_lsm` and
   `test_partitioned_flush` pass; a KV copy from a live node reopens and
   verifies every key.
3. **Already in tree (8g), roll with the next build:** `disk_log_new_bytes`
   O(1); `kv_seg_w_open` `setvbuf` 1 MiB.

- **Read:** `src/kv/kv_compact.c` (`kv_maybe_flush_locked`, `compactor_main`,
  `kv_compact_locked`), `src/kv/kv_lsm.c` (`kv_flush_locked`), `src/kv/kv_seg.c`,
  W13 above, the "L1 compaction is a background thread" learning.
- **Forbidden:** raising `KV_LSM_MEM_DEFAULT` or `KV_LSM_L0_DEFAULT` as the
  fix (W13); raising the election timeout, `HOST_TICK_US`, or the 400 ms
  apply budget; any compaction step under `h->mu`; making `fsync` succeed
  on a merged-back dirty set; moving `mdraft/` to another device to hide
  the compactor's I/O (D11 says it is not the sharing).

Everything the §10 steps delivered (10.5c-1..35d, step 11's deletion of the old
engine, step 12 parts A–D) is landed and gated; the per-increment narrative is
in the commit history and in `.cursor/rules/efs-project-state.mdc`, not here.

**When the queue above is empty,** the next task comes from a measurement, not
from this page: run the gates in [testing.md](../testing.md), and take the
largest gap between what a gate reports and what the ceiling table at the top
of §1a says the hardware allows. If closing it needs a design decision the
spec does not contain, stop and ask (§4).

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
| Production Raft host | `src/server/raft_host.c`, [architecture.md §10](#architecture) 10.5 | I1–I4, I16; never `efs_raft_snapshot()` until KV flush-through-applied; SNAP blob is the existing WAL item payload | `tests/test_kv_lsm`, `tests/test_wire`, `tests/stress/raft_host_smoke.sh` (scratch cluster; not live `efs-test`) |
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
2. **Unit tests pass:** `make test` (`test_sim`, `test_meta_apply`,
   `test_raft`, `test_kv_lsm`, `test_stage_evict`, … — no accepted-failure
   list).
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

A returned `write()` is **not** durable or cross-client visible. Bytes
land in the client dcache; same-client read-your-writes hold. Publication
(the machine above) runs at `fsync`, last `close`, or `O_SYNC`/`O_DSYNC`/
`-o sync`. That is POSIX and every production PFS. The stronger
"`write()` publishes" alternative was measured (START-HERE W2) and
rejected: a peer sees none of an un-`fsync`ed 4 KiB write, and `kill -9`
of `efs-fuse` loses a 64 MiB acknowledged `write()`. `O_SYNC` is the
specified write-through path; it is not wired yet. `fsync()` covers the
client-side dirty set plus the publication quorum.

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
  128 KiB chunk still stores a full chunk image (k+f fragments) — roughly
  **80× data-path amplification** for the logical 4 KiB. Disjoint writes
  do not share one generation CAS. A partial publish appends an
  **immutable span**: the object is a full chunk image, readers copy only
  `[off, len)`, and the base generation does not change, so concurrent
  disjoint spans do not STALE each other. Overlap, or a trailer already
  holding `EFS_CHUNK_DELTA_MAX` (8) spans, returns STALE. The client
  refetches and publishes one image that folds exactly the spans it
  read; `delta_base_n` and `delta_base_seq` must match, or that CAS
  STALEs. A fold that names a longer list than the image contains
  deletes a peer span. That client fold is the consolidation. There is
  no separate background span compactor. **efs stays optimized for
  HPC-sized, aligned I/O**; the span path removes the invented chunk
  conflict, and the bytes on disk stay a full chunk per span.
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
  operation" would be a lie: publication (`fsync`/`close`/`O_SYNC`)
  crosses a persistence boundary — an ordinary completed write on PLP
  media, or an explicit FUA/flush without it
  ([data protocol](protocols/data.md)). A plain `write()` does not.
  What batching removes is paying that boundary once per chunk when one
  boundary could have covered thousands; it cannot remove the boundary
  itself.


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

**Where we are (Oct 1 2026, honest).** The boundaries exist (Phase M is
complete) and the state machines are pure enough that `tests/test_sim`
runs them. The file-size rule is not met: of a 70k-line `src/`,
`server/raft_host.c` is 12.1k lines, `client/write.c` 6.0k,
`meta/meta_apply.c` 5.7k, `client/efs_fuse.c` 5.5k, `meta/metadata.c`
3.8k. Splitting those by responsibility is a standing follow-on, taken
when a change touches them, never as a drive-by.

**Boundaries** (aligned with the planes, so the architecture and the
code structure are the same map):

```text
raft/       the consensus core — a pure state machine, transport- and
            storage-agnostic; no I/O inline, no globals (`raft.c`,
            `raft_disk.c` on-disk log, `raft_mem.c` for the simulator).
kv/         the ordered applied state — `kv_mem.c` for the simulator,
            `kv_lsm.c` + `kv_wal.c` + `kv_seg.c` + `kv_compact.c` +
            `kv_snap.c` on disk, behind `include/efs/kv.h`.
meta/       the POSIX op state machine over kv/ and raft/ (`meta_apply.c`,
            `txn.c`, `session.c`, `lock.c`, `dir_*.c`); no socket or FUSE
            calls inline. `metadata.c` is the client-side staging table.
wire/       the protocol — versioned encode/decode, nothing else.
data/       the data plane — EC encode/decode, store + transport vtables
            (`efs/store.h`, `efs/transport.h`), loop/conn transports.
server/     `efsd`: handlers, the production Raft host (`raft_host.c`),
            the NVMe fragment store (`store.c`), writer pool, peer pool.
client/     `efs-fuse`: `efs_fuse.c` is the FUSE adapter; `ops.c`,
            `read.c`, `write.c`, `inode_rpc.c`, `stage_evict.c` hold the
            path/RPC/data logic.
sim/        the deterministic simulator the state machines run under.
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

**The flag is gone (Step 11, Sep 11 2026).** Every "When `EFS_MD_RAFT=1`"
and "Flag off is a no-op" below records how that slice was gated while
the old in-memory table still existed. The Raft+KV host now starts
unconditionally and is the only metadata engine; the table those slices
bypassed is deleted. The 19820 scratch cluster is retired;
`tests/stress/raft_host_smoke.sh` starts and stops its own private
cluster.

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
itself**. Before the simulator (Step 10.5) that proof was a 13-node wipe +
rsync + rebuild + ssh orchestration, and the signal was poor: most
posixstress "failures" were 15 s timeouts (saturation, not correctness),
and a single run was noise. The strategy is to **push each class of bug
to the cheapest layer that can catch it**; `tests/test_sim` is that
deterministic layer and is the correctness gate for every protocol change.

The layers, cheapest first:

| Layer | Answers | Cost | Catches |
|---|---|---|---|
| **unit** (`make test`) | is this function right | ms | logic, encode/decode, pack/unpack |
| **simulator** | is the *protocol* right under any interleaving / failure | ms | message-ordering, leader/fencing, recovery, the §4 invariants |
| **synthetic bench** (`efs-bench --meta`) | is it fast, did it regress | seconds | throughput/latency regressions, op-storm cost |
| **posix / posix2** | is it a correct filesystem (vs XFS) | minutes | semantic / peer-visibility gaps |
| **13-node cluster** | does it perform on real hardware | slowest | perf ceilings, RDMA, NVMe — **not** correctness |

A bug should be caught at the **lowest** layer that can see it. The
simulator is the layer that is both fast and deterministic — unit tests
cannot see a message race, the cluster cannot reproduce one.

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
blocked. **Unrelated bug found while gating (not from this change, present on
HEAD):** `test_lock` had 6 `efs_lock_getlk` (F_GETLK) failures. Closed
Sep 18: the test passed one `efs_lock_req` as both `req` and `out`, and
`efs_lock_getlk` clears `out` before reading `req`, so the request zeroed
itself. The production caller always passed distinct structs, so the
F_GETLK path was correct all along.

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

