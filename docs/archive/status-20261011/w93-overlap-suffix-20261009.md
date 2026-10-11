# W93 verified overlap suffixes — October 9, 2026

Historical investigation: its W93-open disposition predates the later [atomic-apply acceptance](../../status/w93-atomic-apply-20261010.md). Original failed/not-run verdicts and restoration limits below are retained. W97 and [current Intel acceptance](../../status/intel-deployment-acceptance-20261011.md) remain active; this archive move closes no additional gate.

Second user-authorized hour starts 6:23 pm EDT (22:23 UTC) on
`devel-persistence-pressure`, isolated worktree, NUC only. Preserved stores,
TCP/direct I/O, commands disabled; no devel merge.

## Why the earlier no-op change was insufficient

Diagnostic-only build e3ed79e8 classifies one minute of the original tiny-files
workload with both prior experiments off and receiver flow control on.
Counters count each initially rejected behind-tail request once, rather than
double-counting the eligibility retry after a flush. Shapes verify the previous
term and every overlapping entry term, with bounded counts and overflow checks.

| Node | Fully duplicate | Matching prefix + new suffix | Behind-tail requests outside a cohort | Suffix arrivals in a cohort | Non-read/current-boot suffixes in a cohort |
| --- | ---: | ---: | ---: | ---: | ---: |
| n3 | 1,563 | 6,261 | 7,794 | 28 | 13 |
| n4 | 14,405 | 8,163 | 21,570 | 589 | 126 |

No classified prefix conflicts, configuration commands or unreadable prefixes
occur in this sample. n3/n4 suffix requests carrying read contexts number
2,952/6,354 (including arrivals outside a cohort). Counter intervals are
approximately 60.2/60.1 seconds, due to five-second diagnostic publication;
these are not all cluster requests, unique operations or dropped tickets.
Most useful suffixes arrive without an existing hold. Extending only an active
no-op cohort therefore cannot address most of this traffic.

## Implemented opt-in extension

Frozen production build ca810bac adds `EFS_RAFT_BATCH_OVERLAP=1` (default off).
A stable-leader/term, correctly routed follower request with a completely
term-matching prefix and a new suffix can start or extend a durable cohort.
Admission sees only its new entries; the cohort byte budget counts only newly
appended bytes. The normal receive path revalidates the log, skips old entries,
and queues replies behind the existing durable watermark. Configuration and
prefix conflicts never enter this path; term/leader changes and retired boot
incarnations retain their original behavior. A fresh read flushes an existing
overlap hold before its new data can start another one. Empty/read probes keep
the prior rules. Reply/count bounds, byte caps and persistence-failure fencing
are preserved, and no format or sync guarantee changes.

Classification counters now distinguish actual ineligibility endings from
eligible overlaps; `batch_overlap` counts selected receive requests once,
including when a byte/read boundary requires reevaluation before processing.

Actual-core ASan/UBSan tests cover starting/extending a hold, admission of only
new entries, refused admission, old boot IDs, committed and uncommitted
conflicts, higher terms, fresh-read flushes, cumulative byte caps and sync EIO.
ACK/apply callbacks assert they never see an unproven durable index. Linux
core/store/wire and actual checkpoint/inbox/classifier tests pass. Full service
POSIX 212/212 (65.6 s), peer 64/64 (33.8 s) pass with the extension enabled.
Private old-leader read/session/publication/membership partition gates pass.

## Sustained comparison method

Two 900-second cases use identical ca810bac executables: overlap off/on,
receiver flow on, duplicate/no-op continuation on in both, group scheduling
off, diagnostics on. The optional `EFS_ADMISSION_PRESSURE` setting is not
enabled; transport flow control and metadata admission pressure are distinct.
Four writers target each metadata group. Preparation selects 128 directory
inodes per group; each writer owns 32 and writes up to 1,000 names per directory,
with a hard 32,000-file worker cap. Timed operations are CREATE → 1–512-byte
write → close; directory creation/preparation are excluded. Inode parity and
first-file placement are checked against the actual source mapping. Fresh
output roots, deterministic payload oracle, bounded latency arrays and
periodic space/RSS/disk/commit/apply samples are retained.

