# W93/W97 read latency and credit retirement — October 9, 2026

User-authorized hour starts 10:44 pm EDT October 9 (02:44 UTC October 10).
Isolated `devel-persistence-pressure` worktree, NUC only, TCP/direct I/O,
command service disabled. Main devel and Xorinox are untouched; existing
service stores and test payloads are preserved.

## Revisit the read-latency warning

The prior A/B used a different newly-created read-probe inode per case and
only eight idle / sixteen mixed samples per group. Those readings cannot
attribute a latency difference solely to overlap batching. This hour uses
both previously-created probe sets in every native phase trial, verifies their
inode-group mapping, warms each 32 times, and runs overlap off/on/off for
90 seconds after graceful restarts of the identical ca810bac build. FLOW=1,
NOOP=1, GROUP_DRAIN=0, OBS=1 in all three. No compiler, profiler or other
workload overlaps these timed cells. `os.open` → `os.pread` → `os.close`
records individual phases; a further 1,000 held-descriptor reads per probe
separates data reads from repeated opens. Payload equality is checked always.

| Native complete operation, ms | Off 1 | On | Off 2 |
| --- | ---: | ---: | ---: |
| Group 0 p50 | 6.428 | 6.623 | 6.637 |
| Group 0 p99 | 13.246 | 13.014 | 13.166 |
| Group 2 p50 | 7.835 | 7.806 | 7.669 |
| Group 2 p99 | 15.606 | 17.365 | 15.793 |
| Samples per group | 6,204 | 6,058 | 6,136 |

The large group 0 idle separation is not reproduced by these native phases;
on lies within the off-control median variation and its p99 is lower. Group 2
has a modest on-case tail increase; repeatability is not established. Native
close p50 is 12–13 microseconds: asynchronous release means it is not the
measurement of durable lease cleanup. Open p50 is approximately 3.4 ms on
group 0 / 2.3 ms on group 2, data read p50 approximately 3.0 / 4.9 ms. Held
reads are generally faster, with individual probe medians around 0.8–1.8 ms.

This phase experiment deliberately uses native open/pread; it is not identical
to Python `Path.read_bytes`, which may perform additional file metadata/EOF
calls. Therefore it does not dismiss the original mixed-load warning. The
credit comparisons keep the exact same original baseline probe paths in every
cell and use the original Python API, with a read pair every five seconds.
Every sample records its path. Probe calls are observation load common to all
modes; sparse mixed tails still cannot certify rare-event acceptance.

## Separate profiling, before the credit fix

A separate 30-second native-read workload records cpu-clock samples on four
service daemons and two FUSE clients, followed by a separate 12-second strace
window on n3/n4 during another read workload. Profiled rates/latencies are not
performance acceptance data. Perf reports 979 samples, zero lost, approximate
9.89 CPU-seconds across the attached processes in 30 wall seconds, not a
single-core saturation measurement. Self samples are scattered through host
wakeups, KV lookup/hash work and protocol/kernel paths; no dominant compute
routine is established by this short sample.

Strace aggregate syscall wall time across n3/n4 threads is 81.32 seconds:
futex 33.72, poll 28.86, fdatasync 17.39 (11,220 calls), pread64 1.22, pwrite64
0.13. These sums include waiting and multiple concurrent threads. Futex
errors include expected wait races; neither these percentages nor sync elapsed
prove device busy time, a hardware ceiling or the cause of the prior mixed
read warning. Raw perf stays on NUC with checksum; decoded report and strace
summary are retained locally.

## Implemented bounded credit-return candidate

Source commit `ea23775b` adds default-off `EFS_RAFT_FLOW_EARLY_RETIRE=1`.
The old pump retains consumed inbox credits through both KV release and
applied-marker persistence. The candidate returns consumed credits after KV
release and before the independent marker tail. Raft processing/apply is done,
its frame buffers are freed, and the KV barrier has returned. New incoming
frames keep their own credits; only the completed batch count/bytes retire.
This permits bounded transport admission while the checkpoint runs, without
moving Raft ACKs, applying new work or weakening the KV-before-marker rule.

The same 256-frame / 32-MiB hard inbox limits, 64-frame / 8-MiB pressure
thresholds, low-water hysteresis and control reserve apply. Credit return is
not a consensus/durability ACK. Failed KV release still forbids marker advance;
retirement after failure preserves the existing completion accounting. Failed
marker writes retain old saved indices. Shutdown still uses the original
forced successful-KV prerequisite. No disk format or checkpoint interval changes.

Actual-source regressions exercise early/late callback ordering, arrivals during
the marker tail, count/byte preservation, no-flow/no-work cases, KV failure and
failure of either marker. Linux core/store/wire, classifier, checkpoint and
async flow-channel/BUSY/legacy/lost-reply regressions pass. The new counter
`flow_retire_early` verifies that the measured path actually executes.

