# NUC closure session — October 10, 2026

**Tracking update, Oct 11:** The NUC filesystem/device persistence-latency
investigation in this report is now [W111](w111-nuc-persistence-investigation-20261011.md).
Dated W97 labels and measurements below are retained as historical evidence.
The [AMD overload/admission gate](amd-overload-admission-20261011.md) is closed
and does not depend on this NUC investigation.

User assignment: review devel, create and use `devel-nuc`, reserve NUC
exclusively for three hours, prioritize documentation items with short remaining
NUC gates. Session starts October 10 at 23:41 EDT (October 11 03:41 UTC);
the work window ends October 11 at 02:41 EDT (06:41 UTC).

## Branch and preserved service

Branch starts at devel `746ae9f099351624f5c38fa872dcc6f4daa5e7c3`.
Worktree: `/Users/mike/Documents/Codex/2026-10-07/i-l/work/efs-find-stats`.
Other checkouts, AMD, Intel and Xorinox are outside this session.
Production code initially matches that captured devel tip; the early branch
commits add measurement and missing operator acceptance tests.

Remote isolated source and evidence: `/data1/efs/devel-nuc-20261010`.
Existing four-node stores and both service mounts are preserved. Measurements
cleanly drain the regular clients and daemons, then restore the installed
service in `finally`. No reboot, drive setting change, installed-store wipe,
forced dirty discard, main merge, push or tag is requested.

## Ordered work and acceptance

| Item | Selected remaining scope | Session state |
| --- | --- | --- |
| W96 | Production KV WAL reservation on/off, both physical drives, repeated ABBA batch distributions, bounded allocation, reset/reopen and exact replay; full native/POSIX/peer/fault/cold gates | Closed Oct11; repeated isolated measurements and full software recovery gates pass |
| W78 | Absent/dead/invalid/unrelated/wrong-storage/stale-start PID and owned daemon/recorder retirement | Closed Oct11; current native and real NUC startup/stop gates pass |
| W79 | Both aliases and help, real consumed successful mkfs replies, uncertain-outcome retry preserves namespace/salt, explicit extra-argument refusal | Closed Oct11; direct/buffered private gates pass |
| W70 | Accepted/refused quota reductions, unchanged bytes and no fictitious migration | Closed Oct11; direct/buffered × one/two-root current-code gates pass |
| W80 | Retired QUERY_STATS gives nonzero unavailable response with no false totals in normal/raw output; current help and legacy/malformed reply tests | Closed Oct11 for retired endpoint; current-code nonempty namespace, fragment, help and reply gates pass |
| W97 | Three-writer durable file controls outside EFS; growing/reset files versus preallocated rings, reversed repeats, barrier distributions, disk/CPU counters and exact reopened blocks | Investigation advanced: eight clean cells, four profiles and four written-ring cells pass; remains open |

W73/W64 retain production-session/platform obligations. W53 explicitly requires
investigation followed by the user's keep/revert decision. W54/W38's physical
multi-host gate cannot close on a single NUC. W92's current billion-entry and
400 GiB fixtures belong to the retained separate execution environment. These
are not replaced by unrelated NUC smoke passes. RDMA placement follows W106:
AMD software RDMA or orcd-test hardware RDMA, not this NUC session.

## Measurement limits

The W96 harness includes production `kv_wal.c` and differs only by the existing
compile-time reservation constant (zero control versus default 1 MiB). It
persists one or sixteen 128-byte values per atomic record. Every 8 MiB it
checks replay, resets the covered WAL and verifies bounded reservations. Timed
throughput includes these checks and reset barriers; reported sync counts also
include reservation-tail repair and the final reopen check. This is a WAL
principle measurement, not end-to-end file creation or a complete production
metadata batch-distribution model. Sync percentiles have 1 microsecond bins
with a 1-second overflow bin and a separately retained exact maximum.

