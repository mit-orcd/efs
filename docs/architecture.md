# Architecture

[Design](design.md) · [Failure tolerance](failure-tolerance.md) · [Operations](operations.md) · [Scaling roadmap](scaling-roadmap.md)

This is the document the system is built against. It states the goal, the
failure model, the consistency model, the invariants, and the architecture
that satisfies them. The [scaling roadmap](scaling-roadmap.md) is the
*increment plan* for the current implementation; this is the *specification*.
When a design question comes up, the answer is decided here first, then
reflected in the roadmap.

**Status: ratified Sep 1 2026.** The metadata layer described here replaces
the current whole-table snapshot + 2PC design. The data path is unchanged.

---

## 1. Goal

A high-performance parallel POSIX file system.

| Property | Target |
|---|---|
| Objects (files + dirs) | ≥ 2³² (4.29 billion) |
| Cluster size | 3 nodes (smallest) to 64 nodes |
| Reference size | 2³² objects on 4 nodes |
| Failure tolerance | survive any 1 node down with full read+write |
| Consistency | immediate cross-client visibility (no TTL caches) |
| Data path | client-direct, RDMA, 2+1 EC (unchanged) |

The 2³²-on-4-nodes number is the binding constraint. It is what forces
metadata out of RAM and onto local NVMe, and it is what forces the metadata
store to be paged rather than serialized whole.

## 2. Failure model

- Nodes fail by crashing (stop, restart, rejoin). No Byzantine behavior.
- The network can drop, delay, reorder, and partition messages. Partitions
  heal.
- Up to **1 node** may be down at once and the system stays fully available.
  (2+1 EC on data; Raft on metadata — both tolerate exactly one failure.)
- Clocks are loosely synchronized (NTP). Used for expiry/TTL and cache
  hints only — **never** for correctness of replication or fencing.
- A node that loses its local storage rejoins empty and rebuilds from peers.
- Clients are untrusted and may crash or be killed at any point.

## 3. Consistency model

- **Metadata: strict serializability per shard.** Every metadata operation
  is applied on exactly one shard's Raft group, in log order. A client that
  receives success for op *O* sees *O*'s effects in every subsequent op, from
  any client, immediately. There are no stale reads and no TTL caches.
- **Cross-shard operations** (rename across dirs, hardlink, unlink-with-open)
  are the only multi-shard cases. They are rare and are handled by a
  documented two-phase pattern with a reconcile rule, not by a global lock.
- **Data: read-your-writes within a client; quorum durability across
  clients.** A write returns after 2-of-3 fragment acks. A subsequent read
  from any client that has seen the metadata commit reconstructs the data.
  Un-fsynced data can be lost on client crash (POSIX).

## 4. Invariants

These are the properties that must hold under every interleaving, including
the failure model. They are the test oracle for the simulator (§9) and for
`fsck`. A bug is a violation of one of these.

**Replication / leadership**

- **I1 (single leader).** At most one node believes it is the leader of a
  shard group at a given term, and only a node with a majority of votes in
  that term can become leader.
- **I2 (leader completeness).** A committed log entry is present in the log
  of every subsequent leader.
- **I3 (fencing).** A node that has lost its leadership (higher term seen)
  rejects client writes and does not apply new entries.
- **I4 (no dual owner).** At most one node serves writes for a shard at a
  time. (Follows from I1 + I3.)

**Metadata state**

- **I5 (name uniqueness).** Within a directory, a name maps to at most one
  inode.
- **I6 (no dangling dentry).** Every dentry points to an existing inode row.
- **I7 (nlink).** An inode's `nlink` equals the number of dentries naming it.
- **I8 (no resurrection).** A committed unlink is never undone by a later
  flush, report, rebuild, or leader change.
- **I9 (absent ≠ unavailable).** A lookup that cannot reach the authoritative
  store returns an error (EIO/BUSY), never `ENOENT`. `ENOENT` is only ever
  returned for a name that is genuinely absent. (This is the Sep 1
  dangling-dentry bug class.)
- **I10 (durability).** An operation acknowledged to a client survives the
  failure of any single node.

**Data**

- **I11 (chunk durability).** An acknowledged chunk has ≥ 2 of its 3
  fragments on distinct nodes.
- **I12 (no torn chunk).** A read returns either the old chunk or the new
  chunk, never a mix.

## 5. Architecture

### 5.1 The split

Two layers, cleanly separated:

