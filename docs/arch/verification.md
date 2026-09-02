# Verification — the deterministic simulator and the code→signal cycle

[Architecture](../architecture.md) · [Development](development.md) ·
[Performance](performance.md)

## The simulator (build first, architecture-independent)

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

## Shortening the code → signal cycle

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