## Append/truncate defect found by the functional gate

The initial `ea23775b-read-pressure` candidate passes POSIX 212/212 but fails
one of 64 peer tests: `peer_append_while_truncate` returns EIO. Its client log
shows `fetch published ... rc=-9 then pull rc=0 rc=-9` and a retained
base-generation-zero, one-span chunk row. Twenty-five isolated retries pass;
the race is intermittent and this does not establish a credit-order regression.
Performance trials stop at the failing functional gate. The original RPC
response body was not captured; linking that EIO specifically to an empty
GETCHUNKS reply is an inference, not a wire-trace proof.

Code review identifies a real recovery defect independent of credit admission:
GETCHUNKS can successfully return no row after a peer truncate, while the local
staging table deliberately retains an earlier map. The published-fetch retry
ignores authoritative absence and decodes that obsolete map again. Commit
`437b7890` uses the existing explicit absence bitmap for recovery; confirmed
absence returns a zero image with no published observation. It does not delete
local staging, treat failed RPC/object fetches as holes or suppress decode
failure when a published row remains. Fresh publication still uses its existing
CAS/content-epoch authority; this change grants no unfenced publication rights.

Actual-source captured-view regressions cover retained old/staging rows,
zero bytes and cleared observation, optional observation pointers, RPC BUSY
propagation and a genuinely failed decode with a still-present row. Existing
mixed-age fence reads and both pipeline/direct writer-merge paths also pass.
The same regression fails against the pre-fix actual helper and passes under
ASan/UBSan with the fix. The corrected build must rerun full suites and repeated append/truncate tests
before controlled performance comparisons. Original failed logs are preserved.

## Frozen comparison and final gates

Build `437b7890-read-pressure`, isolated `/data1/efs/w93-read-build`, is used
for every late/early/late cell. FLOW=1, OVERLAP=1, NOOP=1, GROUP_DRAIN=0,
OBS=1, COMMANDS_SERVER=0; only EARLY_RETIRE changes. The first two cells last 240
seconds; the reverse control is shortened to 180 seconds to reserve time for
recovery gates after the startup timeout. Common rate/read comparisons use the
initial 180 seconds, with full-interval tails reported separately. Each uses four deterministic tiny-file writers per inode group, fresh
output roots, 32,000 files/worker cap, bounded exact latency arrays and
three equal-duration window rates (80 seconds, or 60 for the reverse control).
Preparation is excluded. Normal SIGUSR1 drain
and replica coverage of a sampled leader cut precede restart. Cap hits would
invalidate unclipped performance acceptance. Background command mix and serial
phase deltas must be reported rather than assuming queues have settled.

Corrected full POSIX is 212/212 (63.2 s), full peer 64/64 (169.9 s).
All twenty corrected isolated append/truncate runs also pass.
All three rate trials and 384 independent first/last exact payload reads through
two cold service mounts pass their functional checks. Private fault gates and final restored-service verification pass. Raw evidence
is `results/measure/20261009-w93-read-pressure-hour/`. No W93/W97 closure or
hardware-ceiling claim is made by the guarded implementation alone.

## Readiness delay and bounded measurement controller

The early cell stops before generating a workload because the preserved-store
n3 cold start takes 33.5 seconds to reach its new listening line while devops
waits only 30 seconds. The process remains alive and joined; the third
idempotent start invocation completes n4 and mounts without replacing stores
or processes. Failed startup evidence is retained as `early-start-failed`.
The measurement controller now permits five bounded idempotent start attempts
with five-second spacing; production devops source is unchanged. A configurable
readiness budget or live readiness polling remains a devops follow-up. This
extra quiet startup interval is another reason not to infer causal gains from
one short pair, even with fixed source/probes. The resumed early cell verifies
all process flags and build checksums before its timer begins.

## First full-duration pressure pair

| 240-second cell | Late credits | Early credits |
| --- | ---: | ---: |
| Mean files/s | 153.50 | 149.05 |
| Three 80-second rate windows, files/s | 211.7 / 125.0 / 123.8 | 203.3 / 118.9 / 124.9 |
| Completed operation p99, ms | 334.44 | 342.37 |
| Group 0 mixed read p50 / p99, ms | 55.56 / 157.19 | 71.09 / 150.61 |
| Group 2 mixed read p50 / p99, ms | 330.90 / 569.05 | 370.22 / 590.24 |
| n3 refused inbox frames | 8,092 | 7,724 |
| n3 entries per WAL sync | 11.48 | 10.65 |
| n4 entries per WAL sync | 1.31 | 1.30 |

Both complete without worker or sync errors, hit no worker cap, drain normally
and cover the sampled leader cut on all hosted replicas. Early-retire counters
are actually active (n3 6,569, n4 18,359); they are zero in late mode. This
candidate is 2.9% slower across the pair and does not meet performance adoption
criteria. Lower refused-frame count alone is not progress in sustained completion.
Read p99 is the maximum of only 49 mixed samples per group, not certification
of a rare-event tail; the read medians are worse with the candidate. Both initial
leader snapshots are n1/group 0 and n2/group 2; recorded terms change on restart.