- **Data path** — chunk PUT/GET, 2+1 EC, client-direct over RDMA. **Unchanged.**
  It already scales and meets the performance targets.
- **Metadata path** — a sharded, replicated, logged, on-disk store. **This is
  the rebuild.** It replaces the in-RAM table + whole-table serialize + CoW
  page flush + 2PC root commit.

### 5.2 Metadata: one Raft group per shard

The inode space is sharded (today: `shard = ino & ((1<<shard_bits)-1)`).
**Each shard is an independent Raft group.** This is the Ceph PG / CockroachDB
range model.

- **Shard count.** Sized so a shard holds ~1M inodes. 2³² / 2²⁰ ⇒
  `shard_bits = 20` ⇒ up to 4096 shards. Shards are cheap; a node hosts many.
- **Replication.** Each shard's log is replicated to **3 nodes** (RF=3), so
  one node loss leaves a majority (2 of 3). On a 3-node cluster every shard
  is on all 3 nodes. On 4–64 nodes, shards are spread by placement.
- **Leadership.** Each group elects its own leader. Leaders are spread across
  nodes, so metadata write throughput scales with node count — there is no
  single metadata primary.
- **Log.** Every mutation (create, unlink, setattr, rename-step, report) is a
  log entry with `(term, index)`. An op is committed when a majority of the
  group's replicas have it durably. This is what makes I1–I4 and I10 true by
  construction.

### 5.3 Applied state: an on-disk ordered KV per shard

Each replica applies its log to a **local, embedded, ordered key-value store**
on NVMe. RAM is a bounded cache in front of it, not the store.

- **Keys.** Two key spaces per shard:
  - `(parent_ino, name) → inode_row` — the dentry index; serves LOOKUP and
    readdir (ordered, so readdir is a range scan).
  - `(ino) → inode_row` — the inode index; serves GETATTR/setattr.
- **Values.** The slim inode row (Phase 4: name moved out, ~96–128 B).
- **Why a log + KV, not a snapshot.** The current design serializes the whole
  table to discover what changed; the Sep 1 measurement is `incr=0` on 100%
  of flushes during growth and `ser=350ms` per flush at 10M. A log only ever
  writes what changed. Snapshots exist only for **log truncation** (a lagging
  replica catches up from a snapshot + recent log), not as the durability
  mechanism.
- **RAM.** A bounded block cache + the leader's in-flight state. 2³² × ~128 B
  ≈ 512 GiB of *data*, but it lives on NVMe; RAM holds only the working set.
  This is what removes the 1.9–3.4 KB/inode RAM wall.

### 5.4 The commit path

1. Client sends the op to the shard's leader (owner routing already exists).
2. Leader appends to its log, replicates to the group's followers.
3. On majority ack, the entry is committed; the leader applies it to its KV
   and replies to the client.
4. Followers apply in log order.

There is no whole-table flush, no CoW page placement, no GC of referenced
pages, no extras-merge, no 2PC, no catchup-vs-GC race. Those subsystems are
**deleted**, not patched. Most of the bug surface in the project history
(dual-writer root gen, extras-loss-on-adopt, catchup-vs-GC, peers pinned at
an old gen, adopt-before-pages-local, rebuild-discards-unflushed-ops) is the
failure-mode enumeration of the snapshot model; the log model does not admit
them.

### 5.5 Cross-shard operations

