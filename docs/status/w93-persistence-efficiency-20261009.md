# W93/W97 persistence-efficiency investigation, 2026-10-09

User-authorized one-hour work started 21:02 UTC on `devel-persistence-pressure`;
NUC only, preserved service stores, TCP/direct I/O, commands disabled.

## Cohort termination and the bounded experiments

Completed W97 enabled trials show 68k–75k n3 host group-switch boundaries.
Inside active follower cohorts, behind-tail traffic ends 2,790–3,664 n3 /
7,842–7,920 n4 cohorts; empty requests end 1,063–1,137 / 1,144–1,375.
n4 has no group-switch boundaries. Raising caps cannot remove these boundaries.

Commit d11db9c7 adds `EFS_RAFT_BATCH_NOOP=1`: an existing cohort can retain
a same-leader/term, fully term-verified duplicate or empty request with no
fresh read context. It starts no new hold and appends no new bytes. Replies
remain behind the data cohort's durable watermark. New suffixes/conflicts,
configuration, changed leader/term and fresh reads use the original boundaries.
Regressions cover on/off barrier counts, read/conflict boundaries, sync failure,
no early ACK/apply and storage fencing. All existing count/byte bounds remain.

`EFS_RAFT_GROUP_DRAIN=1` groups only the initial prefix of valid, stable-leader,
noncommitting AE data traffic into stable per-group chunks (16 frames / 1 MiB
assembly bounds, existing oversized-frame handling). A follower must have
commit equal to applied, and the request cannot advance that applied prefix.
Commit/apply, fresh-read, empty/control, election/no-op, snapshot, membership
and invalid-frame boundaries retain global position and the entire remaining suffix is left
in inbox order, because such a boundary can invalidate the initial state view. This restriction preserves
cross-group metadata dependencies; merely preserving each group's FIFO is
insufficient. No extra collection delay, new queue or larger inbox is added.
Tests compile the actual ordering helper and prove FIFO, fences and bounds.
Both experiments default off; W97 flow remains an independent switch.

## Persistence tail and a prerequisite correctness fix

Across enabled 900 s W97 trials, n3 KV release occupies 239–246 s and applied
marker persistence 150–159 s; n4 occupies 282–286 s and 199–200 s. The pump
performs these phases serially after Raft drain. Same-cycle applies already
share one KV WAL hold/release; distinct Raft WAL, KV WAL and marker files
cannot acquire one durability boundary simply by omitting an fsync.

Review found that normal pump and shutdown ignored failed KV release when
advancing applied markers. Commit 0bd7b5be gates both paths on successful KV
sync and propagates marker failures, preserving old saved indexes. Actual-helper
regressions prove a failed KV barrier writes no markers in either path. A
private live EIO/cold-recovery fixture passes: `/data1/efs/efs-checkpoint-fault-ayu2b3ag`.
It restores the pre-fault cold KV image after EIO and requires the retained
durable Raft log to rebuild the exact acknowledged inode. This models unsynced
KV loss, not physical power loss. The same fixture against the pre-fix c2ab465b
binary fails as intended: applied.0 advances 4 → 5 after KV EIO. The corrected
build keeps its marker unchanged through normal pump and shutdown.

Next design options: capture a durable KV sequence/index pair and use a bounded
checkpoint worker that cannot pass it, or encode applied watermarks in the same
recoverable KV WAL. The former needs shutdown/rotation/snapshot coordination;
the latter needs explicit on-disk migration, snapshot filtering and rollback
semantics. Neither is implemented here. Extending the checkpoint interval is
not a substitute for a tested recovery contract.

## Metadata work per tiny file

