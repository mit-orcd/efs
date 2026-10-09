# W97 solution investigation — 2026-10-09

Branch: **devel-persistence-pressure**, created from da653c33. NUC resumed
successfully. Existing data retained; TCP/direct I/O and virtual commands
disabled throughout. The group-isolation trial installed source **a770290a**, HELLO build
**a770290a-pressure**. The subsequent pump-phase trial uses **2cca7cfc**,
build **2cca7cfc-pressure**, with the same replication policy.

## Implemented fix: independent Raft groups keep their priority messages

The production outbox_pri_replace helper selected the first matching message
**type** in a per-peer queue without checking its **Raft group**. A group-2
reply could replace a queued group-0 reply, and heartbeats had the same issue.
These groups have independent log/commit/read progress. A deterministic test
using the actual compiled helper fails on the original code because the other
group's ACK disappears. It passes with the group-isolation fix.

Commit a770290a limits same-type replacement to the same group. It retains
existing same-group coalescing, queue/byte bounds, caller ownership on capacity
refusal, and emergency vote eviction of a heartbeat. No wire, durability,
read-authority or store-format change is made. tests/test_host_outbox.py is
part of the standard test recipe; host pressure policy and Raft/store tests
pass with diagnostics off and on.

This is a reproduced correctness defect. Its exact frequency and share of the
live slowdown were not measured; the experiment does not establish it as the
single cause of receiver pressure.

## Rejected fix: retaining a suffix after an older reply

The first candidate (c5ff653e) retained outstanding-window tracking when an
older successful reply covered only part of it. A synchronous-network
regression failed twice before the change and passed afterward, including
lost-reply timeout, rejection retry and fresh-read progress. Existing
Raft/store suites also passed.

The live result did not meet acceptance: late throughput regressed and
concurrent_appends exceeded the 30-second budget in the full core POSIX suite.
Candidate: **211/212**, peer **64/64** in 234.5 seconds. Restoring the baseline
produced **2/2 append-only passes in 4.6 seconds** and **212/212 full core
passes in 69.7 seconds** with the same jobs/timeout flags. The candidate was
reverted in **6005fc5d** before the group-isolation build. These controls are
sequential; the precise regression mechanism is not isolated. Preserve actual
asynchronous sender/coalescing/probe and wall-driven tick behavior in future
models rather than inferring host acceptance from synchronous fixtures.

A ten-second strace of the rejected peer append gate shows six-byte writes
around 0.1–1.4 seconds. A separate 49 Hz user-cycle profile collected only
36 samples, with no reported sample loss; it is too sparse for a dominant-hot-
path conclusion. It shows LSM/transaction-resolution work occurs, not that
CPU or hardware is the established limiter. These attaches occurred after
timed measurements and affected only the rejected functional-gate phase.

## Clean five-minute observations

All timed workloads use eight workers, seed 970001, fixed readable payload
bytes and original tiny-file size/depth/count distributions. Fresh output
paths and cooperative SIGUSR1 stop preserve current writes. No build or
functional suite overlaps the clean runs. The earlier exploratory run
included compilation and is retained separately, not used as a throughput
baseline. The group trial's higher target prevents early completion; all
measured prefixes remain below the earlier 500,000-file target.

| Run | First 100 s | Middle 100 s | Last 100 s | n3 inbox drops |
|---|---:|---:|---:|---:|
| Baseline 4b149e88 | 112.18 files/s | 60.52 | 48.44 | 91,932 |
| Rejected partial-reply c5ff653e | 122.24 | 72.20 | 38.73 | 79,300 |
| Group isolation a770290a | 139.63 | 43.70 | 70.05 | 129,365 |

Completed files including final drain: **22,407 / 23,592 / 25,639**.
The partial-reply candidate reduces leader data-send counters from 235,836
to 148,748 (about 37%), but fails sustained-performance/live-gate acceptance.
Group isolation records 260,462 accepted core data-send callbacks; its n3
drops comprise 40,458 data requests and 88,907 empty requests. These send
counters count acceptance into the host sender path, not actual wire
transmissions; queued replacement and transport outcomes remain separate. Do not claim pressure is eliminated or
hide the worse middle interval and higher drop count.