Exact open/write/close/total p50/p95/p99 values include successful final drain
operations. Timed rates use cumulative samples at 0/300/600/900 seconds. Small
immutable read probes cover idle before/after and mixed traffic every minute;
the same probes run in both modes. They add a small reported observation load,
and the mixed sample count cannot certify rare-tail behavior. Any worker cap
hit invalidates unclipped performance acceptance. Logs are captured/compressed
after each timed interval; no compilation, functional suite or profiler runs
overlap those intervals. Loaders drain using SIGUSR1; all hosted replicas must
apply a sampled leader cut before a restart. No pending writes are discarded.

The comparison lock is held by the controller with `flock --close`, preventing
service daemons from retaining it. The initial diagnostic deployment inherited
the lock; the next deployment correctly refused it, then a graceful stop
released those descriptors. No lock-file replacement or force-discard bypass
was used.

Raw evidence: `results/measure/20261009-w93-overlap-hour/`. This is one
sequential A/B pair, not repeatable sustained acceptance; drive state and
retained background work remain confounders despite controlled group placement.

## Pressure-counter interpretation

With receiver flow control enabled, `inbox_drop` increments at the BUSY branch
of `server_raft_host_inbox` before any copy/admission. It therefore counts
non-admitted frames/retry pressure, not unique user operations, lost files or
lost durable log entries. Transport feedback is not a durability ACK; Raft
retains log authority and resends. Baseline n3 records 28,826 refusals: 24,776
data frames and 4,050 empty requests. No sync errors are recorded. n3 mean
sampled inbox residence is 86.96 ms. Its 9.66 entries per actual sync include
replication/catch-up/background activity; they are not per-created-file ratios.

The host inbox remains bounded to 256 frames/32 MiB, with flow thresholds at
64 frames/8 MiB and reserved control space. A receive cohort retains its
16-request/1 MiB-new-command bound. RSS and status samples observe behavior
but do not prove continuous queue or lag maxima; status probes are sequential
and compare against a sampled leader cut.

## Completed 900-second comparison

Both cells complete normal drain, report no loader errors or cap hits, and
all hosted replicas apply the sampled leader cut. Identical efsd SHA256:
`3c8fe4872cca6afb98f674014de05c7e6db77ce411f6a7a10917ca4879596369`.
The installed build reports `ca810bac`, built 22:32:23 UTC on NUC.

| Workload measure | Overlap off | Overlap on |
| --- | ---: | ---: |
| Mean files/s | 120.30 | 129.66 |
| First / second / third 300-second files/s | 135.89 / 116.77 / 108.25 | 147.36 / 118.56 / 123.05 |
| Group 0 window rates | 119.29 / 102.58 / 93.26 | 128.85 / 103.93 / 108.97 |
| Group 2 window rates | 16.60 / 14.19 / 14.99 | 18.51 / 14.64 / 14.08 |
| Total operation p50 / p95 / p99, ms | 37.52 / 282.46 / 363.14 | 35.66 / 277.70 / 342.12 |
| Open p50 / p95 / p99, ms | 1.53 / 158.66 / 218.11 | 1.41 / 155.16 / 205.28 |
| Close p50 / p95 / p99, ms | 35.65 / 125.21 / 173.33 | 33.95 / 121.93 / 163.98 |
| Write p50 / p95 / p99, microseconds | 23 / 47 / 245 | 23 / 67 / 276 |
| n3 / n4 entries per actual Raft WAL sync | 9.66 / 1.20 | 11.14 / 1.33 |
| n3 / n4 follower requests per cohort | 1.00 / 1.02 | 1.42 / 1.21 |
| n3 / n4 BUSY admission refusals | 28,826 / 9 | 29,259 / 0 |
| n3 / n4 mean inbox residence, ms | 86.96 / 16.27 | 87.66 / 15.20 |
| Maximum sampled daemon RSS, MiB | 1,624 | 1,689 |

