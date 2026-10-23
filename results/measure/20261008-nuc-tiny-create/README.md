# NUC tiny-file creation investigation — Oct 8, 2026

Baseline c22754236b38, four cohosted daemons, TCP, buffered I/O. User generator
/home/efs/tiny_files.py: 8 folder workers, 1–512 byte precomputed payloads,
max depth 8, max 1000 files/folder, 1M file target. No explicit fsync, but EFS
close publishes durably. All output trees preserved. Fresh directories and
random directory/payload selection mean rounds are exploratory, not controlled
statistical comparisons. Tracing perturbs throughput; rate is captured before
strace/perf. Screen progress batches 64 files per worker, so counts can lag
by up to 504 completed files. Startup rate is not sustained throughput.

Baseline screen: 17860/414.1s then 20284/527.1s. Generator wall strace:
openat 131162us, close 86152us, fstat 29423us, write 182us. Daemon trace:
11872 fsync (2207us average),1124 fdatasync (3760us) in12s, all4nodes.
Counts include background work. Sync stacks identify create/HOLD/REPORT,
KV WAL and applied marker persistence. Untraced10s disk sample: nvme0n1
95.38%busy,615.3writeIOPS,3.568MiB/s,2.345mswriteawait,232.6flushIOPS;
sda3.08%busy,259writeIOPS,3.468MiB/s,0.128mswriteawait.
Perf stat FUSE+n2+n3:0.203CPUs over10s. No CPU saturation established.

## Five iterations

Each round built with an explicit c22754236b38-tiny-rN identity, passed
Raft-store and KV-LSM tests, drained both main clients, stopped only the four
owned main daemons, restarted TCP/buffered preserving stores, and launched the
same generator in screen1110701.tiny_files against a new directory.
Rate was captured at30s, then10s wall-clock strace on generator and10s perf
on all main efsd processes. Output roots /data1/efs/mnt/tiny-profile-r1..r5.

| Round | Cumulative candidate | Screen files /30s | files/s |
|---|---|---:|---:|
|1|KV WAL fdatasync|4924|164|
|2|plus Raft log fdatasync|2791|93|
|3|plus skip already-durable Raft log sync request|5594|186|
|4|plus applied restart-marker checkpoint100ms instead of10ms|3819|127|
|5|revert round4; replace yield loop with200us cohort delay|4392|146|

Rounds4/5 tuning reverted. Final candidate equals round3: data-only durable
WAL synchronization and avoidance of a redundant already-covered Raft barrier.
No observed large improvement and no proven sustained speedup. Runs are not
randomized controlled comparisons: growing retained namespace, random folder
selection and restart recovery/background work differ. Original170files/s was
5sstartup; original38files/s was527slifetime; neither is directly comparable
to these30srates. Eight workers with measured tens-to-hundreds-ms per-file
metadata waits cannot deliver1000–10000files/s merely by changing sync flavor.
Payload write latency is small; current create/hold/stat/report lifecycle still
pays durable replicated metadata round trips. Further work needs stage-level
latency/RPC counts and controlled batching/placement experiments, preserving
open-unlinked safety, publication and durability boundaries.

All user/output trees preserved, original generator stopped withSIGTERM only
between experiments. No force discard, mkfs or store wipe. Final candidate
has source changes, not a clean committed release. Binary hashes/source patch
and final validation retained separately. These process-crash/reopen tests do
not simulate physical device power loss.

## Final validation and state

Final candidate c22754236b38-tiny-rfinal passes Raft-store/KV-LSM suites,
raw query RPC/bounds, all nine virtual gates, full POSIX and peer suites,
and persisted restart rebuilding the independent112file/153dir/4212byte oracle.
Private fixture /data1/efs/evidence/tiny-final-validation-20261008 retained; these are
private buffered/TCP regression tests, not physical power-failure acceptance.
Main service restarted preserving all stores. Generator resumed in existing
screen1110701.tiny_files using /data1/efs/mnt/tiny-profile-final with original
parameters. Final code changes remain uncommitted on devel-find-stats; main
/Users/mike/git/efs concurrent work untouched. Oldw90loader remains paused.

## Sixth round — eliminate the CREATE reply GETATTR