| Run | n3 mean sync sections, early/middle/late | n4 mean sync sections |
|---|---|---|
| Baseline | 3.347 / 7.489 / 6.876 ms | 2.880 / 6.545 / 6.157 ms |
| Partial reply | 3.612 / 7.878 / 6.426 ms | 3.148 / 6.802 / 6.087 ms |
| Group isolation | 4.034 / 6.688 / 7.576 ms | 3.511 / 6.182 / 6.643 ms |

Device-wide NVMe write/flush completion times rise within all three runs.
For group isolation, writes average **2.35 / 4.70 / 5.02 ms**, flushes
**2.05 / 3.77 / 4.06 ms**, busy time **85.8 / 91.8 / 88.2 percent**.
Counters include all device users and queue time. No readable NVMe hwmon
sensor was exposed by the unprivileged probe. Do not infer thermal, firmware
or cache behavior from these timings.

## Validation and remaining work

Group-isolation final gates: **212/212 core POSIX**, **64/64 peer** in
32.5 seconds, clean cluster/mount restart, and **128 cold sample payloads**
across all four retained test trees. Both mounts match independently predicted
seeded lengths and content. This is sample verification, not a full tree scan
or power-loss test. Final six process environments confirm diagnostics and
virtual commands off, TCP and direct I/O. No active loader is retained.

**W97 remains open.** Group isolation is implemented and functionally tested;
a consistent sustained-performance improvement is not established. These are
single sequential runs on a growing namespace with differing prefixes,
scheduling, leaders and device/cache state, not randomized identical-state
repeats. Time-aligned windows also cover different completed portions of the
seeded workload. Histogram quantiles are bin upper bounds, dump boundaries
are approximate and observer overhead is uncalibrated. No hardware ceiling
or full W64/W73 acceptance is claimed.

Next investigate bounded handling of extending small-entry windows and empty
commit/read probes using the real asynchronous host queues; measure per-group
physical sends, replacement/eviction, receipt/consensus ACK and queue residence.
Separate Raft, KV WAL and payload persistence plus competing I/O and scheduler
waits. Test adaptive flow control/coalescing with actual read/probe/timeout
semantics, fixed-work as well as fixed-time repeats, idle/mixed p99 and full
fault/recovery gates. Never enlarge queues or remove durability as a substitute
for that evidence.

[Clean baseline](../../results/measure/20261009-w97-clean-baseline/README.md),
[rejected candidate](../../results/measure/20261009-w97-partial-reply/README.md),
[group-isolation evidence](../../results/measure/20261009-w97-group-isolation/README.md).

## Further reproduction: queue residence and pump persistence phases

Diagnostic source **2cca7cfc** adds bounded opt-in histograms without changing
replication admission or durability. Raft/store pass with observation off/on,
and the production outbox-helper regression passes. Another clean five-minute
run reproduces **113.40 → 50.55 → 55.80 files/s**; n3 refuses **94,175 frames**:
**34,276 data requests and 59,899 empty requests**. n4 refuses none. These are
replication frames, **not lost files or durability tickets**. Raft retransmits;
the counters do not identify the exact retry/recovery of every refused frame.

n3 queue residence rises **153 → 360 → 468 ms mean**, measured before each
frame is decoded; this includes waiting behind earlier frames in the current
drain. Histogram p95 upper bounds are **524 ms / 1.049 s / 1.049 s**. Its host
mutex acquisition averages about **0.15 microseconds** overall, so waiting for
that mutex is not the observed pump bottleneck.

Across approximately 301.5 s of n3 observation, pump drain consumes **233.75 s**,
KV WAL release **38.87 s**, and applied-index persistence **24.77 s**. These
serial pump phases total **297.39 s**, about **98.6%** of the observation
interval. Drain includes decode, Raft processing/persistence and metadata apply;
it is not a pure disk timer. Raft sync sections total **226.89 s**, with means
**3.187 / 6.976 / 7.135 ms**. Sync counters can include other threads; their
similarity to drain time supports persistence attribution but is not an exact
partition of drain time. Metadata durability outside the drain also matters.

