# W93 atomic apply recovery and sustained acceptance — Oct 10

Status: **W93 closed Oct 10** for bounded durable batching and the stated AMD/NUC acceptance. Runtime defaults remain unchanged; the tested NUC bundle remains opt-in.

Latest integrated acceptance is engine `37ad6a8790c8-integration`, including all
16 final captured branch tips and the receipt-window admission repair. The new
four-run comparison improves candidate mean by **30.54%**, passes the larger
idle-read matrix and restores both NUC mounts with exact binary identities.
See the integrated acceptance sections below; the earlier build retains its
separate evidence and results.

All 15 initial branch tips captured in the retained merge manifest were integrated into
`devel-persistence-pressure`; other checkouts and branch pointers are preserved.
The first repair (`8c214f7e`) fixes a receipt race: a peer can complete an APPEND
between the initial receipt miss and the later authoritative receipt read.
That later read must return the canonical result rather than reserve again at
an advanced EOF. Six actual-source tests cover direct/captured requests,
completed/changed-payload/retired receipts. The old Linux library fails the
negative control; the repaired library passes. Linux full native tests and
local ASan/UBSan pass. The real ACK-bearing replay test subsequently passes.

The full private 100 ms gate initially exposed harness errors: its stop path
used `mountpoint`, whose GETATTR can fail while the FUSE client remains alive,
and could skip asking its own client to exit. Always run safe stop for the
owned client; allow its existing 30-second drain budget. The unchanged durability
assertions then pass: POSIX 212/212, peer 64/64, sync/append faults, dirty stop,
mutation shutdown and corrupt WAL tails. Dirty stop takes 26.728 seconds and
retains exact cold payloads. W66 now fences all storage members: the partition
test must retain an incomplete fence during partition and resume the same epoch
after healing. That current contract passes. The KV persistence failure gate
also passes after matching the current production diagnostic, with marker-byte
and cold metadata assertions unchanged.

## Real recovery defect and repair

The 1,000 ms retained-checkpoint fault revealed a real bug. After actual n3
checkpoint `pwrite` EIO and a private process crash, all eight hardlink counts
were **3 instead of 2**. Exact copied cold n3 KV confirms this. Replaying
already applied PREPARE/RESOLVE can increment the count again; a separate stale
checkpoint cannot make those operations idempotent. Do not weaken this gate.

One applied Raft entry now stages its KV effects and commits those effects plus
a validated node-local group cursor in one atomic WAL record. The staging bound
is 32,768 effect keys and 64 MiB. The applying thread sees its overlay, scans
merge it with committed state, other readers retain the old committed view,
and competing writers wait until commit/abort. No outer LSM mutex is held
across dispatch. Failed or oversized scopes cannot publish partial effects.
The cursor is committed before publishing the apply verdict or re-enabling a
previously incomplete replica's campaign. ACKs remain behind existing durable
watermarks and KV sync barriers.

The two fixed cursor keys use a shard prefix outside the valid range. Group
images neither export them nor erase the receiver's cursor. Startup restores
the maximum validated KV cursor and valid legacy checkpoint; missing KV keys
support upgrade, malformed or unreadable proof refuses startup, and a cursor
beyond the recovered Raft log refuses startup. Legacy files remain. An older
binary may only be restored after clean shutdown brings those hints current;
crash downgrade to code that ignores cursors is unsupported.

Meaningful tests cover staged get/delete/ordered scans, reader isolation,
writer exclusion, abort, failed commit, key/byte limit refusal, crash before
and after commit, and a torn final WAL record exposing neither effect nor
cursor. Actual-source cursor decoding covers upgrade, stale/torn hints,
malformed proof and IO/NOMEM/BUSY refusal. Group image tests preserve the target
cursor. Local ASan/UBSan passes these apply-scope cases. An initial Linux private
1,000 ms marker-fault smoke now passes all eight exact payload/link/count/mode/
truncate/absence checks and a cold offline n3 KV oracle. That offline helper
also had an invalid null decision callback; it now supplies the real local KV
transaction decision reader. Temporary smoke binaries identify as `unknown`;
final acceptance must use a frozen committed build and repeat the full gates.

