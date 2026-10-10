# W93 checkpoint cadence investigation — October 10, 2026

User-authorized hour starts 04:26 UTC (00:26 EDT), NUC only. Branch
`devel-persistence-pressure`, worktree `i-l/work/efs-find-stats`. Existing stores
and payload trees are preserved. TCP/direct I/O, four daemons, two mounts;
command service and client command opt-in remain disabled. This is further
W93/W97 investigation, not closure or a hardware-ceiling claim.

## Committed branch integration

Frozen branch tips are in
[merge-tips.json](../../results/measure/20261010-w93-checkpoint-hour/merge-tips.json).
All 16 captured local branch tips are ancestors of this branch: first fast-forward
through `51880999`, then `4be9ecf6` (integrated devel). No other checkout or branch
pointer is changed; commits made elsewhere after this boundary are outside the
frozen integration. The merged native Linux build succeeds. Its first `make test`
stops at the doc-link gate because the isolated build copy omitted results files;
this is a controller packaging failure, retained in `merged-test.log`. Complete
linked evidence is supplied before the candidate's full rerun.

## Implemented retry correction and bounded experiment

`4bb7b169` moves `applied_saved_us` to the successful pwrite/fdatasync path.
Previously a failed open/write/sync updated the throttle timestamp although the
saved index stayed old, unnecessarily suppressing immediate retry. The fix does
not turn a failed barrier into success or advance its index.

`EFS_RAFT_APPLIED_INTERVAL_MS` defaults to the historical **10 ms**. Explicit
integer values 10–1,000 are accepted; malformed/out-of-range values refuse host
startup. A changed index reaches persistence when the cadence expires, when
1,024 additional entries accumulate, or on forced shutdown. No pending-work
queue or background marker writer is introduced; only the existing scalar
indices/timestamps are retained. Time is best effort when I/O stalls: neither
this cadence nor the entry trigger bounds lag after persistence failure.

Existing KV group commit already tracks appended/synced record watermarks and
returns without fdatasync when no new records need persistence. An empty pump
cycle is not another physical WAL barrier; the measured release phase includes
lock/wait work. Removing its call would not establish a useful safe speedup.

The pump still completes its KV WAL barrier before saving captured applied
indices, and failed KV release still blocks both periodic and forced markers.
Raft WAL syncs, consensus ACKs, publication barriers, log retention and the
16-byte complement-checked per-group marker format are unchanged. This only
coalesces restart hints, extending possible replay of already-durable effects.
It does not combine or overlap the KV and Raft durability boundaries. Runtime
adoption requires replay and live functional evidence; the longer cadence is
an experiment, not a new production default.

`d4f2ddb3` adds opt-in counters `applied_sync`, `applied_error`,
`applied_throttle`, `applied_entry_cap`, distinguishing actual successful marker
syncs from helper calls. Existing phase histograms count pump cycles, including
skipped marker work; their call counts must not be called marker-sync counts.
All counters remain diagnostics, not ACK/admission authorities.

Actual-source tests cover strict parsing, default/candidate cadence, the entry
trigger, forced shutdown, exact complement record bytes, open/pwrite/sync
failure, immediate retry, unchanged/zero/unhosted indices and backwards time.
The existing actual-source KV-error and retirement-order tests also pass.

## Test and measurement contract

`50f99afa` adds a private-follower pwrite interposer: only its applied-marker
writes fail; KV and Raft barriers run normally. The private replay gate proves
old marker bytes remain while newer effects apply, crashes the private fixture,
then verifies rename, hard links/inode identity, mode, truncate, deletion and
exact payloads after cold replay. Separate real KV-WAL sync-failure gates check
periodic/forced marker refusal and cold replay at the maximum 1,000 ms setting.
The ordinary sync-write, append, dirty-stop and WAL-tail private gates use
100 ms. These are process-crash/injected-I/O tests, not physical power-loss tests.

