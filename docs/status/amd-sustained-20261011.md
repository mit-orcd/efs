# AMD sustained tiny/large service gate — Oct 11

**Current disposition:** The later [unpaced admission acceptance](amd-overload-admission-20261011.md)
closes this AMD overload/admission follow-up, including exact-artifact retained
service deployment. Earlier failures below remain historical evidence. NUC
persistence attribution is separately tracked by [W111](w111-nuc-persistence-investigation-20261011.md);
W94 integration and W97 receiver feedback remain separate open scopes.

W94's finite CREATE adoption A/B and full normal/fault/direct/buffered gates
pass on the [captured integration](amd-integration-20261011.md). This follow-up
targets sustained service, without claiming full production session/PUT-ticket
integration or selecting a new storage format.

The initial plan uses the private four-node TCP fixture for direct then
buffered, 600 seconds each. The actual timed round shortened follow-up runs as
recorded below; no completed 600-second gate is claimed. Eight tiny workers rotate 64 names per worker, writing 512-byte
generation-specific bodies. Every acknowledged generation is byte-checked
before unlink; names are reused only after unlink. Four large writers overlap
16-MiB rings of 1-MiB writes, fsync every four writes and at completion. The
follow-up offers 2 MiB/s per writer; the unpaced failure below is retained. Thirty-
second windows record CREATE/close/delete completions and latency, with process
CPU ticks and RSS samples. Explicit per-daemon quota is 4 GiB; installed data
and daemons are retained. This measures the accepted combined-adoption build;
it is not a new same-source A/B throughput claim.

After each timed interval, the private servers crash and restart. All 512 tiny
survivors and all four large files must match exact expected bytes. Unlink must
then produce both empty GC queues and empty physical fragment inventories.
The harness drains/restores the two installed mounts with TCP and commands off.
No RDMA, D25 activation or pushes are part of this gate.

Reproducible runner (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/run.sh`).
Acceptance and remaining blockers will be recorded after the actual verdicts.

## Unpaced mixed-load failure — retained, not closed

The initial invocation missed the fixture's required second root argument and
stopped before private nodes started. It restored both mounts; this setup
failure is retained separately. Both reproducible AMD runners now explicitly
supply two parent roots, which may share the same AMD filesystem.

The corrected unpaced run hit ENOSPC at a large writer's fsync/close about one
minute after starting. Explicit per-node logical fragment quota was 4 GiB;
allocated fragment files approached 4.1–4.2 GiB per node including checksum and
allocation overhead. The host still had 389 GiB available. Four logical large
files occupy at most 64 MiB; this is overwritten-generation/GC pressure, not a
full host filesystem. PUT counters reached roughly 5 GB per node and recorded
quota failures. The 30-second sample completed 2,245 tiny closes, before the
failure. It is not a sustained throughput or cold-byte acceptance result.
Failed run (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/evidence/unpaced/direct.log`).