## First frozen acceptance procedure (completed)

The final sequence rebuilds all five binaries with an exact committed ID and repeats full native,
full POSIX/peer/sync/append/dirty-stop/WAL gates, candidate switches, KV EIO,
maximum checkpoint EIO/crash and partition/recovery gates on that code.
It then deploys to the preserved NUC service and runs four 900-second balanced
ABBA cells: 10 ms default cohorts versus 100 ms + overlap/no-op batching.
The treatment is a bundle, not isolation of individual switches. Retain three
300-second windows, per-group barrier/entry data, worker and idle/mixed read
p50/p95/p99, RSS/pressure/replica lag, exact identities and 512 cold payload
checks. No physical power-loss or distributed-RDMA acceptance is implied.

Raw controls and evidence: `results/measure/20261010-w93-closure/` locally and
`/data1/efs/evidence/w93-closure-20261010/` on NUC. Original failed private stores and
copied follower metadata remain preserved. The service cluster is deployed with the frozen repaired build. It is restored
to 10 ms defaults, `.efs` and diagnostics off, both preserved mounts up.

## Intermittent peer append gate

Frozen `ad69a9973384` full native passes. Its first full 100 ms live run passes
POSIX 212/212 but peer is 63/64: concurrent append returns EBUSY on client B.
The original fixture and raw logs remain preserved. A diagnostic-client full
repeat and a normal-client repeat with only open/close tracing pass 212/212 and
64/64, plus cold WAL recovery. Eight append/truncate races and an 800-record
handoff fixture also pass. These repetitions do not prove the first failure's
exact trigger or establish a hardware ceiling.

Review identifies a definite OPEN policy gap: unlike retained CREATE, OPEN
calls lease resume once and returns an unresolved BUSY to the application.
Transient transaction/close-step guards can legitimately leave this exact edge
unresolved. OPEN now waits on the retained edge within one 30-second budget,
including inode serialization, FileID and compensating cleanup. It never
extends an inherited deadline, replaces unknown identity, retries a completed
rejection, or discards pending ownership. Mutex acquisition is deadline aware.
IO/NET and terminal failures retain existing error/cleanup behavior.

Actual-source tests cover two BUSY replies then success using one identity,
terminal known BUSY, inherited deadline expiration, retained unknown edge,
blocked mutex timeout and IO ownership. The old-source negative control fails
its first transient-BUSY assertion; the repair passes. Existing shared-open,
ambiguous OPEN/CLOSE, reopen barrier and stop-retry tests remain passing.
The final release-binary native and all live gates pass before deployment;
the original intermittent trigger is not claimed reproduced by this unit test.


## First frozen safety and sustained NUC results — 5b2df4bd

Engine `5b2df4bd32be-closure` passes full native regression and partition /
publication / session / voter-change gates. Three complete private release-build
runs pass POSIX 212/212 and peer 64/64: 100 ms baseline cohorts, 100 ms overlap/no-op,
and unchanged 10 ms defaults. All three also pass sync-write/APPEND lost-reply,
dirty-stop, shutdown-mutation, corrupt-WAL-tail and exact cold recovery checks.
Private peer durations are 132.4 / 152.8 / 35.1 seconds respectively; those are
functional gate timings, not a controlled performance attribution. Preserve them.
Actual KV-WAL EIO and retained-marker EIO/private crash at the maximum 1,000 ms
cadence pass exact payloads, inode/link count, mode, truncate, absence and the
follower offline KV oracle. The private fault setup now starts n3 last, after
other voters elect, while retaining its follower/catch-up assertions.

The service deploy drained both mounts and all old daemons, preserved the existing
stores, and installed the frozen build. Readiness retries retained daemons, never
wiped a store. Four 900-second eight-worker balanced ABBA cells completed with
zero worker errors and no worker cap hit. Leaders remained n1/group 0 and n2/group 2.
The final control's first attempt failed a pre-load n4 GC-status request before
any loader existed. Its raw startup evidence is retained; a bounded readiness
retry applies outside timed work only. `harness-provenance.json` records that
controller change. Timed work and sampling remain identical.