The full candidate Linux `make all` / `make test`, all five architecture gates,
and separate actual-source checkpoint/retirement regressions pass.
The deployed binary is built from production source `d4f2ddb3`;
`50f99afa` changes only the private harness. Linux full regression must pass
before replacement of service binaries. Functional live POSIX and peer suites
must pass before timed workload comparisons. Exact installed/executing hashes,
flags, source/build IDs and fixture manifests are retained.

Matched 600-second control/candidate create/write/close cells followed by a
180-second return control use 10 → 100 → 10 ms on the
same binary, fresh roots, four workers per verified inode group and deterministic
1–512-byte payloads. FLOW=1, OBS=1; OVERLAP/NOOP/GROUP_DRAIN/EARLY_RETIRE=0.
Preparation and graceful boot/drain are outside timers. The same immutable read
probe paths are read every five seconds in each case. No compiler, profiler or
functional suite overlaps the timed cells. Samples include space, process RSS,
I/O, diskstats and Raft progress. A cap hit, worker error or replica-cut failure
invalidates performance acceptance. Sequential retained-state/drive-state
confounders remain; return control and per-group windows must be reported.

## Results

Live default-10-ms POSIX first run: **211/212**, `concurrent_appends` exceeds
its unchanged 30-second budget; total suite 77.5 s. No peer or cadence comparison
is started after this failure. The isolated two append tests pass in 4.7 s
(120-second diagnostic minimum, not a replacement acceptance budget). A clean
full jobs=4 / 30-second repeat is started. The append profile starts after the
isolated tests finish: retain it as idle/background evidence, not an append-hot-
path measurement. No inference of a persistent append defect or its cause is
justified by the isolated pass.

Around 04:40 UTC NUC access fails: SSH times out, then a fresh connection
reports Host is down. The longer interval has never been enabled on service
processes. Remote repeat outcome, kernel/console cause and service state cannot
be verified while unreachable. No forced discard, service SIGKILL, wipe or
rollback is attempted. Private fault fixtures have not yet run. Recovery and
performance acceptance remain pending, and no speedup is claimed.

A local negative control compiles the original real persist helper from merged
`4be9ecf6`: after injected fdatasync failure its retry timestamp assertion fails.
The corrected helper passes the same contract. Raw expected failure and source
are retained; this reproduces the retry correction independently of live rates.

Access returns at 04:43 UTC with uptime unchanged (14 h 13 min), the same four
service PIDs and both clients. The clean repeat had completed successfully:
**212/212 in 64.4 s**, original jobs=4 / 30-second budgets. The first timeout is
retained as an intermittent full-load failure; a later pass does not identify
its cause. The network interruption did not cause the earlier append timeout
based on observed timing; no kernel cause is established. Full peer gates pass **64/64 in 77.2 s**.
The return control is shortened to 180 s to reserve recovery/restoration time;
report common-180 comparisons separately from the full 600-second pair.

The private retained-marker gate passes at **1,000 ms**: n3's real marker
writes return EIO while KV/Raft continue; captured older markers remain behind
applied work, then private SIGKILL/cold replay preserves exact bytes, original
inode/hard-link identity, renamed-only paths, modes, truncated sizes and removed
names. Physical deletion queues drain. Initial attempts stop before deployment
for missing empty mount directories and at an assertion before the first
throttled marker write; both logs are retained. The gate now waits for the real
refused marker attempt before asserting its contract. The service remains idle
and running on its own stores/mounts during these private fixtures.

The longer cadence fails a required append gate and is not adopted. Raw evidence directory:
[20261010-w93-checkpoint-hour](../../results/measure/20261010-w93-checkpoint-hour/).

## First completed control

600-second 10 ms control: **66.263 files/s**, windows **90.76 / 54.26 / 53.78**;
operation p99 **407.402 ms**. No worker cap/error, Raft sync error or marker error;
replicas catch the sampled leader cut after normal drain. NUC n3/n4 actual marker
syncs: **20,596 / 30,740**; marker phase **95.33 / 140.17 s**, KV release
**158.09 / 182.40 s**, pump drain **298.80 / 265.40 s**. Diagnostics cover slightly
rounded five-second boundaries and concurrent counters, not a transactional
snapshot. Per-cycle phase counts include skipped marker helpers.