Mean file rate is 7.78% higher and operation p99 5.79% lower in this pair.
This does **not** remove the slowdown or prove a repeatable causal gain:
group 2's final interval is slower with overlap on. n3/n4 sync elapsed means
rise from 11.28/7.38 ms to 12.22/7.76 ms. Ratios include catch-up and background
log work, rather than exclusive foreground operations. Sparse status samples
show at most 45/24 entries behind the sampled leader apply cut, respectively;
sequential probes cannot certify continuous maximum lag.

Useful overlap requests selected on n2/n3/n4 number 54,636/41,874/28,562.
Fresh-read overlap boundaries still flush 1,198/1,974/3,295 active holds;
those correctness boundaries are not removed. The n3/n4 barrier-per-entry
ratios improve by approximately 13.3%/9.9%, but whole pump-cycle persistence
remains dominated by serial phases. Cumulative measured phase durations:

| Node / phase, seconds | Off | On |
| --- | ---: | ---: |
| n3 pump drain | 541.29 | 537.44 |
| n3 KV release | 210.27 | 212.96 |
| n3 applied-marker persistence | 111.32 | 110.59 |
| n4 pump drain | 370.47 | 336.30 |
| n4 KV release | 296.89 | 317.08 |
| n4 applied-marker persistence | 228.26 | 236.23 |

These are summed instrumented phase durations per daemon, not independent
hardware utilization percentages or CPU time. No sync error is recorded.

## Read-latency and background-work acceptance limits

| Read sample, milliseconds | Off p50 / p99 | On p50 / p99 |
| --- | ---: | ---: |
| Group 0 idle before, n=8 | 2.37 / 7.74 | 4.94 / 18.35 |
| Group 0 mixed, n=16 | 5.53 / 28.08 | 19.95 / 38.65 |
| Group 0 idle after, n=8 | 2.33 / 11.26 | 7.00 / 24.03 |
| Group 2 idle before, n=8 | 6.93 / 11.14 | 5.96 / 10.63 |
| Group 2 mixed, n=16 | 300.89 / 404.32 | 235.41 / 338.52 |
| Group 2 idle after, n=8 | 14.99 / 45.57 | 12.43 / 25.47 |

Group 0 read samples worsen; a sparse sample cannot prove causality or rare
tails, but it also cannot satisfy the no-idle-regression gate. Warmed isolated
read trials and reversed/repeated balanced comparisons are required.

Baseline background GC/reap accounts for 0.98/1.69/1.66/6.06% of append
commands on n1/n2/n3/n4; enabled-window counts for these opcodes are zero.
Thus some throughput difference can reflect background work finishing between
cases. Command appends broadly match CREATE + PUBLISH + two SESSION commands
per foreground file, reinforcing W94's separate guarded ownership/lease
reduction opportunity; opcode counts alone do not classify each SESSION
subcommand or count per-file durability barriers. Physical free space remains
above 470 GiB on /data1 and 600 GiB on the actual /home NVMe volume during
these captures. All test payloads and existing data are retained.

## Final recovery and disposition

All 256 first/last exact cold payload checks pass through both mounts. The
private direct/TCP suite exits 0 with overlap/no-op on: sync-write and failed
O_SYNC/O_DSYNC retention, append sync boundaries and replay/lost-ACK edges,
dirty-stop recovery, fsynced/shutdown mutations, WAL-tail repair followed by a
second cold crash, orphan/GC draining, quota-zero/late-PUT fencing and physical
fragment reclamation pass. Existing service stores remain preserved and are
restored with overlap/no-op/group scheduling and diagnostics off, receiver
flow on, command service off. This is software fault/process-crash coverage;
physical power loss is untested. The experiment remains opt-in/default off.
W93 broader NUC sustained acceptance stays open: repeat after background
queues settle, reverse trial order, and isolate the observed group 0 read-tail
regression before enabling by default. This is not a hardware ceiling.