A separate 60-second cold restart without writers enables actual GC identity,
delete and ACK diagnostics. Per-node sampled allocation falls from
4,484,579,328/4,382,011,392/4,466,475,008/4,404,641,792 bytes to
3,217,346,560/3,019,173,888/3,202,654,208/3,223,613,440. Node 1 removes 18,274
fragments, reports 1,197,604,864 logical payload bytes reclaimed and zero final
delete/checksum/sweep/reap/ACK/scan errors. Committed GC ACKs succeed. This
establishes progress under quiescence, not complete drain, a proven leak cause,
or exact acknowledged-byte recovery of the failed run. Concurrent inventory
samples explicitly count files that disappeared between scan and stat; the
first inventory-race audit attempt is also retained.
Cold recovery observations (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/evidence/unpaced/cold-gc-recovery.json`).

The unpaced offered load exceeds the observed quiescent reclaim rate. Source
uses bounded GC scans/ACKs, a 200-ms fragment budget per group and one-second
loop; increasing these or adding pressure admission needs fairness/durability
acceptance. No unsafe GC deletion, timeout increase or quota inflation is
adopted. The new 2-MiB/s-per-writer run is a distinct bounded-service gate;
pacing does not repair or close this overload behavior. W94 full sustained
acceptance and W97 pressure/reclamation work remain open.



## Opt-in GC scheduling candidate

`EFS_GC_BACKLOG_IDLE_MS=100` is an experimental scheduling candidate; unset
remains the existing 1,000-ms idle. Strict startup validation accepts only
100..1000 milliseconds. The initial candidate uses a shorter idle only after a leader's capped
reap scan and observed physical deletion progress during that pass. The
fragment-backlog follow-up below extends that scheduling signal. Idle,
wholly blocked work and counter reset retain 1-second backoff. Partially blocked
work can accelerate when other deletions succeed; live offline-member gates
must preserve all-member retention. All group scan,
fragment/ACK budgets and 20-ms snapshot/shutdown slices remain unchanged; no
collection authority, mutation ordering, quota or durability rule changes.
The actual-source regression covers default, malformed/range configuration,
idle/blocked/reset and progress cases. Full AMD native and candidate live gates
are required before promotion; this round does not change the default.

## Timed-round scope adjustment

The 2-MiB/s-per-writer baseline is intentionally interrupted after five minutes
so the opt-in scheduling candidate can receive native and live validation in
the user's 30-minute round. It retains interval counts and final GC/quota
status, but has no cold-byte acceptance oracle and is not a passed sustained
run. The candidate is scheduled for full normal/fault native suites, direct
POSIX/peer and guarded-lease/GC recovery, then 180-second mixed-load gates in
both direct and buffered modes with cold survivors and physical drain. The
shorter candidate load does not satisfy the originally planned ten-minute
or full sustained W94/W97 obligations; neither workload rate nor quota is
changed between the bounded baseline and candidate. Default idle is unchanged.

The retained bounded baseline observed 300 seconds: 52,464 completed closes in
ten full 30-second windows (174.88/s), with final per-node charged usage
3,084,189,696/2,759,720,960/3,084,713,984/2,760,179,712 bytes. It stopped by
cooperative SIGINT, exit 130, and restoration exit 0. This is an intentional
diagnostic stop, not a storage failure or cold-byte acceptance result.
Interval/final-status evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/evidence/baseline/baseline-stop.json`).

## Final first-candidate verdict

Candidate `79a3ef38` passes full normal/fault native suites and direct TCP
functional acceptance (216 core, 67 peer, guarded leases and cold recovery).
The 180-second paced direct load warm-verifies 26,688 tiny generations and
passes exact cold survivor checks, but fails the unchanged 180-second physical
drain deadline. Buffered was not run after that failure. The old harness did
not persist the cold-phase marker before draining; it now records each phase
independently. No throughput improvement or sustained acceptance is claimed.

An additional cold/no-writer diagnostic exits zero: all four physical fragment
inventories reach zero files/allocated bytes at the 30-second sample and remain
empty through 60 seconds. This proves eventual reclamation of this retained
fixture, not a pass of its original deadline. Installed service restoration
passes independently. W94/W97 remain open; the default idle is unchanged.
Recovery evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/evidence/candidate/live/cold-gc-recovery.json`).

## Fragment-backlog scheduling follow-up

The first candidate considered only capped inode REAP scans. Repeated writes
to live ring files instead create fragment-GC records, so that work could
exhaust its 200-ms group budget without requesting the configured shorter idle.
The follow-up includes a full fragment batch with pending records and successful
ACK progress in the backlog signal. Physical deletion progress is still required
by the idle policy; blocked/idle scans keep their backoff. Default scheduling,
per-group budgets, authority and durable ACK ordering remain unchanged. This
candidate is under normal/fault and direct/buffered live validation, not closed.

## All-member inode-fence fanout candidate

Post-load stage timings on the fragment-backlog build show a roughly 221-ms
reap pass with about 188–192 ms in inode fencing for a 32-marker batch. The
leader sends and waits for each storage member serially. The next candidate
sends independent remote requests first, installs the local fence, and drains
every borrowed connection before returning. Every expected member must still
report success; absent members, BUSY, lost replies, malformed responses and
local persistence failures retain the durable REAP record. Storage inventory
and durable death-fence implementations are unchanged. The actual-source
regression checks request ordering and failure cleanup; full native/live
acceptance is pending. This does not enable RDMA or change GC authority.

## Fragment scheduling build `85fb3a4b` verdict

Full normal/fault native gates pass. The normal build is deployed to the four
installed AMD direct/TCP nodes, retaining their stores and default GC idle;
both commands-off clients remount successfully. The private opt-in direct
functional gate passes 216 core/67 peer tests, guarded leases, missing-member
retention and cold/physical recovery.

| 180-second paced mode | Warm-verified tiny generations | Full-window closes/s | Exact cold survivors | Empty inventory by 180-second drain deadline |
|---|---:|---:|---|---|
| Direct | 26,944 | 147.43 | Pass | **Fail** |
| Buffered | 20,224 | 111.66 | Pass | Pass |

Both runs use eight tiny workers/four overwrite rings and the same explicit
4-GiB/node quota and 2-MiB/s/large-writer offered rate. Independent roots and
placement make these acceptance observations, not a matched performance A/B.
Both installed mounts restore successfully. The conditional ten-minute run
is not started because direct failed; fanout validation follows without
overlapping load or compilation. W94/W97 remain open.
Retained verdicts (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/evidence/fragments/live/round.exit`).

