# Internal durability batching measurement — NUC, 2026-10-09

## Findings

The bottleneck is not simply an empty follower queue or a small batch-size cap.
Leader proposer cohorts are nearly single-threaded, but NVMe follower n3 has a
substantial inbox backlog. Interleaved group traffic and strict eligibility
boundaries prevent most of that traffic from becoming a cross-request batch.
Persistence remains expensive; this identifies software/protocol constraints
worth investigating, not a proven throughput improvement or hardware ceiling.

| Node | Active proposers, mean | Shared hold mean | Nonempty inbox messages, mean | Entries per actual WAL barrier | Mean barrier section |
|---|---:|---:|---:|---:|---:|
| n1 / SATA | 1.008 | 12.0 us | 1.09 | 1.011 | 0.143 ms |
| n2 / SATA | 1.180 | 112.4 us | 1.15 | 1.170 | 0.155 ms |
| n3 / NVMe | no samples | no shared holds | 29.71 | 2.114 | 5.835 ms |
| n4 / NVMe | no samples | no shared holds | 5.06 | 1.409 | 5.341 ms |

Proposer depth includes the arriving caller and callers blocked on the host
mutex: it is not a time-weighted thread count. Shared holds are the explicit
disk hold API, not follower thread-local deferral. No samples is not zero
latency. Barrier elapsed includes fdatasync and mutex reacquisition/diagnostic
work, not solely device time. Counts include background and replication work.

Leader overlap is low: n1/n2 proposer arrivals observe depth one 99.3%/86.0%
of the time. n1 hold p95 is at most 32 us; n2 at most 512 us. The bounded
proposer yield loop never reaches its 10,000-spin cap. Longer batching limits
alone do not make dependent create/write/close requests arrive concurrently.

### Where batches end

For n3's host inbox segmentation, 35,161 of 42,954 batch boundaries (81.86%)
are group switches; 1,657 (3.86%) reach the 16-message cap; 6,136 end the
drained inbox. None reach the 1 MiB byte cap. Node 4 has one observed group:
99.58% of its host segments end the drain, and 0.42% reach the count cap.

Every eligible cross-request follower batch on n2/n3/n4 contains exactly one
request. For n3, 1,290 of 1,836 (70.26%) end on an ineligible message and the
rest at the end of the supplied segment. For n4, 3,041 of 5,506 (55.23%) end
on ineligibility. No follower byte-budget/error boundary occurs.

Eligibility checks on n3 reject 106,883 empty requests and 74,893 tail-match
failures; n4 rejects 43,321 empty and 54,944 tail failures. No term, configuration,
admission, entry-limit or byte-limit rejections occur during the selected
intervals. These are check counts, not unique messages: an ineligible boundary
can be checked again after flushing. The counters do not attribute each active
batch's ineligible boundary to a specific rejection reason. Tail mismatches
are not classified as behind/ahead/term-different, and cannot be called
unnecessary duplicates from this evidence alone.

`follow_messages` measures eligible **cross-request** batching; an individual
AppendEntries can itself contain multiple entries, and the existing intra-message
batching also covers overlapping/non-strict requests. Therefore one request per
eligible batch is not one entry per all WAL syncs. Actual WAL cohorts are
measured independently from captured internal write/durable watermarks.

### Time spent assembling versus persisting

n3 eligible follower assembly averages 11.8 us; total through its durability
barrier averages 5.47 ms. n4 averages 14.2 us assembly versus 5.77 ms total.
The assembly work is small relative to the persistence wait. This does not
justify adding arbitrary delays or dropping barriers.

| Window | Files/s | n3 inbox mean | n3 WAL entries/barrier | n3 mean barrier | n4 mean barrier |
|---|---:|---:|---:|---:|---:|
| First 100 s | 93.60 | 41.06 | 2.109 | 3.902 ms | 3.461 ms |
| Middle 100 s | 52.19 | 27.29 | 2.556 | 8.382 ms | 7.376 ms |
| Last 100 s | 38.88 | 19.23 | 1.711 | 7.447 ms | 6.950 ms |

The real interval rate declines while the follower queue remains backlogged.
Queue occupancy alone does not imply the queued requests can be combined.
Node 3 logs at least ten inbox-full dropped AE frames at the workload start;
logging is rate-limited after the first ten, so this is not a total-drop count.
Data and empty control frames are both present. This is a material pressure/
retransmission issue, not proof of corruption: no corruption/failure oracle
was run for those drops, and Raft retries remain the intended recovery path.

The unchanged `.efs` catalog cap is an additional confound: n2 repeatedly
reports initial refresh rc=-2 at 1024 MiB (roughly 1.7–6.3 s logged builds),
and n3 reports a failure after the workload ends. The RAM-backed catalog scale
gap remains W92. Foreground rate changes cannot be attributed only to WAL
policy, device behavior or this instrumentation.

## Implementation and method