Host diskstats show NVMe flush-request mean rising **2.38 → 3.70 → 3.67 ms**
across these windows, while request/flush rates and write MiB/s fall. NVMe busy
about **89.9 / 92.0 / 92.3%**; SATA about **16.0 / 10.3 / 10.2%**. These are host
block counters, not exclusive EFS attribution or proof of a drive ceiling.
Software still selects the number/pattern of barriers; hardware busy does not
establish optimal software or explain the internal drive/FS latency mechanism.

Actual placement differs in XFS options: SATA `/data1` is `noatime`,
`logbsize=256k`; NVMe n3/n4 resolve to `/home` on `rl-home`, `relatime`,
`logbsize=32k`, backed by Samsung SSD 970 EVO 1TB. No mount/device settings are
changed. That difference is a future controlled experiment, not an attributed
cause from this comparison. A sysfs NVMe-temperature probe returns no sensor
rows; no thermal-throttling conclusion is available.

Opcode records on both-group replicas are approximately three SESSION/lease,
one CREATE and one REPORT_PUBLISH per completed file, plus bounded CLOSE_STEP
cleanup. These are newly appended replicated records, not per-file physical
sync counts or proof that all session edges can safely be combined. W94 owns
atomic ownership/adoption improvements; preserve fencing and ambiguous-reply
recovery instead of simply skipping those edges.

## Strengthened follower oracle

`dd546567` waits for an actual refused marker attempt and records real fixture
process settings. `8ae58789` adds an offline reader of n3's stopped cold KV.
After replay it verifies renamed/link identities, absence of original/deleted
names, nlink, mode and reduced size directly on the follower, in addition to
client payload reads. This prevents a leader-served FUSE response from masking
wrong follower state. The final fault batch stops on the append replay regression before this stronger
oracle or the real-KV-error maximum-cadence gate runs. Both remain pending; the
earlier passing gate alone does not prove their additional assertions.

## Completed 600-second candidate

100 ms achieves **77.053 files/s**, **16.3%** above the first 10 ms control.
200-second windows are **100.27 / 65.99 / 64.91 files/s**; operation p99
**353.906 ms** versus **407.402 ms** (13.1% lower). Mixed-read p99 improves
from **280.302 / 380.799 ms** to **165.935 / 335.977 ms** for groups 0/2;
only 121 mixed samples per group and eight idle samples per phase are available.

Actual n3/n4 marker syncs fall **20,596 → 9,515** and **30,740 → 5,645**;
marker phase falls **95.33 → 48.53 s** and **140.17 → 28.20 s**. Raft entries
per sync are **4.80 → 5.50** on n3 but **1.080 → 1.067** on n4: this change
reduces the serial marker tail, not broadly solved Raft batching. KV release
phase rises with additional completed work (**172.00 / 238.81 s**). No error
or worker cap invalidates this cell; replicas catch the sampled cut.

Equal first-180-second rates are **94.886 / 104.062 / 92.691 files/s**
for 10/100/10 ms. Return-control 60-second windows **158.53 / 65.88 / 53.67**
reproduce the initial fast-then-slow curve; the full 600-second candidate remains
16.3% faster than the full initial control. These are one sequential triplet,
not repeated sustained acceptance. Keep the operational default at 10 ms.

## Active profile and independent persistence probe

The separate diagnostic workload captures all four daemons and both clients:
30 seconds CPU-clock, 99 Hz, dwarf stacks, approximately 23.09 CPU-seconds,
zero lost samples. Inclusive CPU samples: syscall entry **63.95%**,
`server_handle_conn` **23.23%**, TCP send **20.03%**, `host_sender` **11.77%**.
Overlapping stack percentages must not be added; this is CPU activity, not
wall-clock storage waiting. No isolated new CPU hot function dominates.