Rejected setup/build attempts are retained alongside accepted gates: an aborted
incorrect build label, a stale generated-doc upload, missing linked evidence,
and the first candidate's missing C return. None was deployed; `85fb3a4b`
corrects the return and passes the actual-source regression/full gates.

## Fanout build `e9ea8ca1` — short gates accepted

Both complete native modes pass; the normal build is deployed on the retained
installed stores. Direct functional acceptance passes 216 core/67 peer tests,
including offline-member ledger retention, guarded recovery and late-PUT fences.
Both private 180-second paced workloads pass exact cold bytes and physical
reclamation within the unchanged deadline.

| Build / mode | Warm-verified generations | Full-window closes/s | Mean window close p99 (ms) | Physical drain |
|---|---:|---:|---:|---|
| Fragment scheduling / direct | 26,944 | 147.43 | 156.83 | Fail at 180-s deadline |
| Fragment scheduling / buffered | 20,224 | 111.66 | 319.04 | Pass; elapsed not captured |
| Fanout / direct | 25,280 | 137.86 | 181.32 | Pass, 87.69 s |
| Fanout / buffered | 25,152 | 139.36 | 174.03 | Pass, 92.57 s |

Each passed cold check covers 512 tiny survivors and four 16-MiB large files.
These are independent acceptance cells, not matched A/B: the direct observed
throughput/tail is worse and buffered better, so no universal throughput gain
is claimed. Post-load sampled fencing passes fall from about 188–192 ms to
93–104 ms; batch and placement context still matter. The aggregate sampled
four-daemon/FUSE CPU is about 0.93 cores and peak aggregate RSS about 309–315
MiB on the two fanout loads; workload-driver CPU is excluded.
Reconciled raw-field summary (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/followup-summary.json`) /
reproduction (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/summarize-followup.py`).

The subsequent ten-minute direct/buffered tests retained
the same rate, quota, survivor oracle and 180-second drain deadline. No complete
sustained-service or unpaced-overload acceptance is claimed yet. Installed
service keeps the default idle; private acceptance uses explicit 100 ms.
Exclusive AMD queue (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/fanout-queue.sh`)
may restart the four installed daemons while retaining stores;
long runner (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/fanout-long.sh`) borrows
and restores the two mounts. W94/W97 remain open.

## Ten-minute direct failure and 20-ms-yield follow-up

Fanout's 600-second direct workload completes 65,664 warm-verified generations
and exact cold survivors, but physical drain times out after 180.20 seconds.
The longer backlog therefore remains an open gate, despite both short passes.
Buffered also fails its physical deadline. The opt-in parser now admits 20..1000 ms, matching
the existing 20-ms snapshot/shutdown slice. The next private candidate uses
20 ms only for capped, progressing GC; empty/blocked scans retain 1 second.
Default scheduling and all work/ACK/authority budgets remain unchanged.
This needs full native/direct functional and both 600-second cold/physical
gates, with latency/CPU review; it is not unpaced-overload admission or closure.