n4 has no drops but spends **132.24 s draining**, **96.91 s releasing the KV
WAL**, and **64.35 s persisting applied indexes**, across about 295.6 s. Thus
a full inbox is not required to observe substantial durability cost.

Code inspection identifies a pressure-feedback gap: `handler.c` discards
`server_raft_host_inbox`'s return code and sends the ordinary empty transport
RAFT_REPLY even when admission returns BUSY. This acknowledges transport
handling, **not consensus or durable acceptance**. The sender continues without
an inbox-pressure signal. Meanwhile, new proposals may extend and resend an
unacknowledged non-full batch, and commit/read probes add empty requests. This
is a concrete flow-control candidate, not proof that changing it alone resolves
W97. Any fix must preserve retries, read context, group isolation, elections
and mixed-version transport behavior; increasing queue capacity is not a remedy
for the measured serial persistence time.

[Raw phase evidence and method](../../results/measure/20261009-w97-pump-phases/README.md).
Sequential growing-tree conditions and uncalibrated observer overhead still
prevent a hardware-ceiling claim or matched performance comparison.

Phase-build validation: **212/212 core POSIX in 70.8 s**, then a clean restart
with diagnostics disabled and **32 sampled cold payloads matching both mounts**.
All six process environments confirm diagnostics/commands off, TCP/direct I/O.
The loader exited cooperatively. Peer/fault suites were not repeated for this
observation-only change; no new flow-control policy has been deployed.

## Proposed fix: bounded receiver-pressure feedback

**Status: proposal only, October 9.** No flow-control implementation or
performance improvement is claimed here. Implement and test this as the next
W97 candidate, independently of changes to durability batching. The target is
to stop feeding a saturated follower redundant work while keeping replication,
reads and elections live. Persistence still consumes real time; this candidate
may reduce drops without materially improving file creation, which would leave
W97 open.

### 1. Report actual transport admission

Change the EFS_MSG_RAFT handler to preserve the result of
`server_raft_host_inbox`. Extend the existing EFS_MSG_RAFT_REPLY with a small,
fixed, versioned admission-status payload: admitted, retryable pressure, or
non-retryable protocol/host error. Include a bounded retry hint and an overload
flag that can accompany successful admission. Use explicit encoding and exact
length/version checks; do not serialize a native errno or an uninitialized C
struct. Define this payload in the protocol documentation with its codec tests
when implementing it.

An admitted frame has only been copied into the receiver's bounded queue. This
reply must never update Raft match_index, next_index, commit_index, applied
index or read acknowledgements. A BUSY frame was not queued and cannot earn
any of those acknowledgements. The real AE_REP remains authoritative and must
retain all existing durable-log, term, prefix and read-context checks.

Keep one transport reply per received frame, including a refused frame. The
sender must drain every outstanding reply in its bounded pipeline before
waiting or disconnecting. TCP and RDMA/TCP mixed-channel batches can return
replies in different channel order: a peer-wide overload flag avoids inventing
per-group admission credits from that order. If per-frame attribution is added,
track requests separately per channel; never attach replies to a single global
FIFO without proving ordering.

The current sender checks the message type and ignores its payload, so a new
receiver can report status to that baseline without changing its Raft meaning.
A new sender must accept the old empty reply as legacy transport handling,
with admission **unknown**, rather than as proof of admission or recovery from
pressure. Expose legacy mode in diagnostics. Test both version combinations;
existing HELLO build restrictions still apply. Reject malformed new status
payloads using the existing bounded connection-failure/retry path. A legacy
peer may continue dropping frames; do not claim cluster-wide backpressure
until all relevant peers support the status.

### 2. Pace requests to the affected destination

Maintain fixed-size pressure state in each peer sender, guarded by its existing
outbox mutex: pressure deadline, retry delay and last admission feedback.
Start an experiment with a **5 ms** delay on BUSY, exponential growth capped at
**50 ms**, and bounded jitter inside that cap. These are trial parameters,
not established optimal values. Use monotonic timed waits; release the mutex
while waiting and wake promptly for shutdown or eligible control traffic.
Never sleep under the host mutex or make a handler wait for Raft processing.