| Cell | Files/s | 0–5 / 5–10 / 10–15 min files/s | Operation p50 / p95 / p99, ms | Mixed-read p99 group 0 / 2, ms |
|---|---:|---|---|---|
| Control 1, 10 ms | 66.56 | 83.46 / 58.74 / 57.49 | 84.25 / 354.73 / 467.17 | 277.89 / 510.11 |
| Candidate 1, 100 ms + overlap/no-op | 82.33 | 99.51 / 73.37 / 74.11 | 68.41 / 279.82 / 361.89 | 212.19 / 392.49 |
| Candidate 2, same | 81.39 | 98.16 / 73.36 / 72.66 | 70.55 / 272.45 / 342.69 | 207.12 / 454.75 |
| Control 2, 10 ms | 60.45 | 76.69 / 52.26 / 52.41 | 95.62 / 343.52 / 443.81 | 232.39 / 478.02 |

Mean candidate rate is 81.86 versus 63.51 files/s across controls, **+28.9%**.
Both candidates beat both controls, including settled windows; the changing
control rate still limits exact causal attribution. This is a bundled treatment,
not isolation of individual switches. Group scheduling and early retirement stay
off. Approximately 181 mixed reads per group/cell give limited p99 confidence.

n3 mixed-group durable entries/sync are 5.08 / 6.55 / 6.45 / 4.47 in ABBA order;
n4 group 2 entries/sync are 1.070 / 1.144 / 1.164 / 1.078. n3 applied-marker phase
time falls from 140.8 / 144.9 seconds in controls to 75.7 / 75.8 in candidates;
n4 falls from 215.2 / 212.8 to 44.1 / 43.9 seconds. Marker barriers, actual sync
syscalls, Raft entry histograms and phase elapsed time are separately retained.
All samples show zero local commit-minus-applied lag; sequential replica probes
observe transient commit spreads, and all replicas cover each final leader cut.
Raft sync errors are zero. Inbox refusals remain on n3; transport pressure is
not declared eliminated. Queues retain explicit frame/byte bounds and apply
staging retains key/byte bounds. RSS and GC/disk snapshots are retained.

After clean restoration to 10 ms defaults, `.efs` and diagnostics off, all 512
sampled first/last exact payload checks pass through both cold mounts. No process
crash test is represented as physical power-loss or distributed-RDMA acceptance.
No hardware-ceiling claim follows from these results.

Idle group 2 p99 in the short probes is variable: 18.7–25.8 ms in first control,
30.0–36.8 in candidates, and 31.6–36.5 in final control. Do not hide this behind
lower candidate medians. The separately matched 6,000-read-per-group default / cadence-only /
full-candidate / default check below completes this acceptance step.


## First acceptance: larger idle check and disposition

All four larger read-only cases use the same frozen binaries and immutable
payloads, exclude startup/warm-up, retain every raw read, verify identities and
use the same paired group order. Each case has 6,000 reads per group.

| Settings | Group 0 p50 / p95 / p99, ms | Group 2 p50 / p95 / p99, ms |
|---|---|---|
| Default 10 ms, control 1 | 6.43 / 11.17 / 15.78 | 8.14 / 13.48 / 38.46 |
| 100 ms cadence only | 5.63 / 10.07 / 14.54 | 6.52 / 10.37 / 34.91 |
| 100 ms + overlap/no-op | 5.67 / 9.91 / 13.84 | 6.55 / 10.36 / 34.43 |
| Default 10 ms, control 2 | 6.56 / 11.24 / 16.11 | 8.12 / 12.92 / 39.65 |

The bundle does not reproduce the suggested idle regression: both groups'
p50/p95/p99 improve against both larger controls. Group 2 maxima are 78.7 / 58.0 /
61.5 / 77.7 ms in table order. Read-only probes still exercise FUSE OPEN/CLOSE
metadata; they are not bare-device read latency or native GET-only measurements.