12-second n3/n4 strace wall sums: futex **132.70 s**, poll **28.23 s**,
fsync **19.96 s** over **37,447 calls**, fdatasync **17.86 s** over **6,883**.
Thread-summed waiting is not device busy time. The independent eight-second
path trace has successful marker sync mean/p99 **3.69 / 11.69 ms** (746 calls),
Raft WAL **5.07 / 16.49 ms** (1,440), KV WAL **6.49 / 20.72 ms** (691); two
unfinished/failed lines excluded. Path trace does not include fsync: identify
the high fsync call count by path before proposing its elimination.

With the production service stopped, a fresh 4 KiB buffered XFS overwrite plus
fdatasync probe verifies exact bytes and removes only its private scratch files.
SATA QD1 mean/p99 **85.9 / 117 microseconds**; NVMe QD1 **1.120 / 1.282 ms**.
Four independent writers: SATA mean **160–163 microseconds**, NVMe **2.50–2.62 ms**.
Only 128 measured operations per worker after 16 warmups; overwrite is not
append/allocation or EFS publication and cannot establish a hardware ceiling.
The low-load NVMe persistence penalty also exists without EFS running. Verify
XFS/LVM/device cache/power settings in controlled future experiments; this
probe does not identify which layer causes it.

## Blocking append gate and service restoration

The 100 ms private full fault batch passes synchronous small/queued/append
writes and cold exact bytes, then fails `append_replay_race` at
`acknowledged-next`: sequence 2, ACK 1, length 8 must return offset 8/end 16.
One of 32 requests does; 31 return offset 16/end 24. Initial, cold retry and
leader-death replay cases pass before that failure. Fixture manifest and
individual result logs are retained in `fault-fixture`; physical stores remain
on NUC. This is a real safety blocker, not accepted performance. Remaining
dirty-stop/shutdown/WAL-tail/max-KV/stronger-marker gates are not executed.

Traced code path: `server_raft_host_append` routes ACK-bearing RPCs to dual-group
hosts, takes a local striped lock, then `host_append_transaction` checks the
receipt and captures/executes exact transaction images. The lock is per process;
concurrent RPCs can still execute on two dual-group hosts. The next investigation
must inspect committed-but-unresolved receipt visibility, recheck between EOF
collection and receipt folding, and exact-image conflict/retry. This is a
localization hypothesis, not an established root cause or justification to
change durability/offset rules. A private 10 ms control then passes the complete append lost-reply, initial/
cold/leader-death/ACK-next/retired/changed-payload/cold-next sequence plus physical
GC/restart reclamation. The separate 100 ms append-only repeat also passes. Both controls omit the
preceding sync-write/crash cases of the failing full batch, so this is not a
matched full-batch cadence comparison. The failure is intermittent or depends
on earlier work; neither the root cause nor cadence causality is established.
A later pass does not erase the retained 31/32 safety failure.

Production service restores with the preserved stores, both mounts, default
10 ms, FLOW=1, other Raft experiments and OBS=0, commands disabled, TCP/direct.
Executing hashes match installed binaries. **384 exact first/last payload
checks pass through both cold mounts**. `final-service-state.json` explicitly
records the failed/not-run gates and restoration-only status. There is no
`finish-pass.txt`; do not infer full acceptance from restored service health.

W93 and W97 remain open. The retry-timestamp correction and experiment are
committed, but longer cadence remains disabled. Prioritize append safety before
repeating cadence acceptance, then profile fsync paths and reduce barrier/
metadata count while retaining authoritative ACK and watermark guarantees.

## End-of-hour disposition

The 04:26–05:26 UTC investigation completes with the merged branch and focused
source/test commits, raw profiles, comparison controllers and status records.
NUC is restored and healthy at default cadence; no measurement/private
loaders are intentionally left running. The newly added offline follower oracle
is compiled but remains unexecuted because the full fault batch stopped early.
Real-KV-error maximum-cadence coverage remains pending. No W item is closed.