The W97 control uses three independent 64 KiB writers, each with a 256 MiB
file and a SHAKE-derived full-length body with an eight-byte counter. Growing
mode truncates and durably resets at the boundary; ring mode
preallocates before timing and overwrites slots. It measures allocation/rotation
costs; it neither implements an alternative EFS storage engine nor establishes
NAND-cache occupancy. These controls use ordinary buffered file I/O plus
fdatasync, not O_DIRECT. Their64KiB body relates to a physical fragment, but
they do not model fragment/checksum files, parent-directory barriers, metadata
publication or the complete EFS workload. Full current files are reopened and checked exactly,
without claiming kernel-cold or power-loss coverage. Sync histograms overflow
at 100 ms, retain the overflow count and exact maximum. Passive counters retain
host contention and process CPU rather than assuming zero software overhead.

## Terminal evidence

The three-hour session completes five named closures: W70/W78/W79/W80/W96.
All planned private gates and drive controls have terminal successful verdicts;
W97 remains an investigation, not a sixth closure. Regular service restoration
and bounded existing-data reads pass; installation of the newer tested binaries
remains approval-blocked.

Named NUC engine build: `3df1931032b8-nuc`, branch `devel-nuc`; production
C/header/Makefile code is unchanged from the captured devel tip. The copied
source and gate overlay are verified by SHA256. Subsequent changes add tests
and documentation; the raw W97 body overlay is `927eaa3c`, without an engine
change. The full Linux native suite passes with its five documentation checks.

* W70: all four direct/buffered × one/two-root cases observe the new running
  quota, refuse a below-usage reduction without another quota change, and
  retain exact fragment bytes. These are volatile quota semantics.
* W78: unit ownership cases include absent/dead/invalid records, another live
  process, wrong storage and stale start identity. Actual wrapper launches with
  invalid/unrelated/dead records retain the unrelated process, record the real
  new daemon identity and retire it gracefully. Recorder tests retain unrelated
  and wrong-output processes and stop the matching recorder with SIGINT.
* W79: direct and buffered three-node private fixtures test both alias helps,
  missing/malformed/excess arguments, compatibility notices and unchanged salt
  and namespace. Four real successful replies are consumed by a relay with
  zero bytes delivered; each CLI fails explicitly, and an explicit retry
  succeeds without resetting the export. Exact exchanges are retained.
* W80: normal/raw queries on both nonempty authoritative namespaces and four
  stored-fragment fixtures produce no totals and return unavailable exit 2.
  Help explicitly names retirement. Separate unsupported/legacy/malformed
  reply fixtures pass; malformed replies exit 1. This does not activate or
  close W92's mounted snapshot/scale features.

W96's sixteen 90-second ABBA cells and independent read-only, exact ordered
key/CRC/payload checks all pass. Both shapes cross reservation/reset boundaries
within the matrix. NVMe single-record controls do not reach the 8 MiB reset
boundary in 90 seconds; the candidate and larger shape do. Reserved slack
stays below 1 MiB. These are conditional WAL results:

| Volume / items per record | Control records/s | Reserved records/s | Gain | Mean sync control → reserved |
| --- | ---: | ---: | ---: | ---: |
| SATA / 1 | 5725.89 | 10460.37 | +82.69% | 0.172 → 0.093 ms |
| SATA / 16 | 4819.16 | 5670.87 | +17.67% | 0.193 → 0.162 ms |
| NVMe / 1 | 292.41 | 830.57 | +184.05% | 3.382 → 1.177 ms |
| NVMe / 16 | 282.97 | 391.05 | +38.19% | 3.467 → 2.492 ms |

Means combine equal-duration repeats; sync means pool actual call counts,
including recovery barriers. SATA's one-item p99 increases from 225/226 to
277/274 microseconds, despite lower p50/p95 and higher throughput. Larger SATA
records have slightly higher p50 and overlapping p99 (247/250 vs270/250).
NVMe p99 improves in both shapes. Maxima and all individual cells are retained;
this is not an unconditional percentile win or an end-to-end CREATE result.

NUC ASan/UBSan instruments the production WAL, checksum and portable hash code
in the real short-write/ENOSPC/sync-failure/reopen/reservation/tail test. It
passes with leak/error halting. Valgrind runs the separately verified normal,
non-ASan WAL binary: zero errors and zero live heap blocks at exit. These do
not claim full-server sanitizer or physical power-loss acceptance.