The actual-core matrix is repeated twice on SATA and NVMe, with one/two groups,
width one/sixteen, batching off/on: 32 cases. Eligible 16-entry cohorts reduce
2,560 actual `fdatasync` calls to 160. In each two-group case, group 0 and group 2
individually have 1,280 records and 1,280 → 80 barriers. Every callback verifies
its ACK index against its own group's durable coverage. Every record's term and
payload is verified after disk reopen. Single-entry controls remain one barrier
per record; no gain is claimed without eligible concurrent work. Retained
`by_group` fields report each group's readiness-to-ACK p50/p95/p99. Batch rates
rise roughly 6.5–8 times in these controlled queue-ready bursts; these are not
end-to-end file-create rates. The original matrix is preserved separately.

W93 closes with the existing AMD acceptance, final NUC safety suite, repeated
sustained NUC gains, per-group durability evidence, reported read tails,
structural queue/apply bounds, sampled RSS/lag and exact cold checks. Rates use
the final 900-second sample; operation latency includes the small bounded loader
drain tail. Observation log windows and perf enable/disable acknowledgements
retain their exact timing offsets. Maximum sampled process RSS across ABBA is
2,125.9 MiB. Largest sequential-probe replica commit spreads are 181/group 0 and
39/group 2 entries; these are observed values, not continuous lag ceilings.
The final larger idle repeat and another 512 cold checks pass. Final live server
hashes match the installed server, four direct/TCP daemons and both mounts are up,
and no loader/profiler remains. All final acceptance exit records are zero;
original failures and expected negative-control exits remain retained.

This closes this item's persistence/batching acceptance, not a hardware ceiling,
universal optimum, or the separate W53/W94/W97 obligations. W97 pressure/refusal
work remains visible; no dropped-frame elimination is claimed. Earlier-credit
and group-drain experiments are evaluated/rejected for adoption and stay off.
The accepted bundle is opt-in (`EFS_RAFT_APPLIED_INTERVAL_MS=100`,
`EFS_RAFT_BATCH_OVERLAP=1`, `EFS_RAFT_BATCH_NOOP=1`); it is not a global-default
change for untested deployments. Clean shutdown is required before downgrading
to binaries that ignore the atomic recovery cursor.


## Concurrent final branch integration

After the completed acceptance above, the last ancestry audit found concurrent
`devel`/`devel-intel`, query-cache and W66 commits. These are merged at frozen
engine `cdc5c26539e9`, preserving the W93 repairs and both test dependencies.
`merge-closure-tips.json` records this final boundary. Four-voter handling,
namespace/GETCHUNKS retries and query revision persistence are independently
changed; current production topology remains the existing two three-voter groups.
The earlier acceptance remains attributed to `5b2df4bd32be`. Final integrated
native/live gates and repeated sustained NUC revalidation are retained under
`results/measure/20261010-w93-integrated/`; do not represent the old evidence as
execution of this new build. NUC stayed on the accepted old build until the
new isolated safety gates passed; it now runs the frozen repair below for
sustained revalidation. No moving concurrent branch is silently chased
past this final captured integration boundary.

### Integrated receipt-window admission repair

The first integrated `cdc5c26539e9` full live run passes the sync/APPEND/dirty-stop
and shutdown gates but fails `concurrent_appends`: POSIX 211/212, EAGAIN. Keep
`initial-cdc5c265/full100.log` and private fixture `gc-direct-hg1fadwi`.
The concurrent file has 57 accepted eight-byte reservations rather than 200;
its adjacent two-process case accepts all 120 records. Twelve client admission
messages retain the oldest unresolved identities (3828 then 3846) and a full
16-position receipt window, sometimes with only one live owner. Later completed
positions cannot be acknowledged until that oldest owner finishes. Increasing
the receipt limit or advancing ACK past an unresolved identity is not a fix.

`37ad6a87` waits for identity admission before any reservation RPC, outside the
append stripe, retrying within the inherited write deadline. Success reacquires
the stripe within that same deadline; a failed reacquisition retires only the
new, never-submitted identity. Generic identity admission now respects an expired
caller deadline even inside its one-second attempt. No partial write is accepted,
unknown operation is replaced, or callback budget extended. Saturation can still
fail honestly when the full deadline expires. This prevents immediate one-second
EAGAIN and avoids making an older operation wait behind the admission lock.