n3 serial pump/KV/marker diagnostic totals are 140.2/56.2/28.3 seconds late,
145.2/57.2/29.0 early; n4 totals are 94.2/84.0/59.0 late and 90.9/85.9/61.3
early. These per-process phase sums do not identify device busy time. Returning
credits sooner does not remove the serialized persistence barriers. Safe bounded
checkpoint coalescing/overlap remains a separate unimplemented W93 design gate;
no sync may simply be omitted. Four foreground metadata commands per tiny file
remain visible; W94 lease-adoption prerequisites remain independent.

The return control and final cold/fault/restored-service verdicts follow below.

## Return control and decision

All three cells complete without worker/sync errors or cap hits, drain normally,
and all hosted replicas cover their sampled leader cuts. The comparison below
uses the common initial 180 seconds (37 mixed read samples per group); full
240/240/180-second operation distributions remain separate in raw summaries.

| Common first 180 seconds | Late 1 | Early | Late 2 |
| --- | ---: | ---: | ---: |
| Files/s | 163.65 | 157.22 | 168.15 |
| Group 0 files/s | 141.94 | 136.37 | 147.07 |
| Group 2 files/s | 21.71 | 20.85 | 21.07 |
| Group 0 read p50, ms | 45.71 | 71.14 | 61.32 |
| Group 0 read p99, ms | 157.19 | 150.61 | 146.05 |
| Group 2 read p50, ms | 322.36 | 357.70 | 307.46 |
| Group 2 read p99, ms | 569.05 | 590.24 | 477.72 |

Early credits are 3.9% slower than the first control and 6.5% slower than the
return control. Sequential device/namespace state and the extra startup wait
prevent causal attribution of a regression, but there is no demonstrated gain
and no basis for default enablement. The option remains default off and the
restored runtime must set it off. Refused frames are transport admission events,
not evidence of lost files. Initial controls and candidate preserve the same
leader placement; command mixes, persisted state and startup spacing are still
not identical experimental reset fixtures.

Sampled server RSS in the full pair is approximately 0.75–1.72 GiB per process,
with a roughly 20-MiB loader; this is sampled resident memory, not peak or a
whole-system hard-memory proof. Queue-age means are n3 75.97/77.51 ms and n4
13.10/12.00 ms late/early. Count/byte admission limits remain enforced by source
and actual-host regressions. Neither throughput, CPU samples nor sync elapsed
establishes a hardware ceiling. W93/W97 remain open; prioritize measured serial
persistence-tail reduction with an explicit bounded checkpoint/recovery contract
and W94's independent ownership-fenced metadata reduction.

## Final validation

All 384 first/last exact payload checks through both cold mounts pass after a
graceful preserved-store restart with NOOP/OVERLAP/EARLY_RETIRE/OBS off. Full
functional suites and repeated append/truncate verdicts are recorded above.
The first private fault invocation exits before stopping service because its
stop-helper script was missing; that controller error is preserved separately.
The helper is added and the private fixture is rerun without replacing the
original failure record. This is a measurement-harness omission, not an EFS
fault verdict or a reason to discard pending production writes.

Private direct-I/O/TCP gate returns zero with early credits, overlap and no-op
cohort options enabled on all four actual test daemons (process options retained).
It covers synchronous writes and injected publication faults, append sync
boundary/replay races, dirty-stop recovery, shutdown mutations, invalid WAL-tail
repair, staged/committed deletion, fourth-member ledger recovery and physical
quota/fragment reclamation. Final `GC reclamation direct PASS` and zero exit are
retained. Fixture manifest hashes the exact source tree, test script and daemon,
FUSE and management binaries; JSON verdicts and per-node/client logs are copied.
This is process-crash/injected-fault coverage, not physical power-loss evidence.

The preserved service is restored and verified: four installed `437b7890`
daemons respond with both Raft groups, both normal mounts are active, both
clients have commands opted out, TCP/direct I/O remains configured. FLOW=1;
NOOP/OVERLAP/GROUP_DRAIN/EARLY_RETIRE/OBS and COMMANDS_SERVER are all zero.
No loader, perf or strace remains active. Exact PID/options, binary checksums,
Raft status, time and free-space output are in `final-service-state.json`.
Existing user data and all completed benchmark payload trees are preserved.
Work finished approximately 62 minutes after the authorized start; the extra
time completes recovery validation and safe service restoration.

Focused source commits are `ea23775b` (default-off credit experiment) and
`437b7890` (confirmed-absence recovery fix); a separate documentation/evidence
commit records the final results. Main devel and Xorinox remain untouched by
this work. W93/W97 are open, and experimental credit return is not promoted.