The initial native attempt stopped at missing documentation evidence links
because results had been excluded from the source copy. Preserve its exit 2
and subsequent NOTRUN stages in `native-incomplete-copy`; actual linked
evidence was then copied, all five doc checks passed, and the unchanged full
native run passed. No check or failure was suppressed.

[Raw evidence](../../results/measure/20261010-devel-nuc/) retains controllers,
source/binary provenance, operator fixtures, dropped reply frames, all W96
windows and exact replay manifests, native/sanitizer output and the initial
failed copy. Final live/pressure/restoration verdicts will be added here.

## W96 final software acceptance

Both full live modes exit0. Each reports core POSIX214/214, no skip/notrun,
and peer66/66. Direct core takes82.3s, peer273.6s; buffered core88.5s,
peer94.2s. The direct peer mutation phase was briefly observed with an eight-second
strace; these gate times are correctness evidence, not clean performance A/B.
Both modes pass positive sync-write and failed-publication recovery, append
lost-reply/leader-death/exact cold bytes, dirty-stop and active shutdown,
physical GC, second cold restart after malformed WAL-tail repair, empty physical
inventory and zero quota. Full native includes KV/Raft and compaction regressions.

The separate actual follower KV-fsync refusal gate exits0: periodic and shutdown
checkpoint refusal, cold replay of the uncheckpointed durable Raft tail and exact
bytes/nlink pass. WAL ASan/UBSan and Valgrind evidence above remain scoped to the
WAL harness. W96 closes for the documented software gate, without a hardware
power-loss or sustained tiny-file throughput claim.

Exact POSIX TSVs, engine SHA/version manifests, sync/replay/GC/fault JSONs and
logs are in `full-fixtures/{direct,buffered,wal-failure}` under the raw evidence.
Bulk payloads remain in the preserved private remote fixture roots printed in
the stage logs; they are not copied into Git.

## W97 partial allocation controls — NVMe completed

Four uninstrumented eight-minute NVMe cells finish with exit0 and exact
reopened-file verification. Three independent writers each synchronously
persist64KiB per operation. Rates below sum all three writers; first/last
windows are complete30s windows, not the final partial window.

| Order / mode | Whole-run ops/s | First window ops/s | Last window ops/s | First → last per-writer mean sync |
| --- | ---: | ---: | ---: | --- |
| 0 growing | 230.74 | 430.29 | 188.26 | ~6.87 → ~15.82ms |
| 1 ring | 404.08 | 188.23 | 438.00 | ~15.83 → ~6.73ms |
| 2 ring | 573.37 | 506.57 | 438.59 | ~5.83 → ~6.72ms |
| 3 growing | 229.63 | 427.70 | 185.54 | ~6.91 → ~16.06ms |

Both growing repeats decline similarly with no EFS process running. Explicit
truncate/reset time is only0.43–0.53s summed across writers per480s cell; this
does not explain the decline by itself. Ring cells finish faster after their
initial conversion pass, but differ materially in early windows. Their p99s
also differ (~18ms versus~10ms). Growing p99 is~21.4–21.6ms. The Python controls
consume25–39 CPU-seconds over480s, not a saturated CPU core. Physical-device
write/flush averages are separately recorded; they include other host I/O and
are not fdatasync latency or per-call barrier counts.

This establishes a lower-layer reproduction and a useful allocation/overwrite
contrast. It does not isolate XFS journal force from device firmware/cache,
prove a hardware ceiling, or close W97. SATA repeats, separate instrumented
profiles and fully written-ring controls are still running/queued. The original
ABBA program stays frozen on NUC; the written-ring variant is a separate file.

The written-ring follow-up prepares and fsyncs all256MiB per writer outside
timing, using distinct initial counters. Timed writes alter those counters,
and every current block is checked after reopen, including any unchanged
prepared tail. Order is NVMe/SATA/SATA/NVMe; each timed cell is eight minutes.

Regular-service installation is prepared with exact accepted-binary hashes,
old source/binary backup, clean drain, four listeners/two mounts and preserved
payload checks on both mounts. Automatic approval review refuses execution:
it interprets this request as testing authorization rather than explicit
authorization to replace installed binaries. The user's approval is pending;
no installation has run. Private validation remains authorized and continues.

## W97 clean eight-cell matrix completed