Actual-source tests exercise a window remaining blocked beyond one second then
recovering, unlocked admission, deadline/exhaustion refusal, identity and stripe
cleanup, and a 20 ms inherited deadline inside saturated generic admission.
The unchanged-source negative controls fail the lock and elapsed-time assertions.
Existing retained CREATE/lease/ACK and 800,000 concurrent identity checks pass.
Integrated live and performance acceptance completes below; the older-engine
acceptance remains separate.

### Integrated safety and deployment

Frozen engine `37ad6a8790c8-integration` passes the rebuilt full native suite
and all five documentation checks. Source hashes and all five binary versions /
hashes are retained; server/FUSE HELLO build identifiers are checked separately
from human `--version` text. Every one of the final 16 captured tips is an
ancestor. No additional moving tip is chased after this boundary.

All three complete private configurations pass POSIX 212/212 and peer 64/64:
100 ms baseline cohorts (70.7 / 190.0 seconds for POSIX / peer), 100 ms overlap /
no-op (60.1 / 34.5), and unchanged 10 ms defaults (65.5 / 44.9). These functional
run durations are retained, not used as a controlled speed comparison.
Each also passes sync/APPEND, dirty-stop, shutdown-mutation, physical GC and
corrupt-WAL/second-cold-crash checks. Partition/publication/session/voter-change
acceptance passes. At the maximum 1,000 ms cadence, KV-WAL and retained-marker
EIO/crash/offline-metadata tests pass both with baseline cohorts and with the
accepted overlap/no-op switches together. The repeated per-group SATA/NVMe
core matrix also passes: all 32 cases, exact cold records and own-group ACK
watermarks. Every final isolated safety exit is zero; the original cdc failure
and expected old-source negative exits remain separate.

Both service mounts and all old daemons drained before installation. Preserved
stores required four failed readiness attempts before the fifth completed;
already-started processes were retained. Four repaired-build daemons and both mounts are ready, direct IO / TCP,
commands off. No timed loader existed during failed startup readiness attempts.
Sustained ABBA revalidation completes on this exact source, with fresh workload
directories and the same monitoring / headroom guards as the earlier acceptance.
The larger matched idle matrix follows those four 900-second cells. Default
restoration and exact cold checks pass; results follow.

### Integrated sustained comparison — 37ad6a87

All four fresh-directory 900-second cells complete, drain, and catch every
replica up to its group's final leader cut. No worker error, cap hit or Raft
sync error occurs. The workload, monitoring, headroom guards and ABBA order
are unchanged from the first acceptance; executable source is now the final
merged engine. Preparation, restart and idle probes remain outside timed work.

| Cell | Files/s | 0–5 / 5–10 / 10–15 min files/s | Operation p50 / p95 / p99, ms | Mixed-read p99 group 0 / 2, ms |
|---|---:|---|---|---|
| Control 1, 10 ms | 60.67 | 76.73 / 52.43 / 52.85 | 94.54 / 352.08 / 468.99 | 212.73 / 469.99 |
| Candidate 1, 100 ms + overlap/no-op | 83.18 | 96.04 / 72.99 / 80.50 | 65.60 / 284.73 / 394.76 | 186.11 / 406.15 |
| Candidate 2, same | 75.06 | 91.77 / 66.83 / 66.57 | 77.32 / 274.21 / 351.86 | 210.00 / 351.65 |
| Control 2, 10 ms | 60.54 | 76.20 / 53.06 / 52.37 | 94.29 / 352.06 / 462.26 | 207.02 / 430.65 |

Candidate mean is **79.12 vs 60.61 files/s, +30.54%**. Both candidates beat
both controls, including settled rate windows. Operation p50/p95/p99 improve;
short mixed-read p99 is generally lower, but candidate 2 group 0 is 210.00 ms
versus final control 207.02 ms. Candidate 1 retains an operation maximum of
1.077 seconds and group 2 mixed-read maximum of 1.010 seconds, versus controls'
0.801 / 0.819 second operation maxima and 0.500 / 0.481 second group 2 read
maxima. Do not hide these outliers or overinterpret 181 mixed reads/group/cell.
This remains a bundled, local workload comparison, not separate attribution of
switches or a universal gain.

