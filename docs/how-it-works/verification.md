# Verification — the deterministic simulator and the code→signal cycle

[Architecture](architecture.md) · [Development](developing.md) ·
[Performance](performance.md)

## The simulator (build first, architecture-independent)

Use it for reproducible logical protocol scenarios; public-path and hardware
acceptance remain separate.

**What it is.** A deterministic discrete-event simulator that runs N logical
servers + M clients **in one process**. A seeded PRNG drives message order,
drops, delays, duplicates, partitions, node crashes/restarts, and clock
steps. It shares the Raft core and metadata apply logic with production,
using simulator-specific operation handlers and orchestration against a
**simulated network and simulated disk**. Production host admission and
RPC/FUSE integration therefore need separate coverage.

**Why it helps.** Seeded ordering and controlled fault injection make a
logical protocol failure reproducible. Start at a focused model/unit gate,
then verify public callbacks and real processes, and finally hardware-specific
behavior. Timeouts, saturation and incorrect outcomes must be distinguished
with evidence; none can be dismissed from duration alone. A finite passing
scenario set does not prove every possible interleaving or invariant.

**How it is built.** The metadata core is written to be **transport- and
storage-agnostic**: it sends messages and reads/writes disk through
interfaces. Two backends implement those interfaces: the real one (sockets +
NVMe, what ships) and the simulated one (a message queue + a fault-injecting
in-memory disk, what the simulator drives). Shared core/apply code makes
simulation useful, but different orchestration can hide integration bugs.
For example, `src/sim/sim_raft.c:read_begin_g` starts a fresh read round; the
production host can reuse completed coverage (W82). (This is also why
[development.md](developing.md) makes state-machine purity an architectural
rule.)

## Current coverage and limits

Production metadata runs the Raft host over the durable LSM store;
namespace RPCs carry operation IDs, subject to the exhaustion gap in W76.
Session metadata/coordinator and staged publication admission exist, while
mount-wide integration remains W72. The [public-path review](../status/spec-implementation.md)
records durability, integrity, repair/profile and activation limits.

`tests/test_sim`, metadata/transaction/session tests and real-store tests
exercise different backends and scenarios. `EFS_SIM_KV_DIR` and
`EFS_SIM_RAFT_DIR` select disk-backed simulator stores; inspect actual test
commands/results before claiming those backends ran. Simulated persistence,
process restart on the same live OS and hardware power loss are different
gates. In particular W71's fragment persistence gap is not closed by healthy
POSIX or process-restart acceptance.

The [fault suite](../../tests/faults/README.md) includes simulator and isolated
process/FUSE layers; GAP/not_run is not PASS. Public writeback, memory, GC and
cold-data gates remain in the [handoff](../status/in-flight.md). The final
[NUC acceptance ledger](../../results/measure/20261007-roadmap-rounds/SUMMARY.md)
is a dated build record, not current process inventory or blanket fault coverage.

Sep 2 and Step 9–10.5c implementation narratives are
[archived](../archive/verification-checkpoints-20261007.md). Required events
below are a coverage target, not a statement that every listed generator/event
is implemented or has passed on real hardware.

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
relying exclusively on handwritten invariants. (This follows the documented [FoundationDB testing approach](https://apple.github.io/foundationdb/testing.html): deterministic whole-cluster single-process simulation, seeded
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
itself**. Before the simulator (Step 10.5) that proof was a 13-node wipe +
rsync + rebuild + ssh orchestration, and the signal was poor: most
posixstress "failures" were 15 s timeouts (saturation, not correctness),
and a single run was noise. The strategy is to **push each class of bug
to the cheapest layer that can catch it**, then exercise the relevant public
and hardware paths. Select simulator tests for protocol changes, but do not
substitute an unrelated simulation pass for their named acceptance gates.

The layers, cheapest first:

| Layer | Answers | Cost | Catches |
|---|---|---|---|
| **unit** (`make test`) | does the selected function/scenario satisfy its assertions | test/suite dependent | logic, encode/decode, pack/unpack |
| **simulator** | does the protocol survive the selected seeded faults/interleavings | ms | message-ordering, leader/fencing, recovery, the §4 invariants |
| **synthetic bench** (`efs-bench <seed:port> --meta` for RPCs; `--bench meta` for local KV/Raft) | is it fast, did it regress | seconds | throughput/latency regressions, op-storm cost |
| **posix / posix2** | is it a correct filesystem (vs XFS) | minutes | semantic / peer-visibility gaps |
| **isolated hardware cluster** | does it behave and perform on the actual storage/transport | slowest | performance ceilings plus hardware/public-path correctness (RDMA, NVMe) |

A bug should be caught at the **lowest** layer that can see it. The
simulator controls logical message ordering; focused concurrent tests and
real-process/hardware gates cover behavior outside its model.

Deliberate moves that shorten the loop:

- **Use focused correctness gates first.** Model/unit tests isolate logical
  defects, POSIX/peer tests exercise mounted semantics, and isolated real
  clusters check storage/transport failures as well as performance.
- **Deterministic over flaky.** Replace fork-and-pray races with forced
  collisions (`threading.Barrier`) so a test either always collides or never
  runs — no more "passes isolated, fails under load."
- **Fail fast, not timeout.** A violated invariant aborts at the step, not
  after a 15s `POSIX_TEST_SEC`. Saturation (a timeout) is reported separately
  from correctness (an assertion) so the two are never conflated again.
- **Live attach over rebuild-and-reprobe.** Verify ptrace permissions and the process/build first;
  `gdb -p` on a wedged `efsd` can expose its current wait without a redeploy.
- **Bounded-context change.** Modularity
  ([development.md](developing.md)) keeps the unit of work small: a change
  loads one module + its interface header, not the whole tree. This is what
  makes both fast isolated tests and model-assisted editing tractable.
- **Executable invariants:** simulator assertions and independent checks
  should capture the relevant property. A real-cluster fsck remains a
  capability gap; mentioning it here does not establish an existing tool.


## Authority and lifecycle review gates

The Oct 7 round-5 isolated Raft reproduction ([W82](../backlog/work-items.md#w82))
shows why a completed quorum index is not enough for a later authoritative
read after leader isolation. Existing tests explicitly start a fresh round in
one partition case; they do not cover the host's reuse of old completed
coverage after the majority commits elsewhere. Preserve the failing review
fixture and add that missing acceptance case when fixing the implementation.

Transaction decision retirement ([W83](../backlog/work-items.md#w83)), namespace
predicate limits ([W84](../backlog/work-items.md#w84)) and ambiguous PUT hint
history ([W85](../backlog/work-items.md#w85)) have separate gates. The hint
helper collision is reproduced; downstream multi-root/accounting behavior
remains source concern. These review fixtures are not passing release gates.

Round 6 reproduced [W88](../backlog/work-items.md#w88) in actual metadata apply
over in-memory KV: an old legacy span retry becomes live after folded-history
eviction and a later full image. Public-path byte/packet gates remain open.
[W89](../backlog/work-items.md#w89) also invalidates the old W23 zero-L0
summary: verify field counts/types and raw evidence before interpreting a
measurement. Corrected historical samples are retained without replacing originals.