Instrumentation commits `ebe72fdb` and monotonic timestamp correction
`9fd7862e`. All staged binaries are built with HELLO identity
`9fd7862e-batchobs`, human version `9fd7862e`, branch `devel-find-stats`.
Deployed SHA256 and commands are retained. Environment `EFS_BATCH_OBS=1` enables
fixed-size cumulative counters/histograms and a JSON summary every five seconds.
Default/unset is off; disabled paths perform flag checks but no observation
clocks, allocations or dumps. This is not a zero-overhead claim for disabled
branches or an overhead-calibrated enabled benchmark. No metrics control
admission, ACKs, fsync, log matching or queue limits.

WAL entry ordinal counts advance after successful entry-record writes. Each
barrier captures the entry ordinal and valid-byte end under the existing disk
mutex, and advances the diagnostic covered ordinal only after successful sync.
Writes during a barrier belong to the next covered cohort. Successful rotation
advances the diagnostic ordinal to avoid counting rewritten records again;
rotation and failed-sync counters are separate. No rotation or sync failure
occurs in this live measurement. This avoids the earlier syscall-boundary
coverage estimates.

Shared hold durations, sync waiter depth/wait durations, proposer arrival depth,
nonempty inbox size, host segmentation reasons, follower eligibility reasons,
cross-request cohort size and assembly/total duration are recorded separately.
Histograms have fixed logarithmic bins; zero/one bins are exact, reported
percentile values are **upper bounds**, not exact percentile measurements.
Last bin is overflow. `max` is cumulative since daemon start, not per-window.
Snapshots read independently cumulative atomics; interval boundary discrepancies
are possible and reported by the analyzer. All selected histogram bin/count
deltas happen to agree in this run.

The cluster was cleanly stopped without force-discard, installed from an isolated
tested build, then restarted TCP/direct I/O with both mounts and `.efs` enabled.
Original data and the old 50,000-file tree remain. A transient SSH timeout
recovered during startup. client.sh's post-unmount PID race is handled only
after verifying both mount and client process are gone; dirty retained mounts
still refuse deployment. Old binaries are retained remotely under
`/data1/efs/batch-internals-20261009/rollback`.

Measurement UTC: 06:34:13–06:39:13, 300 seconds, 61 host/process/disk samples.
Per-node cumulative observation windows last about 300 seconds, with five-second
logger boundaries (approximately -4.5..299 seconds relative to loader start).
Raw `n*-window.log`, `n*-observations.jsonl`, baselines and monotonic timebase
are retained. `analysis.json` records exact endpoints and deltas; reproduce with
`python3 analyze.py`. No perf or strace recorder runs concurrently.

The dedicated wrapper preserves the original tiny_files.py payload/folder RNG
and eight workers, max depth 8, up to 1000 files/folder, max target 50,000 and
1–512-byte payloads. Only cooperative stopping is added. SIGUSR1 stops scheduling
and checks between files; open files finish normally. Loader exits successfully
at 300.62 seconds. Capture reports 18,467 files at its last progress sample;
final output is retained separately, not treated as an independent filesystem
count or checksum oracle. New data remains at
`/data1/efs/mnt/tiny-batch-internals-20261009`.

## Validation and scope

Raft and durable-store tests pass with observation disabled and enabled. The
separate observer oracle proves cohorts 3/1/1, sum five entries and 170 bytes,
including rotation, and emits no metrics when disabled. Native `make test`
recipes through storage/churn pass; the isolated staging tree omitted the docs
checker, so the run stops there. Remaining native recipes were then completed
via the retained continuation makefile with the final consistent build; final
enabled Raft/store tests pass again. This is not a claim that the initial
uninterrupted make invocation passed. Architecture checks run in the complete
local worktree; generated docs are refreshed with this report. Build/test logs
are in `validation/`. An earlier build-identity attempt was stopped and rebuilt;
its interrupted log is retained and is not final-build acceptance evidence.

No new POSIX/peer suite or hardware power-loss gate is claimed by this measurement.
It adds diagnostic instrumentation, not a batching-policy fix. After capture,
the collector verifies loader exit and the cluster is cleanly restarted with
`EFS_BATCH_OBS=0`; final startup/process/environment evidence records that state.
A limited cold peer-read smoke checks 32 retained files: both mounts return
identical payloads of 1–512 bytes. Hashes/paths are in `cold-peer-smoke.json`;
this is not a pre-crash expected-payload oracle or full namespace verification.

## Next controlled investigation

Investigate receiver pressure, group interleaving and non-strict AppendEntries
traffic before increasing caps. Add boundary-specific rejection attribution and
classify tail mismatches behind/ahead/term-different; quantify total drops and
request duplication/retry causes. Evaluate safe suppression/coalescing of redundant
traffic without reordering dependent operations, changing read contexts or ACKing
unpersisted entries. Compare bounded coalescing policies on identical repeated
fixtures, including idle/mixed p99, queue/drop rates, RSS and cold/fault gates.
Increasing queue capacity alone can postpone overflow without increasing service
rate. Device/filesystem and W92 background-load controls remain separate experiments.