n3 entries/sync in ABBA order are **4.51 / 6.72 / 5.87 / 4.50**; n4 is
1.074 / 1.148 / 1.149 / 1.076. n3 applied-marker phase time is
146.9 / 76.9 / 75.1 / 145.6 seconds, and n4 is
213.9 / 45.5 / 42.4 / 214.3. Actual sync syscall windows, all per-group and
worker latencies, background GC and host block deltas remain in the raw bundle.
The fewer marker barriers and broader eligible cohorts retain ACK coverage;
these timings are evidence of reduced persistence work, not drive physics.

Maximum sampled process RSS across these cells is **2,359.7 MiB**. Observed
local commit-minus-applied is zero; largest sequential replica commit spreads
are 143/group 0 and 58/group 2 entries, not continuous ceilings. Inbox bounds
remain 256 frames/32 MiB; atomic apply bounds remain 32,768 keys/64 MiB.
n3 refuses 27,936 / 30,414 / 29,795 / 28,002 inbox admissions. Retried pressure
is still material; W97 remains separate. n4 refusals are 11 / 17 / 0 / 5.
Memory/resource and background work evidence is retained, not an assertion of
constant total RSS with an arbitrarily growing namespace.

### Integrated final acceptance

The matched larger idle matrix completes on the same binaries, with startup /
warm-up excluded and 6,000 exact immutable-payload reads per group per case.

| Settings | Group 0 p50 / p95 / p99, ms | Group 2 p50 / p95 / p99, ms |
|---|---|---|
| Default 10 ms, control 1 | 6.57 / 11.53 / 17.47 | 8.12 / 12.57 / 41.80 |
| 100 ms cadence only | 6.08 / 10.48 / 16.03 | 6.49 / 10.12 / 35.14 |
| 100 ms + overlap/no-op | 6.12 / 10.56 / 15.24 | 6.53 / 10.24 / 35.03 |
| Default 10 ms, control 2 | 6.43 / 10.78 / 16.83 | 8.21 / 13.39 / 38.92 |

Both groups' full-bundle p50/p95/p99 improve against both controls. Group 0
maxima in table order are 57.4 / 50.5 / 53.8 / 66.1 ms; group 2 maxima are
115.2 / 66.0 / 86.3 / 68.3 ms. The bundle's group 2 maximum exceeds the final
control despite lower percentiles; retain it. This comparison does not show
an idle percentile regression hidden by throughput averages. Reads include
FUSE OPEN/read/CLOSE metadata, not raw device IO.

All final native, functional, partition, maximum-cadence fault, core matrix,
sustained, idle and deployment exit records are zero. The first cold restoration
and the post-idle check each verify all **512 sampled first/last payloads**
through both mounts. Final verification at **2026-10-10 11:30:26 UTC** proves
four daemons and two FUSE clients match the installed merged executable hashes;
TCP/direct IO, both mounts up, FLOW=1, 10 ms applied cadence, commands / batch
diagnostics / overlap / no-op / group drain / early retirement off. No owned
loader or profiler remains. Original failures and expected old-source negative
exits remain retained and are not counted as passes.

Raw integrated controls, versions, source/controller hashes, all 32 per-group
core cases, four ABBA cells, four larger idle cases, fixture logs / metadata and
final state are retained in `results/measure/20261010-w93-integrated/`, with an
artifact hash manifest. Executable source is the frozen `37ad6a8790c8` commit;
`4a962397` adds active-FUSE restoration hash checks outside timed workloads.
All 16 final captured tips are ancestors. Other checkouts and branch pointers
are preserved; concurrent tips after this captured boundary are not silently
represented as tested.

**W93 is closed on the final integrated build.** The opt-in tuning bundle,
clean-downgrade rule, sample/outlier caveats and separate W53/W94/W97 obligations
remain as stated above. This acceptance establishes neither a hardware ceiling
nor physical power-loss or distributed-RDMA guarantees.

**Raw evidence note (2026-10-23):** the bulk data in `w93-closure-20261010` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