User requested one additional round. The prior final loader was at18400files /
384.1s (48files/s lifetime) before stopping it. Its directory is retained.
Round6 starts from the previous final candidate and adds one request-local
inode output to the client CREATE helper. FUSE constructs a successful new
CREATE reply from that committed RPC row, avoiding the immediately repeated
GETATTR through efs_client_stat_open. No lookup memo is populated and attribute
and entry TTLs remain zero. O_CREAT opening an existing file retains lookup_fill;
subsequent fstat/GETATTR and server HOLD/release paths are unchanged.

Round6 new output: /data1/efs/mnt/tiny-profile-r6. Build identity:
c22754236b38-tiny-r6, with exact binary/source hashes and patches in round-6/.
At30s:5271displayedfiles,176files/s. Subsequent10s wall strace:
1136openat averaging32.374ms,1136close30.104ms,1139fstat6.739ms.
Traces from different randomized folder/leader states are not an isolated
latency A/B experiment. No syscall savings percentage is claimed from them.

Untraced interval71s→101s:9436→10562displayedfiles,about37.5files/s.
Progress updates are batched; instantaneous completion counts are uncertain.
During that interval NVMe busy93.47%,8.27MiB/s,331.10flushIOPS;
sda busy7.39%,11.06MiB/s. Sustained throughput remains limited; this round
does not establish a sustained speedup or the1000–10000files/s target.

Extended concurrent create error/snapshot tests pass on Mac and NUC;
Raft-store/KV-LSM tests pass on NUC. One pre-existing append_plan.c type-limits
warning is retained. Full private query/POSIX/peer/restart results are recorded
in round-6/full-validation.log. Generator is paused only during that private
validation and resumed by a shell EXIT trap; later lifetime screen rates include
that pause and are not comparable performance observations.

Round 6 final outcome: all private gates PASS; POSIX 216/0/1 in 16.1 s,
peer 64/64 in 12.0 s, restart oracle 112 files / 153 directories / 4,212 bytes.
Population PID 1623836 verified running (Sl) after automatic resume. Round 6
source remains uncommitted on devel-find-stats and installed on NUC.

## Seventh round — separate leaders, explain slowdown, reserve Raft log space

Read the current status in `docs/status/nuc-tiny-file-create-20261008.md`.
Separate client, n1/group-0 leader, n2/group-2 leader and n3 follower profiles
are retained under round-7/before and round-7/after. Earlier pooled daemon
profiles were insufficient to attribute the slow group. All eight sampled
active folders mapped to group 2. Whole-folder worker scheduling accumulates
workers on this group, which requires a replica on the busy shared NVMe.

Baseline controlled alternating eight-worker/256-file runs:
group 0 159.9/175.2 files/s; group 2 18.7/18.9 files/s.
After bounded 1 MiB Raft preallocation: group 0 147.2/163.5; group 2 31.1/31.8.
Same directories and leaders, one deployment between comparisons. Small runs,
not randomized A/B/A; no sustained whole-workload gain is claimed. File records
and durable watermarks exclude the reserved zero tail. All existing durability
barriers remain. Crash/replay/rotation and diagnostic-reader tests cover the
new physical layout; failed/unsupported preallocation uses ordinary writes.

Round 7 startup 2,931 displayed files at 30 seconds (98 files/s), then
2,048 displayed files added from 27 to 57 seconds (~68/s); batched progress and
random folder placement apply. NVMe busy 92.9%. Generator remains active in
`tiny-profile-r7-fix` after validation; all earlier data remains. Later lifetime
rates include deliberate pauses. Exact deployed build/hashes in identity.txt,
all source changes in source.patch, full POSIX/peer/virtual/restart evidence in
round-7/. Changes remain uncommitted on devel-find-stats.

Round 7 acceptance: Raft-store/core, KV-LSM, concurrent CREATE and parser tests
PASS; private buffered/TCP POSIX 216/0/1 (16.0 s), peer 64/64 (10.4 s), nine
virtual gates, raw query RPC, restart oracle 112 files / 153 dirs / 4,212 bytes
PASS. Main generator PID 1747101 verified running (Sl). No physical power-loss
or 1,000–10,000 files/s acceptance is claimed. No commit or push performed.