During pressure, reduce the ordinary request pipeline to one frame at each
retry opportunity; pace data AE, snapshot chunks and ordinary commit probes.
A newer proposal must not bypass that destination's pressure deadline merely
by extending an outstanding batch. Keep normal behavior for healthy peers.
After several successful low-pressure admissions, increase the pipeline
conservatively back toward the existing maximum of eight; an empty legacy
reply or a consensus ACK alone must not clear receiver pressure.

Retain existing bounded outbox ownership and FIFO within each lane. BUSY must
not create an unbounded second retry queue. Unsent frames remain within the
existing bounds; an already transmitted, refused frame is retried by the
existing Raft mechanism. Do not clear or advance Raft progress from this host
feedback, and do not reintroduce the rejected partial-reply tracking change.
Measure retry coverage explicitly: sender pacing must not suppress all retry
opportunities or leave a quiet follower permanently behind.

### 3. Preserve a bounded path for cluster progress

A blanket sleep of the entire peer lane could delay votes and replies and cause
a new outage. Keep Raft replies and election messages eligible during request
backoff. Heartbeats and fresh ReadIndex probes need a bounded escape path per
peer/group, with a timer-driven retry even when no writes arrive. Preserve read
context and prefix validation; receiving a pressure signal cannot satisfy a
read. Exercise simultaneous pressure in both directions and both hosted groups.

At the receiver, reserve capacity **inside** the existing 256-frame / 32 MiB
shared-inbox limits for replies, election traffic and bounded liveness probes.
An initial experiment may reserve 32 slots and 64 KiB, subject to the actual
maximum control-frame size. Ordinary empty commit probes must consume ordinary
request capacity: otherwise the measured majority of empty traffic defeats the
reserve. A full reserve still returns pressure; it never grows. Classify and
validate a frame before granting reserve access. Reserve eligibility must be
precisely represented in the protocol/core contract; do not infer every empty
AE is essential liveness traffic. This classification is an implementation
prerequisite, not permission to reorder or discard Raft messages.

Report pressure before hard overflow using high/low watermarks. Account for
accepted work both in the shared inbox and in the pump's detached local batch;
`drain_inbox` clearing the shared array does not mean the follower finished its
work. Keep accounting fixed-size, include bytes, and retire each frame once on
all decode/error/shutdown paths. Trial ordinary-work high/low marks of 64/32
frames and 8/4 MiB give hysteresis below current hard bounds; validate these
against observed processing latency and control progress before adopting them.
Receiver admission stays FIFO for accepted frames in this first candidate.
A reserve enables admission, but does not itself shorten an existing long drain;
if liveness still fails, reject the candidate and investigate bounded drain
scheduling separately rather than silently changing message ordering.

### Acceptance and decision

Add deterministic tests around the actual asynchronous host sender/receiver
paths, not only a synchronous Raft fixture. Force a slow pump and hard-full
inbox; verify admission status, reply draining, bounded memory, retry progress,
shutdown wakeup and eventual convergence after pressure clears. Cover both
Raft groups, pressure in both directions, fresh reads, votes, leader/term/boot
changes, lost replies, conflict repair, snapshots, malformed status and legacy
replies. Exercise mixed TCP/RDMA reply channels in the harness; retain live RXE
coverage as a separate gate before claiming transport-wide acceptance. Preserve
the existing cross-group ACK/heartbeat overwrite regression.

Record **physical** sends and transport admitted/BUSY outcomes separately from
core enqueue callbacks and consensus ACKs. Include request/control class,
peer/group where correlation is valid, throttled time, queue residence/depth,
retransmitted bytes, leader changes, replication lag, durable entries per sync,
phase timings, file-operation p50/p95/p99, device counters and RSS. Establish
that dropped/refused traffic eventually catches up; an aggregate drop counter
cannot establish that by itself.