The 20-ms follow-up also times the existing warm tiny reads, without adding
I/O. Every generation must still byte-verify before retirement. Read-window
quantiles are new instrumentation; older cells have no comparable read-tail
field, so they cannot establish a matched read-latency gain.

Both 100-ms ten-minute modes now have final verdicts: direct warm-verifies
65,664 generations and buffered 62,016; both exact cold checks pass, but drain
times out at 180.20/180.21 seconds. Original service restores with exit zero.
The 20-ms candidate `715b1103` passes full normal/fault native and direct
functional acceptance; both ten-minute modes now pass cold bytes and physical reclamation (see final verdict below). Warm-read timing
includes open/read/close, and adds no I/O to the existing oracle.

Final deployment restored canonical `/data1/efs/logs/efsd-n*.log` and PID
files used by the portal/devops, plus the guarded PID identities under each
store's `log/`. Earlier manual launches used dedicated deployment logs and
left the legacy PID files stale; those processes were replaced through guarded
port ownership checks, never through stale PID files. Both mounts remain TCP
and commands off. Standard AMD restart defaults are aligned with that
profile; no RDMA test or global scheduling default is activated.

## One-slice candidate: both ten-minute gates passed

`715b1103` with explicit 20-ms progressing-backlog yield completes 62,720
warm-verified generations in the 600-second direct workload. Cold restart
verifies all 512 retained tiny files and four 16-MiB large files; physical
fragment inventories and GC queues empty in 79.23 seconds, within the unchanged
180-second deadline. Buffered completes 52,800 verified generations and drains in 77.94 seconds. This closes
the bounded direct acceptance cell, not W97's unpaced overload/admission issue
or W94's full data-plane integration. The installed normal service retains
the 1-second scheduling default.

| 600-s candidate / mode | Warm-verified generations | Full-window closes/s | Mean window close p99 (ms) | Mean window read p99 (ms) | Physical drain |
|---|---:|---:|---:|---:|---:|
| 100-ms fanout / direct | 65,664 | 109.15 | See raw summary | Not measured | Failed, 180.20 s |
| 100-ms fanout / buffered | 62,016 | 102.88 | See raw summary | Not measured | Failed, 180.21 s |
| 20-ms candidate / direct | 62,720 | 103.94 | 327.79 | 85.09 | Passed, 79.23 s |
| 20-ms candidate / buffered | 52,800 | 87.48 | 473.59 | 132.54 | Passed, 77.94 s |

These independent cells establish reclamation acceptance, not a throughput
improvement: observed throughput is lower in both new cells. Quantiles are
means of 30-second window p99s, not global p99s; reads include open/read/close.
The new direct/buffered sampled four-daemon-plus-FUSE CPU is 0.79/0.70 cores
and peak aggregate RSS 548/482 MiB, excluding workload-driver CPU. Charged
usage still grows during load; bounded post-load drain does not establish a
steady-state capacity bound, unpaced admission, or fairness under overload.

Full normal/fault native and direct functional (216 core/67 peer) gates pass.
Final canonical deployment and independent restoration audit exit zero:
exactly four installed `efsd` processes, two installed FUSE processes, both
mounts accessible, canonical log/PID identities valid, direct/TCP, four metadata
voters and commands disabled. No private fixture processes remain. Installed
version is `715b1103-deploy-normal`; dedicated build source is
`/data1/efs/evidence/devel-amd-gc-yield20-20261011`, while installed binaries are under
`/data1/efs/src`. Existing stores were retained. The installed GC idle remains
the default 1000 ms; 20 ms was explicitly set only on private acceptance nodes.
Restoration audit (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/evidence/yield20/restoration-proof.json`)
and deployment identity (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-sustained/evidence/yield20/deployment/deployment.json`).

AMD restart defaults were tested with a clean environment and explicit override
checks; TCP/direct and commands-off defaults pass, while explicit overrides
remain supported. The original profile is retained with the evidence. W94/W97
remain open for their integration/overload gates. No RDMA, D25, shared-devel
change or push occurred.