All eight cells finish with exit0 and exact current-file verification. The
SATA half repeats the same grow/ring/ring/grow order and geometry:

| Order / mode | Whole-run ops/s | First window ops/s | Last window ops/s | First → last per-writer mean sync |
| --- | ---: | ---: | ---: | --- |
| 0 growing | 5336.77 | 5314.93 | 5384.20 | ~0.546 → ~0.537ms |
| 1 ring | 6916.75 | 6607.08 | 6975.94 | ~0.427 → ~0.404ms |
| 2 ring | 6926.14 | 6696.20 | 6720.09 | ~0.420 → ~0.418ms |
| 3 growing | 5422.36 | 5356.95 | 5356.90 | ~0.540 → ~0.541ms |

SATA p99 is833–838µs for growing and766–776µs for rings. Summed CPU is97–124s
per480s cell, including setup and reopen verification. Explicit growing-file
rotation costs only~2.1s summed across writers. Both volumes retain at least
300GiB physical headroom. Growing files cap their logical lengths at256MiB per
writer; total bytes written across rotations are much larger than retained
file sizes. Raw device sectors and per-operation counts are retained rather
than treating bounded space as bounded write traffic.

Read-only device context identifies Samsung970EVO1TB NVMe and MK0800GEYKE SATA.
The kernel reports NVMe write-back and SATA write-through. NVMe physical flush
counters advance; SATA's do not in these samples. Those observations are
important context for the different durable-call behaviour, but do not prove
firmware guarantees, power-loss protection or NAND-cache exhaustion. Device
settings, APST, cache, filesystem mount options and durability calls are unchanged.

`closure/allocation-summary.json` reproduces the complete matrix. Rings offer
an overwrite benefit here; they are not a compatible production WAL/container
format. Earlier three-node EFS SATA runs also slowed, so stable raw SATA cells
do not establish that EFS's SATA slowdown is repaired. W97 remains open.

## Separate XFS/perf/strace investigation

Four instrumented180s controls finish with exact reopened-file checks. They
are separate from the eight clean480s cells. After90s warmup, each collects20s
of writer-TID tracepoint counts, a separate20s host XFS count,15s of CPU cycles,
then a bounded10s strace. All perf count/record/report steps exit0. The strace
timeout exits124 intentionally after SIGINT detaches the tracer.

| Case | fdatasync calls in writer count | XFS log-force calls | XFS iclog-sync events |
| --- | ---: | ---: | ---: |
| NVMe growing | 3711 | 7422 | 2474 |
| NVMe ring | 9253 | 0 | 0 |
| SATA growing | 106377 | 212758 | 79762 |
| SATA ring | 138616 | 0 | 0 |

Growing files invoke roughly two XFS log forces per fdatasync; warmed rings
invoke none in these windows. These are kernel event invocations, not a count
of distinct drive flushes or per-call serialized barriers. Separate host-wide
counts are close to writer counts for growth and tiny for rings (6 and15
log-force calls), limiting unrelated-XFS-work attribution. Fixed-file reuse
removes this metadata work in the principle test; current file growth still
needs its durability guarantees.

The15s CPU records contain93/165/370/299 samples, no loss, respectively NVMe
grow/ring and SATA grow/ring. Copying/Python evaluation appear in all; XFS
journal CRC work is prominent in SATA growth. The low NVMe sample counts and
CPU-only nature of cycles profiling prohibit a precise blocked-time attribution
or a software/hardware ceiling claim.

The executed controller's strace-c summaries use default accounting, not
explicit wall-time accounting. Supplementary strace-c-w samples are retained
for SATA after the original tracer detaches: across three writers,10s captures
accumulate27.07/28.15s fdatasync wall time for growing/ring, versus0.54/0.69s
pwrite. The host-sampling thread's8.2s sleep is separate. NVMe supplementary
wall traces are explicitly NOTRUN because their exact processes have exited;
their harness monotonic per-sync timing remains available. Never interpret
the original strace-c numbers as syscall wall latency.

The prepared future profiler verifies the last slot's actual counter rather
than a potentially stale30s progress window, and requests wall-time strace
directly. The active controller passes its original warmup checks; no profiling
failure is hidden or manufactured. Exact executed source is retained separately
as `profile-controller-executed.py`; later harness changes are not production
code or a claim they ran in the completed profiles.