Opt-in batch diagnostics now count new Raft entry command tags (rotation copies
excluded in 0bd7b5be). They measure actual append work, not unique operations,
committed verdicts or lost files; replicas must not be summed as unique client
operations. Command deltas will be reported per node and per completed file.
The server supports incorporating the initial open lease in CREATE (W94), but
the exercised FUSE call `efs_fuse_create_at` passes flags=0 to
`efs_client_create_result_inode`, then calls `efs_open_acquire`, which issues
a separate first-open HOLD. `efs_client_rpc_create` already understands
`EFS_CREATE_F_HOLD` and supplies the mount's owner token. Thus the server-side
capability is present, but this FUSE path does not use it. A concrete next
candidate is requesting HOLD on new descriptor-producing CREATE and adopting
that proven lease locally without another OPEN command. Existing-name fallback
must still acquire its own lease; ambiguous replies, local allocation failure,
unlink/open races and clean/failed close need ownership recovery gates before
deployment. This confirms the already-open [W94 adoption scope](../backlog/work-items.md#w94),
including its guarded pending ownership and W72 runtime-session prerequisites;
it is not a new justification for enabling the flag. This change is not
implemented or claimed safe here.
The newer publication protocol has replicated ADMIT, PUBLISH and receipt
retirement, but the measured ordinary tiny-file path uses legacy PUBLISH: the
ADMIT/RETIRE tags are absent. Optimizing those newer phases would not address
this workload. Any merge of the exercised publication/lease phases needs
renewed lost-reply/session/GC gates, rather than treating them as redundant.
GC/reaping and close cleanup remain separately classified background work.
Uncontended close already avoids a deferred-cleanup obligation: it performs
lease/lock/append/reclaim effects inline; a separate CLOSE_CLEANUP command is
for durable BUSY/transaction obligations, not automatically one per close.

## Validation and measurements

Candidate c2ab465b passes Linux core/store/wire, actual async sender and inbox
ordering tests; local Raft ASan passes. Both experiments enabled pass private
old-leader read/session/publication/configuration partition gates. Production
POSIX 212/212 (65.4 s), peer 64/64 (34.2 s). These precede the checkpoint guard.
The 0bd7b5be build passes Linux checkpoint/inbox/async/core/store/wire tests.
Frozen 39be3b38 passes full service POSIX 212/212 (66.9 s), peer 64/64
(38.5 s). HEAD 24865a2b only expands the actual-helper fixture to full inbox
capacity, random FIFO/permutation vectors and optional ASan/UBSan coverage;
its sanitized local run passes. Production binaries remain frozen at 39be3b38
through every timed cell.

Four four-minute exploratory cases use the same final 39be3b38 binaries and seed,
eight workers, fresh output trees, flow enabled and diagnostics enabled:
baseline, no-op only, both, group only. Compilation, functional gates and
profilers must not overlap the timed runs. Loader SIGUSR1 drains and leader-cut
catch-up precede every restart. Single-pass factors do not establish repeatable
performance acceptance. Raw evidence is under
`results/measure/20261009-w93-batch-hour-v2/`.

| Mode | Three 80 s rates, files/s | Mean files/s | Mean file latency, ms | n3 entries/sync | n4 entries/sync |
| --- | --- | ---: | ---: | ---: | ---: |
| Baseline | 121.94 / 50.30 / 48.82 | 73.69 | 106.51 | 3.39 | 1.39 |
| No-op continuation | 157.35 / 69.34 / 55.08 | 93.92 | 84.08 | 4.52 | 1.42 |
| Both | 118.37 / 44.88 / 61.79 | 75.01 | 104.62 | 3.41 | 1.45 |
| Group scheduling | 142.32 / 31.00 / 53.16 | 75.50 | 104.12 | 3.47 | 1.44 |

All four loaders drain successfully and all hosted replicas apply their sampled
leader commit cut. After the full service restart, both cold mounts verify 32
independently seeded exact payloads in each tree (256 checks, all pass).
No-op continuation runs only 28/80 times on n3/n4 in the
no-op-only interval (32/31 with both). That is too little to attribute the
27.5% higher mean to this mechanism; n3's baseline stream also includes 7,987
GC/reaping commands, while the no-op-only interval includes none. Fresh trees
can place different proportions of files on each group, and sequential runs
retain older data and different drive state. Group-only and combined results
are near baseline, and there is no dedicated live regrouped-frame counter yet.
Neither experiment has repeatable performance acceptance; both stay default
off. Rate decay remains and these results do not establish a hardware ceiling.

Next W93 measurement should classify the remaining behind-tail frames into
fully duplicated requests, verified overlapping prefixes with a new suffix,
conflicting terms, fresh-read contexts and arrivals without an active cohort.
The present continuation only admits the fully duplicated/no-new-byte case;
it does not admit an overlapping prefix followed by new entries. Large
behind-tail counters therefore do not measure the amount this fix can combine.
Only after that classification should eligibility be extended to a verified
overlap/new-suffix case, retaining conflict/truncation, byte/count/admission,
read freshness and durable ACK gates. No such extension is implemented here.

Frozen-build private TCP/direct recovery suite also passes with both experiments
enabled: O_SYNC/O_DSYNC and append/fsync visibility before close plus cold crash
bytes; offline synchronous callback failure retains exact bytes for later fsync;
32 identical/lost-reply APPEND RPCs allocate once with durable retirement; failed
partial flush drains without discard; graceful active leader shutdown preserves
18 acknowledged files; repaired KV WAL tails allow later acknowledged writes
to survive a second cold crash. The accompanying GC harness verifies empty
physical fragment inventory and late-PUT fencing on every member after restart.
Fixture `/data1/efs/gc-direct-_7mx29mc`; exit 0. This is process-crash/injected
failure coverage, not physical power-loss certification.


## Checkpoint overlap design to evaluate next

A bounded persistent worker could receive immutable `(group, applied_index)`
pairs only after the corresponding KV release succeeds, then write/fsync them
while the pump drains the next cohort. Keep one in-flight and one coalesced
pending pair, with backpressure when needed; never coalesce across index
regression, snapshot installation or a storage failure. The worker becomes
the sole owner of applied file descriptors and saved-index/throttle fields.
Shutdown must drain its last proven KV boundary before closing files, and
startup/restore must finish before it runs. The Raft log retention/replay
window and snapshot paths need explicit audit and bounded-lag gates.

This overlaps independent work after the KV durability prerequisite, rather
than allowing the marker to race ahead of KV. The current implementation remains
serial; the EIO guard is fixed without weakening sync or extending the marker
interval. A KV-WAL-embedded marker would instead remove a separate barrier, but
has greater on-disk compatibility/recovery scope.


## First measured command breakdown (baseline, 240 s)

Baseline mean 73.69 files/s (80-second rates 121.94 / 50.30 / 48.82);
17,954 successful file create/write/close latency observations. n3 (one copy
of both groups) appends CREATE 17,876, PUBLISH 17,822, SESSION 35,649,
PREPARE/DECIDE/RESOLVE 387, and GC/reaping tags 7,987: about 4 foreground
commands per file plus background/namespace work, 4.44 total appended commands
per completed file. Background GC/reaping is about 10.0% of this node's stream.
These interval counters are not request identities or exact committed verdicts;
five-second publication boundaries and final drain account for some denominator
mismatch. Background work may belong to retained older test data, not these new
files. SESSION aggregates several subtypes; two tags per file must not be assumed
to be redundant lease updates without a subtype/identity trace.

n3 phase sums: Raft drain 124.77 s, KV release 58.72 s, applied markers 38.57 s
in the 240 s workload; n4 120.00 / 67.19 / 46.41 s. n3 Raft cohort mean 3.39
entries/sync, n4 1.39. The serial persistence tail remains a substantial cost.

## Physical barrier census after comparison/recovery gates

A separate 30-second perf syscall census runs with flow on, both experiments
off and diagnostics off. It warms 32 scattered directories/files to open lazy
checkpoint descriptors, then runs a fresh eight-worker tiny-file tree and
drains it normally (6,179 files at final drain). These rates include tracing
overhead and do not belong in the factor table. Descriptor paths/inodes remain
unchanged, no events are reported lost, no selected sync fails, and four calls
still in flight at the recording boundary are excluded.

| Node | Raft sync calls / summed seconds | KV sync calls / summed seconds | Marker sync calls / summed seconds |
| --- | --- | --- | --- |
| n1 | 16,744 / 2.094 | 16,675 / 2.059 | 2,577 / 0.397 |
| n2 | 21,941 / 2.902 | 21,759 / 2.958 | 5,223 / 0.651 |
| n3 | 3,363 / 12.553 | 1,116 / 7.337 | 1,391 / 3.096 |
| n4 | 5,345 / 13.337 | 3,383 / 10.539 | 2,141 / 4.845 |

KV totals include one fsync on n1/n2/n3 in addition to fdatasync. These are
actual selected-file syscall counts, distinct from pump-cycle samples; they
include retained background work and are not exact barriers per logical file.
Summed syscall durations include waiting and are not block-device busy time.
Fragment-file and other descriptors are outside this table. Raw perf remains
on NUC with SHA-256 recorded; decoded compressed events are committed.

At the end, preserved service stores are restored on both mounts with frozen
39be3b38 binaries, TCP/direct I/O, flow enabled, both experiments and diagnostics
off, commands disabled. No loader remains active and no pending writes are
force-discarded. Source/test and evidence commits stay on
`devel-persistence-pressure`; no merge into devel is performed.