Run at least three alternating baseline/candidate repetitions on NUC, with
builds finished before timing, fixed seeds and balanced fresh path prefixes.
Use both fixed work and runs of at least 15 minutes to cover the decline; retain
per-100-second rates and final completion/drain times. Preserve existing data,
monitor space, and report changing namespace/cache/device state as a remaining
confounder. Repeat an observation-disabled control to assess diagnostic cost.
The candidate must improve sustained completion and operation tails beyond
run-to-run spread, with bounded lag and no new idle/mixed-load or election/read
regression. Fewer drops alone is insufficient; higher startup throughput is
insufficient. Reject or revise the candidate if it merely moves backlog into
sender queues or starves a slow follower.

Before promotion: Raft/store fault and recovery suites, full POSIX and peer
suites including concurrent append, cold payload verification and applicable
W64/W73 durability gates. Keep the current deployable build as rollback.

If feedback controls pressure but persistence time still limits throughput,
continue with **W93** durable batching as a separately measured change. The
current contiguous receiver batches are split by interleaved groups and stale
or empty requests. Any per-group scheduling or duplicate handling must prove
term/configuration/read/ACK ordering with the real async host before a live
trial. Investigate the KV release and applied-index barriers separately as
well; do not lengthen checkpoint intervals, skip syncs, or relax durability in
this W97 feedback candidate. No hardware ceiling is established.

## Implementation candidate: transport admission and bounded host pacing

October 9: candidate implementation on `devel-persistence-pressure`, initially
opt-in with **EFS_RAFT_FLOW=1**; unset/0 preserves the baseline admission and
empty transport reply. The knob is independent of storage admission pressure.
This is not yet sustained-performance acceptance or a W97 closure.

The eight-byte transport reply is `EF`, version 1, status (0 admitted / 1 retryable
pressure / 2 host/protocol error), overload flag, reserved zero and big-endian
retry milliseconds. Exact length/version/status validation fails malformed
replies through the existing connection-retry path. Legacy empty replies are
accepted with admission unknown and cannot clear pressure. All outstanding
transport replies are drained after BUSY, including mixed TCP/RDMA pipelines.
Consensus AE_REP processing remains unchanged.

Per-peer monotonic pacing starts at 5 ms and caps at 50 ms with bounded jitter;
ordinary pipeline recovery doubles 1/2/4/8 after successive groups of eight
low-pressure admissions. Control replies cannot move an already scheduled data
retry into the future forever. Replies/votes bypass pacing; a due data slot is
reserved before bounded probe selection. New entry accumulation during pressure
is limited to one pending entry frame per group inside existing outbox bounds;
refused enqueue returns AGAIN without marking an unsent Raft window as delivered.
Existing entry-lane FIFO and same-group control coalescing remain in place.

No new Raft wire flag is introduced. During pressure, cached current AE headers
supply an empty zero-prefix probe per hosted group every 50 ms (new nonzero read
context may escape after 5 ms). Existing vote_granted probe semantics preserve
outstanding data tracking. Term/boot/group/read context are retained; zero-prefix
proof cannot replicate or commit the refused tail. A stale cached context earns
no new read authority. Receiver reserve access for empty probes has the same
bounded peer/group cadence; other empty traffic uses ordinary capacity.

Receiver framing, group, sender, recipient and entry lengths are checked before
reserve access. Replies/votes and bounded empty probes can use capacity reserved
inside 256 frames / 32 MiB. The combined accepted-work counter includes detached
pump work until Raft drain, KV release and applied-index persistence finish.
Ordinary-work high/low marks are 64/32 frames and 8/4 MiB; hard bounds apply to
all accepted work. Rejection counts now include intentional soft-watermark
refusals as well as hard overflow; compare physical sends and admission outcomes,
not the old overflow interpretation of inbox_drop alone.

New regression `tests/test_host_flow.py` compiles actual production admission,
retirement, selection and sender functions against an asynchronous transport
fixture. Linux passes bounded admission/detached accounting, both groups,
control/fresh-read escape, non-starving data retry, independently ordered
TCP/RDMA replies, BUSY pipeline draining, recovery, legacy/malformed/lost reply
handling and shutdown. The transport fixture does not certify physical RDMA.
A Raft regression proves fresh zero-prefix probes can establish read authority
without committing refused data, then ordinary retry catches both groups up.
Existing Raft/store/wire and group-isolated outbox tests pass. Live gates and
repeated sustained comparisons remain required before promotion.
