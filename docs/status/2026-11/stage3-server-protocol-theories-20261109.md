# Stage 3 theories: server side and protocol level (Nov 9 review)

**Status (Nov 9): theories only, no code changed, nothing measured for this
page.** Source: devel `9bb50fe79` (wave 67). Verdict: five in-scope
candidates, P3-G to P3-K, entered in section P of the
[alpha checklist](../alpha-1.0-2plus1-checklist.md#p-stage-3-performance);
each names the measurement that would refute it. Remaining failures: none
(no run). Active run: none. Next action: P1 baseline after Stage 2, then
the refuting measurement of P3-G first. Merge state: committed on devel.

Purpose: the user asked for a code review of the server and of the
client-server and server-server protocol for significant performance
improvements inside the alpha 1.0 scope, reported as theories with their
reasoning, no implementation. Method: the measured baselines already in the
tree, then the code they point at, read at `9bb50fe79`. A candidate is
listed only where a measurement and a verified code fact meet. Every "would"
below is a prediction, not a result.

## What the measurements say first

| Fact | Value | Source |
| --- | --- | --- |
| Tiny-file creation on orcd-mini, 16 workers | 1,649 files/s, about 9.7 ms per file per worker | [profile, Oct 18](../2026-10/orcd-mini-profile-5d673efa8-20261018.md) |
| CPU for that, three daemons together | 9.0 to 9.6 cores of 192; leader samples 63 % in the kernel (scheduler, syscall entry and exit, poll) | same |
| Context switches, three daemons | 32,000 to 36,000 per second, about 20 per created file | same |
| Raft commits per created file (mixed workload) | about 5.4, nearly all on group 0 (group 2: 50 to 186 per second) | same |
| Latency p50 with idle CPUs | create 2.4 to 3.4 ms, open 1.7 to 2.6 ms, stat 1.0 to 1.8 ms, unlink 1.4 to 2.6 ms; p99 5 to 73 ms | same |
| Leader lock waits | 11.6 % of its switches; proposals 44 % of those | same, [`h->mu` plan](../2026-10/raft-host-mu-plan-20261016.md) |
| One lease round fewer per created file | +18.7 % direct, +23.3 % buffered creates per second | [W94](../../backlog/work-items.md#w94) |
| Large sequential writes | 7.3 Raft entries per MiB, 5.66 of them ticket admission and settlement; 3,917 to 6,006 of 8,192 admissions per GiB go as single entries | [Task U](../2026-10/k32-write-cost-profile-20261028.md) |
| K32 before its fix | 23 sequential entries per 1 MiB write, throughput a third, "latency-bound, servers mostly idle" | [K32 fix](../2026-10/k32-write-fix-20261023.md) |
| A K96 round | replica bytes over the store disk's synchronous write bandwidth; 850 MiB written for 576 MiB of replica bytes | [Task EA](k96-test-rounds-20261108.md) |

Reading: the daemons are not short of CPU, disk or network on these
workloads. Throughput at a fixed client concurrency is the inverse of the
latency of a chain of sequential consensus rounds, and the two levers are
the latency of one round and the number of rounds in the chain. The tree
already proves the second lever twice (W94, K32). The first has not been
measured.

## Already done, not proposed again

Group commit of the Raft log with the leader's fsync parallel to the send
(`src/raft/raft.c` `send_idx`/`durable_idx`, `src/raft/raft_log.c`); the KV
log barrier taken off the pump (`host_kv_work`); targeted waiter wakes
signalled after unlocking (`host_waiters_wake`); lock-free verdict reads and
routing view (`h->mu` steps 1 to 3); asynchronous fragment acknowledgement
with one barrier per root (`src/server/store.c`, 200 ms or 256 MiB);
fragments written in place with `O_DIRECT`, no rename; TCP with
`TCP_NODELAY` and `writev`; RDMA wake suppression (W47); Stage 1 tasks AB
to AX. P3-A to P3-F stay as they are.

## P3-G: a consensus round costs thread handoffs, not work

Code facts. A proposal appends under `h->mu` and kicks the pump through an
eventfd (`host_propose_impl`, `src/server/raft_host.c:6008`). The single
pump thread sleeps in `poll` for up to one 5 ms tick when its inbox is empty
(`host_pump_poll_timeout`, the `poll` at line 9015), with no spin. It hands
each AppendEntries to a per-peer sender thread through a heap copy and a
condition variable (`host_send`, line 1424; `host_sender`, line 1056). On
the follower the connection thread copies the frame into the inbox
(`server_raft_host_inbox`, line 14279) and wakes that daemon's pump, which
appends, persists and answers through its own sender thread. The answer
takes the same path back, and the pump wakes the proposer
(`host_waiters_wake`). One round therefore wakes a sleeping thread eight
times: pump, sender, follower connection, follower pump, follower sender,
leader connection, leader pump, proposer. Five of the eight are handoffs
inside a daemon.

Reasoning. The work between the wakes is microseconds; two `fdatasync`
calls run in parallel. On a host that uses 9 of 192 cores every woken
thread lands on an idle core, and the kernel share (63 %), the scheduler's
idle-balance symbol at the top of the profile and 20 switches per file all
describe wake cost. A stat that needs one ReadIndex round and no disk write
still takes 1.0 to 1.8 ms. So the theory is that a round's latency is
mostly the sum of eight wakes, and every latency-bound workload (tiny-file
create, ior hard, fsync-heavy writers, metadata reads) scales with it.

Candidates, smallest first: (a) the pump spins for a bounded time before
sleeping while a quorum round or a read round is outstanding, as the RDMA
receive poller already does for 32 empty polls; (b) the pump writes the
frame to the peer socket itself when the socket takes it and uses the sender
thread only on backpressure, which removes two of the eight wakes and both
heap copies; (c) the follower's connection thread runs the receive step for
AppendEntries under the existing receive-persist cohort instead of queueing
to its pump. Size: unmeasured. If wakes are most of a 1 ms round, (a) and
(b) together would cut the round by a third to a half, and creates per
second at fixed concurrency would rise by about the same factor.

Refute it with: `EFS_WRITE_PROF=1` on a private three-daemon cluster under
the tiny-file workload. The theory predicts `commit_apply` several times
`persist`, and `perf sched latency` wake delays that add up to most of
`commit_apply`. If `persist` dominates, the limiter is the device and this
entry closes with that finding. Scope: threading inside one daemon; no
format, no wire change, no group or leader placement. Risk: (c) changes who
steps the Raft core and needs the TSan lane and the K1 stop gates; (a)
spends CPU and must stay bounded and off when idle.

## P3-H: sequential rounds in a small file's life

Code facts. CREATE already takes the open lease in the same entry
(`include/efs/protocol.h:1182`); the close is its own `LEASE_CLOSE` entry
(`include/efs/meta_cmd.h:223`). Each data object needs a ticket admitted
before its fragments are accepted, and the ticket is bound to the body
digest and the three fragment checksums (`struct efs_put_ticket`,
`include/efs/put_ticket.h`), so admission cannot precede the data. Then
come the fragment PUTs, the publication (REPORT) and the settlement.

Reasoning. A tiny file is created, admitted, stored, published, settled and
closed: up to five consensus rounds and one storage round, of which the
writer waits for at least create, admission, PUT and publication in series.
9.7 ms per file at 1.5 to 2 ms per round fits that chain. W94 measured
about +20 % for one round removed and K32 a factor of three for twenty.
Because the ticket carries the checksums, admission and publication could
be one entry for a single-object file: the admitted ticket and the
publication name the same object. Settlement can retire in the publication's
apply. That would leave create, PUT and one publish round on the writer's
path. Size by the W94 ratio: two rounds fewer of five is roughly +40 to
+60 % creates per second, before P3-G.

Refute it with: a per-file histogram of top-level entry tags and of the
rounds the writer blocks on (`EFS_RAFT_OBS`, `EFS_PUT_TICKET_DBG`) on an
untraced tiny-file run. If the writer already waits on two rounds only, the
remaining ones are background and the gain is CPU, not throughput. Not
verified here: which rounds are synchronous for the writer today, and
whether settlement already rides another entry. Scope:
K13 and K14 contracts kept (a fragment is accepted only for an admitted
ticket; publication is atomic); fused admission and publication needs a new
command encoding in the metadata log, which the user must accept as inside
"no new wire profile" or park. The storage node's ticket check needs the
ticket visible before the PUT, so the fused form applies only where the
client may hold fragments until the publish round, which is a second point
for that decision.

## P3-I: a read-only open pays two replicated lease edges

Code facts. Every open of a regular file, read-only included, calls
`efs_open_acquire` (`src/client/efs_fuse/open.c:881`); the first open of an
inode on a mount takes the HOLD lease (`src/client/efs_fuse/open.h:37`) and
the last release returns it, each a log command.

Reasoning. Open p50 is 0.7 ms above stat p50 on both mounts, the price of
one entry. The md5 reader runs 1,336 files/s with 16 workers, 12 ms per
file, and adds two entries per file to the same group 0 that carries the
creates. In scope without a semantic change: keep the lease for a bounded
linger after the last close and return lingered leases in batches, so a
re-open is free and the close edges stop competing with writers for
proposal slots; a staged deferred close edge already exists
(`EFS_MD_SESS_LEASE_EDGE_DEFERRED`); whether one entry can carry many
closes was not checked.
Removing the open edge itself for read-only opens would change what an
unlinked open file promises and is the user's decision. Size: the linger
halves lease entries on a read-mostly tree; it does not shorten a first
open. Refute it with: lease entries as a share of group 0 commits under the
md5 reader. Below a fifth, drop the entry.

## P3-J: single ticket admissions on large writes (Task U's follow-up)

Task U found that 48 to 73 % of FUSE admissions are logged as single ADMIT
entries although `ADMIT_BATCH` (tag 90) exists, and named cohort formation
as the first follow-up; section P did not list it. The cohort is formed on
the client from whatever the kernel delivers together. The leader sees
every session's admissions and can combine those waiting in the same
proposal window into one `ADMIT_BATCH` with the existing tag, which makes
the entry count independent of FUSE callback timing. Reasoning for size:
admission is in series with each chunk's PUT, so this pays through round
latency as P3-G does and through fewer applies; expect a modest gain on
ior easy unless the untraced rerun shows admission rounds queueing. Refute
it with: Task U's matrix untraced, entries per MiB and write MiB/s with
leader-side combining on a private build. Scope: existing tag, K13
unchanged.

## P3-K: the PUT barrier drags unrelated dirty data (tail latency)

Code facts. The fragment barrier is `syncfs(root)` every 200 ms or 256 MiB
(`src/server/store.c:1743`, design comment at 1568). Fragments are
`O_DIRECT` and leave no dirty pages, but the KV writes its segments
buffered and syncs each at its end (`src/kv/kv_seg.c:335`), and the Raft
log, the KV log and the `.efs` index share the filesystem. The Raft log
rotation already paces its own output, and its comment gives the reason:
a large dirty file makes every Raft log sync on the device queue behind its
flush (`src/raft/raft_log.c:580`).

Reasoning. `syncfs` writes back every dirty page on the filesystem, so each
barrier pushes whatever a running compaction has buffered, and the Raft and
KV `fdatasync` calls of that moment wait behind it. That predicts a
periodic stall, and the profile shows p99 at 5 to 20 times p50 with idle
CPUs. Candidate: pace compaction output with `sync_file_range` the way the
rotation does, so a barrier finds little that is not its own. Size: p99,
not p50. Refute it with: private builds that differ only in the barrier window (50,
200 and 1,000 ms; Task FA built the `EFS_PUT_SYNC_MS` setting in, wave 69) on a
private cluster under tiny-file load; the theory predicts create p99
spikes that follow the barrier period and grow with compaction activity,
and `last_barrier_us` peaks at those moments. No correlation closes the
entry. Scope: no format or durability change; the barrier still covers
every acknowledged fragment.

## Looked at and left out

- One pump, one `h->mu`, one Raft log and one KV for both groups: group 2
  carries 1 to 2 % of the commits, so an owner per group would not help;
  the concentration is W66 and is measured only.
- Single-threaded apply: 2.2 to 2.6 % of leader CPU.
- KV memtable as a sorted array with a `memmove` per insert
  (`src/kv/kv_lsm.c:226`): 2.4 to 3.6 % of leader CPU, bounded by the flush
  threshold.
- One RDMA receive poller per device: 3 to 5 % of daemon CPU.
- Eight tickets and 24 fragment files per MiB follow from the 128 KiB
  chunk and one file per fragment, which are formats.
- A clock-based leader lease for reads would remove the ReadIndex round and
  would rest read correctness on bounded clock drift; not proposed.

## Not done

No run, no profile and no private build for this page. Wake latency per
handoff, the writer's synchronous rounds per file and the barrier
correlation are predictions. The client data path (P3-A to P3-D) and the
packer were not reviewed. The orcd-mini numbers are from Oct 18 on
`5d673efa8`, before K13 tickets and wave 67; P1 must retake them.