These observations strengthen W95's case for an independently specified,
durable packing/reuse experiment. They do not permit skipping file/parent
barriers, replacing checksums/ownership/publication, migrating installed stores
or calling W97 fixed. Fully written-ring controls are the final follow-up.

## Fully written NVMe ring — first completed cell

With every extent already written and fsynced before timing, aggregate rate
still falls1372.53→450.18ops/s. Per-writer sync means rise~2.12→~6.56ms.
Exact reopened checks pass for all12288 current blocks. Physical counter
windows around those complete30s writer windows also change:

| Block counter | First window | Late window |
| --- | ---: | ---: |
| Mean write request elapsed | 1.031ms | 3.212ms |
| Mean flush request elapsed | 1.288ms | 3.490ms |
| Host-written MiB in window | 2574.68 | 844.42 |

Edges use the nearest5s host samples; elapsed windows are30.005/30.006s.
Block counters include queue time and other host I/O. They are not raw NAND
latency. This excludes initial allocation/conversion as the entire slowdown
explanation and demonstrates a lower block-path latency increase too, without
distinguishing firmware cache, device garbage collection, power policy or
kernel/block scheduling. The reversed NVMe repeat remains in progress.

Next W97 discrimination should record block dispatch/completion and drive
health during matched idle/active repeats, then actual direct-I/O fragment and
ancestor-barrier geometry. W95 can test a specified bounded indexed-packing
candidate at equal durability. Neither an unsupported format change nor cache/
power setting changes are adopted from these file controls.

## Final written-ring matrix and service state

All four480s written-ring cells exit0, each verifies12288 current64KiB blocks.
Preparation and initial extent conversion remain outside timing. Rates sum
three independent writers; early/late are complete30s windows.

| Order / volume | Whole-run ops/s | First window ops/s | Late window ops/s | First → late per-writer sync mean |
| --- | ---: | ---: | ---: | --- |
| 0 NVMe | 653.35 | 1372.53 | 450.18 | ~2.12 → ~6.56ms |
| 1 SATA | 7058.49 | 6796.88 | 7114.32 | ~0.430 → ~0.408ms |
| 2 SATA | 6976.79 | 7099.12 | 7100.30 | ~0.411 → ~0.409ms |
| 3 NVMe | 641.61 | 1356.34 | 438.56 | ~2.15 → ~6.74ms |

NVMe's second repeat also increases physical write-request mean1.042→3.310ms
and flush mean1.289→3.593ms. SATA request means stay~0.40–0.42ms, with no
physical flush counter increments. NVMe p99 is~9.9ms and SATA~0.78–0.83ms
for these complete cells. `written-ring/written-summary.json` retains all
intervals, counts, block-counter edges, CPU totals and exact reopen results.
The lower block-path increase repeats on fully written extents. Allocation
adds confirmed XFS journal work, but is not the whole decline. Cache/firmware,
power/thermal and kernel/queue attribution remain unproven; W97 stays open.

Final verification at06:37:08UTC: all four regular listeners, four daemons,
two mounted clients, installed/live binary hashes and16 existing nonempty
payload samples equal on both mounts PASS. This is a bounded final read check,
not a complete before/after inventory. Stores and prior TCP/direct configuration
are retained, with commands/server catalogs disabled as before the experiments.
Normal engine remains installed6b8f05ac, branch macrodevel-persistence-pressure;
private gate engine is3df1931032b8-nuc. No installation is claimed. All task
controllers, private writers and tracers exit; raw fixtures remain for review.

The initial missing-evidence-copy native failure is retained and resolved by
an unchanged full native pass. No final required gate fails. Supplementary
NVMe wall strace remains explicitly NOTRUN as documented above. Future harness
guard/wall-accounting improvements are committed without pretending that the
completed original profiler used them. Production C/header/Makefile contents
on this branch remain identical to captured devel746ae9f0; this session adds
acceptance tests, measurements and documented closures.

No devel merge, push, tag, other-host work, reboot, cache/APST change, device wipe
or installed source/binary replacement runs. Deployment is a concrete prepared
follow-up, pending the user's answer to the automatic-review approval request.