Only rename-across-dirs, hardlink, and unlink-while-open touch two shards.
They use a two-phase pattern (prepare on the child shard, commit on the
parent shard) with a documented reconcile rule on crash (e.g. "hash-shard
row wins, parent-shard row loses"). These are rare and already have the
fan-out skeleton (`CREATE_SHARD` / `UNLINK_SHARD`). They are *not* a reason
for a global lock or a global consensus group.

### 5.6 Membership

Cluster membership (which nodes exist, which are live) is itself a small
Raft group (the "control group"), separate from the data shards. This removes
the current "primary = lowest live node id from a local liveness view" —
which is a pure function of unreplicated state and admits split-brain.
Shard→node placement is derived from the committed membership, so every node
computes the same owner for a shard.

## 6. Why the theory holds up

Let's be precise about what is and isn't claimed. **Raft-per-shard over an
embedded KV is not a new mechanism** — and §5 says so. The claim is stronger
than novelty: **the design is a consequence of the constraints, and the
argument for it is a derivation, not an analogy.** A copy-cat cites a system
that works and imitates it. A derivation starts from what must be true and
shows only one shape survives.

### The constraints are forces, not preferences

Four facts about the goal act as forces that eliminate designs. None is a
taste choice.

| Constraint | Force it exerts | Design it eliminates |
|---|---|---|
| 2³² objects on 4 nodes | ~512 GiB of inode data cannot live in RAM | any fully-in-RAM table (today's) |
| immediate cross-client visibility | no authoritative cache may go stale | TTL / negative / attr caches as truth |
| survive 1 node down, read *and* write | a write must commit without the dead node | 2PC, primary-backup without failover |
| 3 → 64 nodes | work must spread, not funnel to one writer | a single cluster-wide metadata primary |

### Each force admits exactly one answer

- Must page from disk → **an ordered KV on NVMe**, RAM a bounded cache.
- Commit without the dead node → **majority consensus (quorum)**, 2 of 3.
- No single writer → **shard the keyspace** into independent units.
- Spread the writers → **a leader per shard**, leaders spread across nodes.

The four answers compose into one shape: **one Raft group per shard, a
replicated log applied to an on-disk KV.**

This is why it is not a copy of Ceph or CockroachDB. Those systems arrived at
the *same* shape because the *same four forces* act on any strongly-consistent,
horizontally-scaled, fault-tolerant store. We are not imitating their
solution — we are solving the same equations, and the equations have one
solution. Convergent evolution is evidence of a correct derivation, not of
imitation.

### The safety properties are theorems, not testing goals

The current system produces the same bug family forever because its safety
rests on *careful coding* — and careful coding does not compose under message
reordering. The target rests on two results that are **proven**, not tested:

- **Majority intersection.** Any two majorities of a fixed set share a member.
  An election (needs a majority) and a committed write (needs a majority)
  always overlap on at least one node — which is what guarantees a new leader
  learns every committed entry. This is I1, I2, I4, and it is arithmetic.
- **Quorum durability.** A write acknowledged by a majority survives any
  failure that leaves a majority, because at least one survivor holds it. With
  RF=3 that is exactly one node. This is I10, and it is why "survive 1 node
  down" is a theorem here and a hope in 2PC.

2PC fails the second theorem by construction: its commit point is the
coordinator, a single node, so coordinator loss mid-commit blocks or diverges.
That is not a bug in our 2PC — it is what 2PC *is*. The bug history
(dual-writer root, peers pinned at an old gen, adopt-before-pages-local) is
the system repeatedly discovering that its commit protocol does not satisfy
quorum durability. A design whose safety is a theorem does not carry a bug
budget for that class — the class is excluded by the proof.

### What is genuinely ours

The mechanism is proven and shared. What is specific to efs — where the real
design work lives — is the **composition**:

- **Metadata on a log, data on EC — decoupled.** Strong consensus where
  correctness is cheap (small metadata), erasure coding where bandwidth
  matters (bulk data). Most systems pick one consistency substrate for both;
  efs puts each on the substrate that fits its cost model.
- **POSIX on sharded consensus.** rename / hardlink / unlink-while-open are
  cross-shard. The two-phase + reconcile rule that keeps POSIX atomicity
  *without* a global lock or a global consensus group is not in any textbook.
- **The inode record shaped for the KV.** The 512 B self-contained row and the
  (parent, name) + (ino) key split are designed around the store, so readdir
  is a range scan and lookup is a point get — not bolted on after.
- **Deterministic simulation as the gate.** Making the whole thing testable
  in-process from a seed turns "the theory holds" from an argument into a
  checkable artifact.

**The thesis in one line:** the constraints force the shape, the shape's
safety is a theorem, and the part that is ours — the composition with EC data
and POSIX semantics — is where the design earns its keep. That is a stronger
position than novelty: a novel-but-unproven design is a risk; a
derived-from-first-principles design on proven primitives is a foundation.

## 7. Why this satisfies the goal

- **2³² on 4 nodes:** metadata is on NVMe, paged by the KV, RAM is a bounded
  cache. The 512 GiB of inode data fits on the 4 nodes' NVMe; RAM holds only
  the hot set.
- **3 → 64 nodes:** shards re-balance across nodes; leaders spread; adding a
  node moves some shard replicas onto it. Smallest cluster (3) runs every
  shard at RF=3 on all three.
- **1-node tolerance:** RF=3 per shard + 2+1 EC on data both survive one
  loss.
- **Immediate visibility:** ops are applied on the leader in log order and
  acknowledged only when committed; there is no cache to be stale.
- **Performance:** the data path is untouched. Metadata op cost becomes one
  leader RTT + a majority replication, which is the floor for any consistent
  system; the win is that it no longer degrades with table size.

## 8. What is deliberately rejected

- **Millions of tiny consensus groups beyond the shard count.** 4096 groups
  is the ceiling; more re-creates the catchup-storm class.
- **MVCC / multi-version metadata.** The Raft log is the single time axis.
- **A second storage path for metadata pages.** Everything goes through the
  same log + KV. (Two-paths-diverge has bitten twice.)
- **Client-side metadata caching for correctness.** Caches may exist for
  performance but are never authoritative.
- **Kernel module.** Stay on FUSE; the low-level (inode-based) FUSE API
  migration is a separate, orthogonal client rewrite.

## 9. The simulator (build first, architecture-independent)

The single highest-leverage tool, and it pays for itself regardless of the
metadata design.

**What it is.** A deterministic discrete-event simulator that runs N logical
servers + M clients **in one process**. A seeded PRNG drives: message order,
drops, delays, duplicates, network partitions, node crashes/restarts, and
clock steps. The real metadata state machine (the Raft groups, the KV apply,
the op handlers) runs inside it against a **simulated network and simulated
disk**, not sockets and NVMe.

**Why it changes the dev cycle.** Today every correctness gate is a 13-node
wipe + rsync + rebuild + ssh orchestration, and the signal is poor (99% of
posixstress "failures" are 15s timeouts = saturation, not correctness).
Distributed bugs are only findable end-to-end on physical hardware — the
slowest, least reproducible place possible. In the simulator:

- A full cluster scenario runs in **milliseconds**.
- A failure **replays exactly** from its seed.
- You explore **millions of interleavings** overnight.
- The invariants of §4 are checked after every simulated step, so a bug is a
  seed + a violated invariant, not a log line on a live cluster.

Nearly every bug in the project history is a message-ordering or
failure-timing bug that a simulator finds in minutes and that never needed
NVMe or 13 nodes to reproduce. This is the FoundationDB / TigerBeetle central
thesis: make the distributed logic deterministic and testable in-process, and
the hardware cluster becomes a performance harness, not a correctness oracle.

**How it is built.** The metadata core is written (or refactored) to be
**transport- and storage-agnostic**: it sends messages and reads/writes disk
through interfaces. Two backends implement those interfaces: the real one
(sockets + NVMe, what ships) and the simulated one (a message queue + a
fault-injecting in-memory disk, what the simulator drives). The same compiled
state machine runs in both, so "passes in simulation" is meaningful.

**Scope.** It models the metadata layer (Raft, KV apply, op handlers,
cross-shard 2PC, membership). It does not model the RDMA data path — that is
covered by the existing perf harnesses.

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
- **Invariants as executable checks** (simulator assertions + `fsck`), not
  prose — so "is this a bug" is decidable without a human reading a log.

## 10. Migration shape (high level)

This is not a big-bang rewrite. The order is chosen so each step is gated and
the system stays runnable:

1. **Simulator first.** Stand up the deterministic harness against the
   *current* metadata state machine. It immediately starts finding the
   existing bug class and becomes the regression gate for everything after.
2. **Embed the KV.** Introduce the on-disk ordered KV as the applied state
   for one shard, behind the existing op handlers. RAM becomes a cache.
3. **Introduce Raft per shard.** Replace the whole-table flush / 2PC / CoW
   page commit with the replicated log, one shard group at a time.
4. **Membership as a Raft group.** Replace local-liveness primary election.
5. **Delete the snapshot machinery.** flush_blob, CoW page placement, GC,
   extras-merge, catchup-vs-GC — removed once no path depends on them.

The detailed, gated steps live in the [scaling roadmap](scaling-roadmap.md);
this document is the invariant they are measured against.

---

## Appendix: terms

- **Shard** — a contiguous slice of the inode space; the unit of replication
  and leadership.
- **Raft group** — the 3 replicas + log for one shard.
- **Leader** — the single writer for a shard in a given term.
- **Term** — a monotonically increasing leadership epoch; used for fencing.
- **Committed** — present on a majority of the group's replicas.
- **KV** — the embedded on-disk ordered key-value store holding applied state.
- **Control group** — the small Raft group that owns cluster membership.
