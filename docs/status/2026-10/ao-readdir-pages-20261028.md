# AO — bounded directory pages

Branch `agent/devel-task-ao`, tested head `2fb508759`: **AO COMPLETE (scoped)**. `TOTAL 552 FAIL 1 NOTRUN 12` (761 s), only untouched-base B6. Evidence: `docs/status/2026-10/ao-readdir-pages-20261028.md`. Native RDMA/tools/attach exclusions; orcd-mini/multi-host NOTRUN (private scope). Private TCP/RDMA and three fixed rounds PASS. No merge/push.

Defect handoff (details and attribution limits below):

- **FIXED:** committed unresolved directory names and absent-bit tombstone visibility.
- **FIXED:** namespace admission using a separate short envelope; real lock conflicts conflated with transient BUSY.
- **INTEGRATED:** reviewed allocator, publication recovery, session/routing and heartbeat/shutdown dependency repairs, with their own regressions.
- **FIXED:** migration planner/log forwarding cycle exhausting the data pool.
- **FIXED:** cross-group cleanup inspecting a foreign KV; both owners now participate in guarded transactions.
- **OPEN:** untouched-base B6 slow-fsync EIO; historical live-error attribution and retained failed stores remain open. Final passes do not explain every prior failure.

## Change and scope

Task AO requires measurement before increasing serial directory-page size. The
parent's fresh permission refresh stays; every RPC starts fresh ReadIndex
coverage. Source/name cookies remain serial. No proof survives a call, no new
product behavior switch is added, and there is no legacy wire fallback.

The implementation uses a current-format request and compact, variable-length
reply: format `0x414f0001`, at most 256 `(ino, type, name)` entries, 69,912 bytes at maximum, below
the fixed 72 KiB wire cap. All receivers, including peer forwarding and mgmt,
validate format, exact length, count, reserved fields, names/types and
request-relative cursor progress before exposing results or reusing a peer
connection. The previous record's unused inode attributes were already zero;
`ls -l` still obtains attributes separately.

A server invocation shares a 32,768-logical-key scan budget across sources,
layout/proof rescans, tombstones, filtered names and lookahead. A forwarded RPC
gets its own cap and fresh authority: this is **not** an aggregate cap across a
forwarding chain or a bound on hidden LSM/device work. Advancing empty pages
are valid; only `done` is EOF. An exhausted, invalidated layout cut returns
BUSY with the original cookie. The client makes at most 16 fresh RPC attempts
within its existing caller deadline; malformed replies and I/O failures are
not retried. Initial permission checks and K48 layout/bitmap/epoch checks stay.

ORCD testing exposed a separate visibility defect, fixed in `2080aadee`:
materialized-only readdir misses a COMMITted destination PUT while that
participant is unresolved and migration's source DEL has materialized. An
already-used lane leaves the bitmap unchanged, so K48 alone cannot catch it.
The serve-side reader now merges bounded intent-name and materialized-name
streams. Intent names are captured first, then base names, covering the atomic
intent-to-base handoff. The merge stops at the smaller full-stream frontier;
an invisible intent cannot skip an unseen name in the other stream. Both
streams share the scan cap; two fixed 256-name buffers bound temporary scan
memory. First-use bitmap publication also gates suppression of the remaining
local copy. Reads never resolve/materialize an intent. Each encountered intent
obtains current coordinator authority, without a cross-call cache.

`37348f212` adds source-first and destination-first migration participant cuts.
A subsequent first-use review found that a hashed tombstone must still mask
a local name even before the lane bitmap is published. `93cdc8d00` preserves
that distinction; its new synthetic cross-group unlink test PASSes, while a
separate library reverting only that handling FAILs exactly that assertion.
The unit tests also cover committed unresolved names, an invisible intent at
the six-key budget frontier and a zero-name advancing page. Linux execution
PASSed, including the assertion that listing leaves stored intents unchanged.
This reproduces a defect matching the live omission class; the failed live
instant was not captured at transaction-key level, so its exact transaction
identity is not established.

The reviewed namespace dependencies are cherry-picked with `-x` source links:
F12 `a6d66a6d9`/`1e187677d`, F13 `4aac24509`, F14 `b91d74923`,
F15 `bac5f6f72`/`fd6160c15`, F16 `6681c5c1d`/`97670beec`,
F10 `02ba70ce7`, F11 `9dc317626` (and append-harness `e8fd647ab`). The F12 conflict omitted only an unavailable
`#if EFS_FAULTS` test hook, not its typed committed-row logic. The first
F16 import lacked its separate follow-up: final CREATE inode reads whose
transaction coordinator is unhosted now delegate to a dual-group member.
F10 selects and reserves MKDIR's allocator watermark inside serialized apply,
rather than repeatedly losing a proposer-side value comparison to ordinary
CREATE. It adds transaction kind 12, for fresh clusters only. F11 gives own
PREPARE/RESOLVE entries the existing progress-renewed apply wait, with the
same two-second cap, rather than treating them as follower ReadIndex waits.
Their deterministic regressions remain; none bypasses commit, durability,
identity checks or a caller's deadline.

A further lock defect is fixed in `8b979a1c2`: FLOCK's wire BUSY had conflated
an actual competing owner with transient ReadIndex/proposal admission. A grant
therefore returned EAGAIN immediately even on an uncontended file. Current
FLOCK replies use **LOCKED 254** for actual `AGAIN` and **LOCK_LIMIT 253** for
`NOLCK`; transient BUSY retries the identical owner/session/range under the
existing attempt/deadline bounds. Nonblocking conflicts and capacity refusals
remain immediate. Actual server classifier, RPC loop, FUSE errno mapping and
current-wire smoke regressions are updated. Positive extracted tests PASS;
new host/RPC tests against prior source FAIL exactly the conflict-classifier
and grant-retry assertions. There is no compatibility path or product toggle.

The loaded phase diagnostic (`namespace-phase-diagnostic/`, observer-enabled)
returned all 160 concurrent-created names in 1.6–2.3 ms across its three rounds;
its last round still failed six other namespace cases. It does not reproduce
or waive the previous concurrent-create timeout. Group 2 unexpectedly elected
a new leader at `07:13:34.311Z`, around the third-round failures. That is
correlation, not proof of each EIO's cause. Reviewed F19/F20 heartbeat/contact
fixes (`ee9748b5b`/`91238d981`) and F30–F32 routing/session/ticket fixes
(`9c54f7c89`/`25246866d`/`da562c23c`, plus harness `b6e809776`) are now included.
They preserve Raft validation and existing caller deadlines, with deterministic
regressions. F19's initial merge excluded upstream shutdown context; integration review
restored its source and real-core regression from `68f8c7fc0` in `ccdaeee1e`.
The pump relinquishes stopping leadership before read/tick work and after
inbox drain, preserving receive-persistence fencing and committed state.
Unrelated upstream K34 fixture edits were not imported. F30 kept this branch's route model while adding the lost-hint case.
No claim is made that a single dependency explains every historical failure.

### Final migration routing and cleanup corrections

Retained stacks and exact five-second RPC/GC stalls exposed a concrete
forwarding cycle: a single-group source leader delegated migration planning
to a dual-group host; once the destination existed, that host forwarded a
`DIR/MIGRATE` log command back to the source leader. Submit interpreted the
same command as planning again. Every recursion occupied another pooled
connection until the 32-slot data pool filled. This mechanism is confirmed
in source and the saved pool-wait stacks; it is not an attribution of every
historical failure.

`4ebfb7754` separates the roles with RPC-only planning tag **104**, exactly
`[tag:1][reserved-zero:1][directory:8]`. Length, reserved byte, nonzero inode
and owning group are validated before planning. Only the planning request
may delegate; the canonical log command has normal leader-only one-hop
proposal/apply semantics. Management uses the same request. The planning tag
cannot enter the Raft log; no fallback, migration mode or user switch exists.
The actual routing/classifier regression checks bounded delegation, one
source apply, malformed frames and refusal propagation. The prior routing
function fails the new boundary oracle; this is a synthetic source control,
not an old-binary/store compatibility gate.

That correction alone still failed one live ENOENT. Review found a separate
unsafe assumption: a source-only log apply cannot decide whether its foreign
hashed destination exists by inspecting its own KV. `434f3e488` therefore
uses byte-guarded transactions for **all cross-group copy and cleanup**.
Copy retains the physical source; cleanup compares both current row images,
deletes only the source and preserves the exact hashed live row/tombstone at
its owner. First-use bitmap OR and sequence accounting remain in the owning
transactions. Serialized local apply re-peeks the current candidate and
refuses foreign-shard work, covering a different first leftover appearing
between planning and apply. It never writes an unhosted shard.

`2fb508759` strengthens the real-transaction regression at COMMIT before any
RESOLVE, both participant orders, COMMIT/ABORT, first-use lanes, tombstones
and stale hashed captures. The oracle uses the existing FUSE name
collection rule: deduplicate source-stream names across pages. An initial
synthetic oracle incorrectly counted these raw duplicates; a later experiment
unnecessarily demanded source retention after atomic bitmap/delete apply.
Both failures and discarded experimental source remain in `bitmap-preflight/`.
The corrected stronger oracle PASSes against the unchanged qualified
`434f3e488` product. No extra product phase from those experiments is retained.
The final successful live stages do not identify the precise transaction
behind each earlier ENOENT/EIO; unresolved historical attribution stays open.

Final ORCD identity is `efsd 434f3e488 (branch agent/devel-task-ao, built
2026-10-30T12:52:45Z on node9901)`, daemon SHA-256
`03a6d4b2e6e4a61e0796541647721828943993bc62347017add01c0593c027f2`.
All seven binary hashes, build commands and focused exits remain in
`cleanup-434f3e488/`. Each RDMA primary/stress result has nonzero application
frames in all five process logs, measured TCP counters, **zero post-setup TCP
application bytes and zero violations**. This proves the private hardware
transport path, not multi-host qualification.

## Measurements

Native host: intel1, eight logical Intel Core Ultra 7 256V CPUs, x86_64,
kernel `6.12.0-211.67.1.el10_2`; full inventory in `environment.txt`.
Private native root: `/home/maik/efs-taskAO-20261029`; retained raw root:
`/home/maik/efs-artifacts/taskAO-20261029`. Three-member loopback TCP, O_DIRECT,
8 GiB/node, packing/query/KV maintenance active. Directories contain
64/1,024/16,384/100,000 empty files. D4 offers four writers × five operations/s,
16 files/writer with overwrite/truncate/rewrite; actual rate is recorded.
Every listing uses a fresh handle and an exact complete-name oracle. All final
acknowledged D4 versions are read back, followed by checked unmount and daemon
stops. No failed operation is retried or discarded.

Identical archive-only probes record page RPCs/latency, actual Raft
`read_round` **starts**, daemon/client CPU, whole-syscall stat at 10 Hz and
Raft observation maxima. Round starts include background work; a same-duration
D4-only control is retained, not subtracted as exact listing-owned rounds.
Stat includes caches. CPU ticks quantize short windows; maxima include
population. Observer-enabled timings are not clean production throughput. Repetitions
use fresh handles within one fixture, with background migration/maintenance
active, rather than independent fresh clusters. Existing production processes
were left alone; the private-task guard does not exclude every external host
workload. The measured spread does not rule out shared-host variation.

| Baseline names | getdents median (range), s | `ls -l` median (range), s | Page RPCs |
|---:|---:|---:|---:|
| 64 | 0.00285 (0.00106–0.00407) | 0.13680 (0.08718–0.67200) | 1 |
| 1,024 | 0.00603 (0.00277–0.00648) | 2.08062 (2.04204–2.38871) | 16 |
| 16,384 | 0.07577 (0.03775–0.08497) | 29.25040 (28.80992–34.59068) | 256 |
| 100,000 | 1.89319 (1.30153–2.26684) | 865.58209 (806.05000–881.64271) | 1,563 |

Baseline product `9c4bfd171`, build `9c4bfd171-ao-probe`, **PASS 24/24**;
106,325 D4 operations, all final versions and stops exact, no workload errors.
Whole-run stat p50/p99: 1.146/62.012 ms. The initial driver lacks later
per-window raw stat fields; do not claim they exist in its capture.

The archive-only 256-entry counterfactual (`a2c6ccf4b`, product base
`9c4bfd171`, build `9c4bfd171-ao256-probe`) **PASSed 24/24**, 118,204 D4
operations, exact final versions and checked stops. Its 100K getdents median
was **0.8625 s (0.2091–1.0010)**, outside the entire baseline range, with
**391 RPCs rather than 1,563**. That justified the bounded implementation.
It did **not** improve `ls -l`: 100K median **931.9409 s (923.2084–980.6644)**;
16K median **39.6884 s**, versus baseline 29.2504 s. These regressions are
retained. Both runs had group-0 leader 1; group-2 leader differed (3 versus 2),
and directory inodes differed while remaining in group 0. Actual offered work
was about 19.95 ops/s. A page-size change alone is not established as the
cause of the attribute-dominated regression.

The superseded pre-intent-fix confirmation (`9855f7386-ao-probe`) was stopped
with SIGINT during a background-only control after **22 completed listings**.
The driver verified all final D4 versions and cleanly stopped its processes.
Its raw verdict remains FAIL/KeyboardInterrupt, exit 130, explicitly recorded
as an intentional incomplete measurement, never a final performance PASS.
Both completed 100K getdents were 0.17195/0.58578 s, 391 RPCs; two `ls -l`
results were 1,045.43/1,104.08 s. The corrected `6f9f0a945-ao-probe` matrix completed **24/24 PASS**, with
79,781 D4 overwrite/truncate/rewrite operations, all 64 final payloads exact,
zero workload/name errors and checked shutdown:

| Names | getdents median (range), s | `ls -l` median (range), s | RPCs |
|---:|---:|---:|---:|
| 64 | 0.00305 (0.00101–0.00662) | 0.15526 (0.06612–0.34665) | 1 |
| 1,024 | 0.00552 (0.00205–0.04793) | 2.19614 (1.70512–2.75841) | 4 |
| 16,384 | 0.04636 (0.01900–0.13629) | 35.61097 (35.36162–38.33445) | 64 |
| 100,000 | 0.49911 (0.44631–0.55880) | 633.54636 (589.05488–665.00055) | 391 |

The 100K enumeration median is **3.79× faster**, its entire range below the
baseline minimum. Smaller-directory timings overlap; no universal speedup is
claimed. The 16K `ls -l` median is still slower than baseline, while 100K is
faster. Attribute work, leadership, migration and shared-host variation
prevent attributing those `ls -l` differences to page size alone.

For 100K getdents, baseline → corrected median page p99 is 29,233 → 14,637 µs;
cluster round starts 3,880 → 1,001. Median CPU seconds for daemons 1/2/3 and
FUSE are `(1.02, 0.86, 1.08, 0.15)` → `(0.10, 0.24, 0.20, 0.07)`.
For 100K `ls -l`, page p99 is 7,235 → 9,468 µs, round starts
1,847,839 → 1,773,439, and CPU `(516.48, 347.33, 447.02, 44.46)` →
`(268.43, 428.98, 349.00, 38.92)`. These are per-run medians, not a single
joint sample or exclusively listing-owned work. Corrected whole-run stat
p50/p99 is 1.424/62.848 ms; 100K getdents windows have median p99 36.548 ms
but only a few 10 Hz samples. Corrected per-node pump/lock-wait maxima are
175.661/11.547, 180.558/24.362 and 279.720/27.826 ms; baseline maxima are
226.921/26.235, 244.711/4.066 and 251.503/42.130 ms. There is no demonstrated
uniform hold/wait improvement. All raw rows and aggregate JSONs remain.

This full matrix precedes the first-use tombstone and namespace dependency
repairs. The timed positive directories already have their used-lane bits;
the tombstone change targets the absent-bit deletion cut. An earlier 12-case
getdents-only confirmation on `96cf53f46-ao-probe` completed separately:
**12/12 PASS**, zero listing/workload/payload/cleanup errors. Its medians for
64/1,024/16,384/100,000 are 0.00104/0.00235/0.02181/**0.70905 s**,
with 1/4/64/391 RPCs. All three 100K results (0.67838–0.75524 s) remain below
the baseline minimum: **2.67× median enumeration improvement**. The then-current `2bcfc2d1b` product differed only in shutdown demotion,
outside those timed listing paths. Later routing/cleanup changes do affect
listing and migration; their separate final confirmation is reported below.
No confirmation replaces the full matrix or invents new `ls -l` results.

The `96cf53f46` confirmation details: 303 D4 overwrite/truncate/rewrite operations,
64 final payloads compared byte-for-byte, no uncertain/error outcomes.
At 100K, median page p99 is **51.500 ms** (11.917–52.791), versus baseline
29.233 ms (9.254–31.681): the newer run's page-tail median is worse despite
faster complete enumeration. There is no blanket tail-latency improvement.
Median cluster read-round starts are 1,076 (baseline 3,880); per-daemon/FUSE
CPU seconds are `(0.45, 0.25, 0.22, 0.12)`. Whole-run stat p50/p99 is
1.235/53.963 ms; the short windows and different run duration limit comparison.
Pump/lock-wait maxima for nodes 1/2/3 are 329.526/24.487,
152.021/21.868 and 160.568/1.759 ms, including population. The earlier
corrected full matrix's lower page tail is not substituted for these results.

### Final-source enumeration confirmation

Fresh `2fb508759-ao-probe` (built on intel1 at
`2026-10-30T13:17:25Z`) completed **12/12 PASS** after the final routing and
cleanup corrections. `final-enum-2fb508759/` and
`qualified-2fb508759/measurement-summary.json` retain every case, name
oracle, probe source/binary hashes and checked shutdown. All 64 final D4
payloads matched byte-for-byte; 316 overwrite/truncate/rewrite operations,
zero creation/workload errors or uncertain outcomes. No overlapping
private-task work was detected. The 100K timed windows delivered 18.77–20.35 operations/s
against 20 offered; the short smaller windows often contain no write or
stat sample and cannot establish concurrent per-page tails.

| Names | Final getdents median (range), s | RPCs |
|---:|---:|---:|
| 64 | 0.00100 (0.00097–0.00113) | 1 |
| 1,024 | 0.00222 (0.00215–0.00912) | 4 |
| 16,384 | 0.06074 (0.05326–0.07223) | 64 |
| 100,000 | **0.73534 (0.63852–0.83453)** | **391** |

The 100K enumeration median is **2.57× faster** than baseline 1.89319 s,
with all three results below the baseline minimum. Median page p99 is
**49.675 ms (8.364–53.244)** versus baseline 29.233 ms: **worse**, despite
faster full enumeration. Median cluster round starts are 1,122 versus 3,880;
per-node/FUSE CPU seconds `(0.15, 0.38, 0.29, 0.15)`. These starts include
background work, not solely listing proofs. Whole-run stat p50/p99 is
5.863/111.438 ms; 100K windows contain only 6–8 stat samples each, with
median window p99 16.982 ms. Neither statistic establishes a tail improvement.
Pump/lock-wait maxima including population are 203.307/2.775,
129.979/29.319 and 236.894/13.875 ms. There is no uniform hold/wait win.
The earlier full `ls -l` matrix and its 16K regression remain stage-specific;
no final-source `ls -l` timing is invented.

## Qualification and every material failure

| Stage | Verdict and scope |
|---|---|
| Initial compact build `68ee39b3c` | BUILD FAIL: missed mgmt caller fields; fixed `bdc3f4185`, log retained. |
| Initial native `bdc3f4185` | `TOTAL 546 FAIL 1 NOTRUN 12`, 781 s; only B6 slow F0 fsync EIO. |
| Untouched base `9c4bfd171`, intel1 | Same standalone command fails F0 EIO at 60.08 s; same full gate `TOTAL 546 FAIL 1 NOTRUN 12`, 782 s, F0 EIO at 60.06 s. |
| Pre-correction native `9855f7386` | `TOTAL 547 FAIL 1 NOTRUN 12`, 708 s; same B6. |
| Final product `2bcfc2d1b`, native full gate | `TOTAL 548 FAIL 1 NOTRUN 12 STATUS FAIL (740s) OVERBUDGET 7 BUILD 11s`. Only baseline B6 F0 EIO, 60.07 s. All new regressions and copied main AO result PASS; focused metadata/txn/Raft, admission, lock, heartbeat, shutdown, K48 and W130 captures exit 0. |
| SSH intermission | Final live qualification was temporarily NOTRUN while the shared master expired. Access was restored; the resumed stages below retain their own exact outcomes. |
| Liveness native `96cf53f46` full gate | **548/1/12 FAIL**, 722 s, seven OVERBUDGET; only the same base-confirmed B6 F0 EIO 60.06 s. AO, metadata, lock, liveness and harness checks PASS. |
| Liveness enumeration + retained focus | **12/12 PASS** on all four sizes; D4 final payloads and checked stops exact. Focused regression, K48/W130 and copied main-gate AO evidence retained successfully. |
| Liveness source `96cf53f46`, native fast preflight | **455/0/11 INCOMPLETE**, 13 s (exit 3); zero failures, all eleven NOTRUNs verified as explicit host exclusions. Continued to full gate without changing that verdict or rerunning the preflight. |
| Native `22d34f24e` / `055e4c893` | **548/2/12 FAIL**, 815/695 s: baseline B6 plus one extracted append-harness compile failure after F11. `e8fd647ab` updates its stub to the own-entry wait, preserving every batch assertion; focused test PASS. |
| Completed F10/F11/F16 `22d34f24e`, ORCD | Primary TCP/RDMA **218/218 PASS** each. Fixed three-round RDMA **FAIL**: 216/218, 218/218, 218/218; first round uncontended `fcntl_lock_basic`/`flock_unlock_on_close` EAGAIN, 15.0/14.4/26.9 s. |
| Lock-corrected `055e4c893`, ORCD | Positive/negative focused regressions PASS; primary TCP **217/218 FAIL**, unchanged 15 s concurrent-create timeout. RDMA and three-round stages not started. |
| Namespace phase diagnostic | **FAIL**: 218/218, 218/218, 212/218; last-round name timeout and five namespace EIO/wrong-errno cases. Observer-enabled, never clean qualification. All three observed concurrent-create cases finish exact enumeration in 1.6–2.3 ms. |
| Final admission native `7d355d5c1` | **547/1/12 FAIL**, 786 s, five OVERBUDGET; only base-confirmed B6. All three harness repairs PASS. |
| `7d355d5c1` primary ORCD TCP/RDMA | Both all seven AO checks and **218/218 POSIX PASS**, 20.370/30.538 s. RDMA application traffic in all five logs; zero post-setup TCP bytes/violations. |
| `7d355d5c1` fixed three-round unprobed RDMA stress | **FAIL**: 218/218, 218/218, then 215/218 (20.4/8.5/53.3 s). Last round: two 15 s name-test timeouts and `names_control_and_high_bytes` EIO. No retry or waiver. |
| F10-only `9f41e53ea` ORCD | TCP PASS; RDMA AO checks PASS but POSIX **211/218 FAIL**: namespace EIO and unchanged name/create/mtime deadlines. Three-round stage not started. F10 alone is insufficient. |
| F10-only native stage | **INTERRUPTED/INCOMPLETE**, not a gate result. A supersede assertion detected active runtime, but a non-fail-fast shell continued and stopped its controller. Closed only owned child writers/mounts/daemons normally; initial unmount refused active writers, no force-discard used. Interruption and cleanup journals retained. |
| Dependency native `7fe39669b` | **547/4/12 FAIL**, 731 s: same B6 plus three extracted-harness link/compile failures (missing deadline API and bootstrap includes). Fixed in `e565fb892`; failed logs retained. |
| Repaired receipts `7fe39669b` ORCD | Focused units PASS; TCP AO PASS, POSIX **217/218 FAIL**, concurrent unlink EBUSY. RDMA not started after TCP failure. |
| Corrected native `6f9f0a945` | `TOTAL 547 FAIL 1 NOTRUN 12 STATUS FAIL (753s) OVERBUDGET 6 BUILD 10s`; same B6 F0 EIO 60.06 s, no new failure. |
| Readdir/metadata/split gates on corrected native | PASS: permission, reply/client+peer framing, collection, source cookie; metadata 1.5 s, K48 6.8 s, W130 26.6 s, AO live 15.4 s. |
| ORCD final `2bcfc2d1b`, TCP | **215/218 FAIL**, 62.0 s POSIX / 80.60 s total. Six AO checks PASS; fsync_then_fstat_size EIO and names_crazy_roundtrip/names_crazy_dirs unchanged 15 s timeouts. All four daemons stopped orderly. RDMA/fixed rounds NOTRUN after failure; `complete-2bcfc2d1b/`. |
| Recovery integration `ba42a7968`, ORCD TCP | **215/218 FAIL**, 48.4 s POSIX. Focused 23 gates PASS, including actual pin-owner/priority scheduler tests; all six AO checks PASS. Concurrent_create_unlink_two_proc EIO, concurrent_creates_same_dir ENOENT (`t2-0`) and mtime 15 s timeout. RDMA/fixed rounds NOTRUN after failure; `recovery-ba42a7968/`. Native full gate 550/2/12 FAIL (803 s): base B6 plus imported orphan budget row, fixed in 6b2cc49f1. |
| Migration integration `7bcf9e33a` | TCP/RDMA primaries PASS (218/218); fixed RDMA rounds **FAIL [1,1,1]**: 216/218, 179/218 (30 NOTRUN), 205/218 (7 NOTRUN), 72.1/26.7/77.5 s. Initial mount stop FAIL; later checked unmount and four orderly stops PASS. Observer stacks were attached only after failure; later outcomes may be perturbed. |
| Matched public-VFS control | Untouched product base 9c4bfd171 **FAIL [0,1,1]** (218/216/216); candidate 7bcf9e33a **FAIL [0,0,1]** (218/218/195, final 16 FAIL/7 NOTRUN). Same fixed workload without AO-only pause hook on both, own wire/binaries. Base has EIO/EBUSY/15 s timeout; candidate has additional timeout/worker-kill cascade. This is not a blanket waiver or compatibility test. `public-control-20261030/`. |
| Planner/log separation `4ebfb7754`, ORCD TCP | **217/218 FAIL**, 15.0 s POSIX / 27.58 s total: trunc_zero_then_high_pwrite ENOENT. Focused 26 PASS and six AO checks PASS; RDMA/fixed rounds NOTRUN. `route-4ebfb7754/`. |
| Guarded cleanup `434f3e488`, ORCD | **PASS scoped**: focused 27; TCP and RDMA each all seven AO checks and unchanged POSIX **218/218**, 23.21/25.88 s total. Three fixed unprobed RDMA rounds **218/218 each**, 17.3/48.8/44.2 s POSIX, 132.25 s stage, checked stops. `cleanup-434f3e488/`. |
| Native integration `6b2cc49f1` | **551/1/12 FAIL**, 791 s, OVERBUDGET 6, BUILD 31; only base B6 F0 EIO60.09 s; orphan unit budget corrected. |
| Native planner source `6f3e1f1c3` | **552/1/12 FAIL**, 726 s, OVERBUDGET 5, BUILD 31; only base B6 F0 EIO60.06 s. |
| Final native `2fb508759` | **552/1/12 FAIL**, 761 s, OVERBUDGET 3, BUILD 31. Only base B6 F0 EIO60.06 s; all current code regressions passed. Separate fresh enumeration confirmation **12/12 PASS**; exact payloads and checked stops. |
| Initial live driver attempts | Harness failures: unstarted node-4 log lookup, already-renamed pause-file unlink, stale non-host voter-mask assumption. Fixed without weakening product checks; captures retained. |
| ORCD `d3f248a0c`, TCP | AO checks and unchanged POSIX **218/218 PASS**. |
| ORCD `d3f248a0c`, RDMA | **FAIL 217/218**: create/unlink child `os.unlink(c0-11)` returned EBUSY. |
| Original RDMA error-only diagnostic `591ef117c-busyprobe` | **FAIL** before POSIX: immutable `stable-0040` missing after initial exact 273-name listing. Churn never changes stable names. |
| Tombstone-corrected `dd31ef14c` ORCD | Unit/negative-control PASS; TCP 218/218 PASS; RDMA **215/218 FAIL**: content-directory EIO, dangling-symlink link EIO, unchanged mtime 15 s timeout. Stores/logs retained under `tombstone-dd31ef14c/`; no waiver. |
| Corrected unprobed ORCD `2080aadee`, TCP and RDMA | **PASS**, each all AO checks and POSIX **218/218** at unchanged deadlines. RDMA elapsed 22.975 s. |
| Untouched-base public-VFS RDMA control | **218/218 PASS once**; this alone did not waive the original EBUSY. Only current-wire mgmt forwarding was excluded; no old-format encoder/fallback. |
| Base error-only stress, 20-round maximum/stop-first | **FAIL round 0**, 217/218: `dir_many_files` unlink EBUSY; client probe identifies admission at the 16-position receipt window. No later rounds. |
| Corrected intrusive stress, 20-round maximum/stop-first | Round 0 **218/218 PASS** in 39.5 s; round 1 **216/218 FAIL** in 83.4 s: unchanged 15 s mtime and 75 s directory deadlines. No later rounds. |
| Fixed three-round base intrusive comparison | **218/218 each**, 9.8/9.4/8.3 s. |
| Fixed three-round candidate intrusive comparison | **FAIL each**, 217/211/212 of 218: flock EAGAIN; namespace/mtime timeouts; later namespace EIO and unlink admission BUSY. Every failure retained. |
| Raw-scan negative control, compact wire unchanged | **FAIL before POSIX**: immutable `stable-0055` missing. Only the serve-side call used the known-unsafe raw scan; never a shipping mode. Full returned names retained. |
| Fixed three-round corrected unprobed comparison | **218/218 each**, 8.2/9.2/8.4 s. |
| Fixed three-round corrected client-only probe | **218/218 each**, 16.1/8.4/9.1 s. No server logging added. |

Earlier native identity: `efsd 2bcfc2d1b (branch agent/devel-task-ao, built
2026-10-30T08:02:10Z on intel1)`. Source archive SHA-256:
`b79029c140badd7e34ac5838f2eaf92c83da080f1c9f869ccf6042a97875baeb`.
The efsd binary SHA-256 is
`fe1f51925040c8530e27a7db612a2e4c8260590f6640d53667d8308d143db4b9`;
all four daemon/client/mgmt/fault binary hashes are retained in
`gate-2bcfc2d1b/binaries.sha256`. Build logs retain compiler/link options.
Later native/ORCD integration stages and their exact identities are listed separately below; the original measurements are never reassigned to a newer build.

Final native full-gate identity: `efsd 2fb508759 (branch agent/devel-task-ao,
built 2026-10-30T13:04:13Z on intel1)`, daemon SHA-256
`7888f3041b480929b0507a5c23e279334142dd242c9f25ef9444366cd02f1088`.
Archive SHA-256:
`f75bf0de5a910ade4df12e9a185d9cf9dfebe6ce666d0f31a32744959c2a1fd8`.
`qualified-2fb508759/` retains all four binary hashes, compiler/link logs,
summary, per-command timings and closed failures. The full gate reports
**TOTAL 552 FAIL 1 NOTRUN 12 STATUS FAIL (761s) OVERBUDGET 3 BUILD 31s**;
only B6 F0 fsync returned EIO after 60.06 s, matching the untouched base.
This is a scoped AO acceptance exception, not an overall suite PASS.

Native NOTRUN is limited to absent external cluster lifecycle scripts,
valgrind headers, usable IB hardware and a sanitizer attach restriction.
Earlier OVERBUDGET rows are retained in each raw summary. The 96cf53f46
run has seven: K33, K35, stop-progress, operator workflows, K43, expanded
opid admission and K22. The opid test now includes three actual-public-call
frontier regressions; its reporting budget is updated from 15 to **25 s**
using loaded 16.9 s × 1.25, rounded to five seconds (`af1a0c938`).
The old OVERBUDGET result remains unchanged; this edits neither a product
nor a regression-test deadline. Final 2bcfc2d1b's seven recorded overruns are
K33 growth, K35, stop-progress, K43, opid admission, K22 and KV LSM. AO live
itself takes 24.1 s within its 30 s budget; K48 12.9 s and W130 28.9 s pass. The AO live row has a 30 s budget and three
disk slots, derived from an earlier measured loaded 20.5 s × 1.25 rounded up
to five seconds; no test deadline was enlarged. The measured full-gate totals (including final 761 s) remain below
D44's 1,100 s target. This is **not an overall make-test PASS**. Final-source overruns remain K35 fsync degraded (153.4/80 s), K43 coordinator kill (55.4/25 s) and K22 leader admission (40.5/30 s); reporting budgets and runtime deadlines were not relaxed for these results.

### Failure classification

- **Stable-name omission: FIXED.** Deterministic participant cuts and the
  compact-wire raw-scan negative control substantiate the fix. The original
  failing listing did not retain its full name array; subsequent runs do.
- **Receipt-window admission BUSY: pre-existing defect, addressed and qualifying.**
  The original uninstrumented RDMA log has `03:01:41.980Z opid-admission: BUSY
  oldest=5027 next=5043 live=4 window=16` in the failure interval. The base
  diagnostic (`scripts/evidence/ao/ao_busy_probe.py`) identifies admission for unlink `f0085`, oldest 4630/next 4646/
  live 3. Reviewed F12–F16 typed committed-row/receipt, proof-lock and session
  bootstrap repairs are imported with their original source references.
  `6c8bdc72b` also makes public UNLINK/RMDIR/LINK scope admission and RPC to
  the **existing configured normal I/O envelope**, just as CREATE now does.
  It preserves inherited deadlines, one operation identity and the same
  receipt window; no regression-test deadline is enlarged. The old public
  calls instead used an independent one-second admission fallback, before
  the RPC's normal 30-second envelope. A deterministic actual-public-call
  test releases a healthy oldest owner at 1.25 s and requires exactly one
  successful RPC for each operation. An inherited 20 ms deadline must fail
  BUSY without allocating/sending an identity and without leaking pins.
  Positive Linux test PASS; the same test against previous ops FAILs exactly
  the shared-envelope assertion (`namespace-budget-preflight/`). This proves
  that admission defect, not the exact cause of every earlier live EIO.
- **Intrusive stress failures: preserved; attribution limited.** The old
  diagnostic added stdio to `rc_to_inode_status`, including callers under
  Raft ownership. `32aa08e76` removes that instrumentation; only client-side
  failure stages remain. Its transform proves server bytes unchanged.
  Matched unprobed/client-only runs pass, supporting observer interference,
  not proving every failed operation's cause. A later uninstrumented
  `dd31ef14c` run also failed namespace EIO and mtime, so logging is not a
  sufficient explanation for the product stalls. Reviewed devel F12–F16
  namespace receipt/bootstrap repairs and shared public admission envelope passed
  primary TCP/RDMA on `7d355d5c1`, but its third fixed stress round failed.
  The incomplete F16 dependency was corrected and F10/F11 passed both
  primary transports on `22d34f24e`, whose fixed stress found lock refusals.
  The F10-only stage also failed, so allocator starvation alone does not
  explain the live failures. The earlier liveness/shutdown `2bcfc2d1b` is qualified
  natively with the base B6 exception, but its resumed ORCD TCP gate failed
  215/218. Its logs show a group-2 election during the early namespace load
  and repeated coordinator-lost recovery work. The branch lacked the reviewed
  decided-pin owner and observed-shard priority repairs: cold discovery could
  put a known high-shard intent behind the 64-shard-per-pass walk. Those
  dependencies are now integrated with their actual scheduler regressions.
  This is a concrete code defect and a plausible stall contributor, not yet
  proof that it explains each live timeout or EIO.
  Do not count failed intrusive
  runs as green or claim their timeouts are all demonstrated on base.
- **B6 slow fsync: pre-existing, OPEN.** The same F0 fsync EIO remains on
  final code despite the separately required shutdown integration. No
  durability relaxation is included; this baseline exception is not a PASS.

The diagnostic CPU capture covers only five private processes during 12.717 s
of the last failing round: 22,507 samples, zero lost. Recording exited 143 as
the targets stopped; the file is readable and report recovery succeeded.
Locks/RDMA dominate sampled CPU; absence of a hot readdir stack does not
exclude off-CPU stalls. It is not clean product timing evidence.

## Environment, reproduction and retention

Intel1 is the assigned native host. ORCD is a separate private single-host
stage on node9901, TCP `10.1.99.1`, RDMA `172.16.99.1`, `mlx5_0`, four members
by the forwarding check, O_DIRECT. Actual node9901 inventory reports 192
logical CPUs, EPYC 9474F, about 395 GB RAM, kernel
`5.14.0-503.38.1.el9_5.x86_64`, fd limit 1,024 and unlimited memlock; the
brief's older eight-CPU description is not the observed machine.
Primary ORCD fixture quota is 1 GiB/member; fixed stress uses 8 GiB/member.
Private libfuse: `/data1/efs/deps/libfuse`. Only private 19xxx ports and owned
paths were used. Production 17432–17435 daemons and `/data1/efs/mnt` were
untouched. Final process/mount inspection records those same production process identities after all private tests and cleanup; this is not a production health gate.

Archive the named commit into a fresh private source tree. On intel1:

```sh
EFS_GIT_ID=2fb508759 EFS_GIT_BRANCH=agent/devel-task-ao \
 EFS_GIT_DESCRIBE=2fb508759 make -j8 all tests/faults/efsd
TMPDIR=PRIVATE/tmp EFS_GIT_ID=2fb508759 EFS_GIT_BRANCH=agent/devel-task-ao \
 EFS_GIT_DESCRIBE=2fb508759 EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 \
 EFS_BUILD_JOBS=8 scripts/make_test_summary.sh
```

For each measurement, instrument a **different archived copy** with
`scripts/evidence/ao/ao_instrument.py`, then build and
run `scripts/evidence/ao/ao_readdir.py --work NEW_FIXTURE --output NEW_EVIDENCE`.
Defaults cover all four sizes and three repetitions of getdents/`ls -ln`.
Use `scripts/evidence/ao/ao_summary.py EVIDENCE` for aggregates. Linux pidfd, FUSE,
fusermount3 and a C compiler are prerequisites. The 1,200 s listing watchdog
is a measurement bound, not a changed product or regression-test timeout.

For ORCD, build a separate native archive with the private FUSE installation;
use only an unused private 19xxx range (check listeners first):

```sh
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse
export PKG_CONFIG_PATH="$EFS_FUSE_PREFIX/lib64/pkgconfig"
export LD_LIBRARY_PATH="$EFS_FUSE_PREFIX/lib64"
export PATH="$EFS_FUSE_PREFIX/bin:$PATH"
EFS_GIT_ID=2fb508759 EFS_GIT_BRANCH=agent/devel-task-ao \
 EFS_GIT_DESCRIBE=2fb508759 make -j4 \
 FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$EFS_FUSE_PREFIX/lib64" \
 efsd efs-fuse efs-mgmt tests/faults/efsd
EFS_RDMA_DEV=mlx5_0 python3 scripts/evidence/ao/qualify_ao_readdir.py \
 --work NEW_FIXTURE --output NEW_EVIDENCE --transport rdma \
 --bind 172.16.99.1 --base-port 19381 --posix
```

The driver checks exact stable names and payloads under same-directory churn,
a row-cut pause spanning a real leader change, one halted replica, addition
of member 4 and variable replies forwarded through partial-group hosts.
`--posix` runs unchanged core/`names_crazy_roundtrip` at default deadlines.
Use `--transport tcp --bind 10.1.99.1` and a separate range for TCP. Long jobs
run fully detached in screen. Current diagnostic helper generates a bounded
20-round, stop-first-failure driver in an archive; it adds no server probe.
The historical intrusive version is reconstructible from `591ef117c`.

Reproduce the exact fixed three-round diagnostic from committed source:

```sh
python3 scripts/evidence/ao/ao_three_rounds.py "$PWD"
EFS_RDMA_DEV=mlx5_0 python3 scripts/ao_fixed_three.py \
 --work NEW_STRESS_FIXTURE --output NEW_STRESS_EVIDENCE \
 --transport rdma --bind 172.16.99.1 --base-port 19861 --posix
```

The generator's default output SHA-256 is
`d071b2dddb529df82d4373653e66801f221b1f30db3c1d60c749c98c4d001543`,
matching the retained unprobed driver. All three default-deadline suites run;
any failed round keeps final FAIL. Primary qualification separately covers
the management forwarding block. For a matched public-VFS control, add
`--public-vfs-control` to generation on **both** source trees; it omits only
the AO-only row-cut hook. Copy the committed `qualify_ao_readdir.py` template
into the disposable base's scripts directory first, without changing base
product code. Each cluster builds/uses its own binaries and wire. These
controls diagnose shared failures and cannot replace the real AO gates.
No legacy protocol is implemented. Exact generated drivers, commands, hashes
and method manifests remain with every run.

ORCD raw root: `/data1/efs/evidence/ao-20261029/`. The **first-stage**
48,707-file manifest, now `SHA256SUMS.stage1`, was finalized and verified; SHA-256:
`b8bdaa2298057b2f72addaf2b2ea53a1ec7367dd24f531539aa033096f920eea`.
The first-stage private `/tmp/efs-taskAO-20261029` is removed; postflight confirms no owned
process or mount. `PRUNED.txt` lists deleted successful/fixed-defect fixtures,
source/build trees, archives and runner scratch. Four closed failed fixtures
remain as `kept-*` bulk exceptions for OPEN admission/observer-attribution
investigations, with full copied-file verification and path journal
`RETAINED-STORE-MOVEMENTS.json`. `/tmp` and evidence are different filesystems;
the initial rename refused EXDEV without moving anything, then on-host copies
were hash-verified before deleting sources. No stores were exported to the Mac.
Profiler data and the build-ID symbol archive remain. A separate report using
only the extracted archive resolved private symbols after original binaries
were deleted; the temporary symbol cache was also removed.

Original `d3f248a0c` POSIX did not pass `--keep`, so its suite removed its own
logical test fixture even on failure. Logs/post-cleanup stores remain; exact
failing namespace replay was forfeited. Its stable AO fixture was outside
that cleanup. Later runs use `--keep` for retention only. Exact runtime-store
replay of the now-fixed omission/negative-control fixtures is also forfeited
by pruning; committed deterministic regressions and saved names/logs remain.

The later private `/tmp/efs-taskAO-tombstone-20261030` is now **removed**.
After SSH access was restored, all new qualification above completed before
finalization. Twelve additional failed fixtures were copied on host with every
regular-file hash, directory and symlink target compared before deleting their
sources. The original four kept fixtures remain. These are explicit **OPEN
historical-attribution bulk exceptions**; the final passes do not prove the
cause of every old EIO, ENOENT, timeout or interrupted cleanup. See
`FINALIZE-orcd-movements.json`. No failed result was relabelled PASS.
`PRUNED.txt` records discarded reproducible builds, successful/fixed-defect
fixtures and runtime-replay limits. The original manifest is retained verbatim
as `SHA256SUMS.stage1`.

Final ORCD manifest: **329,245 regular files and 48 symlinks**, SHA-256
`69ce914646808ddd36c976121a60535e2b5e4eb2929c51da9a87000ba80e9684`.
Every regular-file hash and existing internal symlink target was verified;
archives were checked readable before scratch deletion. The final closed
cleanup log and exit marker are included; only the current manifest, its
checksum and verification log are excluded. The separate finalizer socket
directory was removed after its screen exited. Postflight records no owned
runtime or mount and the unchanged production process identities, not a
production health/deployment gate.

The first native finalization completed before the resumed qualification. Retained root:
`/home/maik/efs-artifacts/taskAO-20261029/`; that historical manifest covered **5,371
regular files and 33 recorded symlinks**, SHA-256:
`75420c58389405760bc5f93f6bfa69e5dc643255b0c71cab5dddb0c9b015ff52`.
Every regular-file hash and symlink target was verified; all link targets
exist inside evidence. Only the current manifest, its checksum and the
self-verification log are excluded. Captures and finalizer output are closed.
Eight late B6 fixtures were atomically moved on the same filesystem into
`kept-b6-late/`; the three earlier base/candidate fixtures remain. These are
OPEN B6 bulk exceptions, traced by `FINALIZE-native-movements.json` and the
earlier movement journal. Interruption logs remain; bulk from interrupted,
successful and now-reproducible fixed-defect fixtures is pruned, so runtime
replay of those pruned fixtures is explicitly forfeited. `PRUNED.txt` records
this. Checked `edelete` removed only `/home/maik/efs-taskAO-20261029`;
`FINALIZE-native-postflight.json` records no owned daemon, client or mount.
No raw bundle was committed or exported from the hosts to the Mac.

Resumed native finalization is also **complete**. Four additional closed B6
fixtures were atomically moved to `kept-b6-resumed/`; the original exceptions
remain. `FINALIZE-native-resumed-movements.json` records every destination and
verified text retention before pruning runner bulk. The old 5,371-file seal
is preserved verbatim as `SHA256SUMS.native-first`. Current closed manifest:
**6,811 regular files and 45 symlinks**, SHA-256
`93a9189371865473366b1dc7a7492e56430e875b3e069709ae70a3c9ada50705`.
All regular hashes and existing internal link targets were checked, archives
opened before deletion, and closed finalizer logs/exit markers are covered.
Only the manifest, its checksum and verification log are excluded. Checked
`edelete` removed `/home/maik/efs-taskAO-recovery-20261030` and the exited
finalizer's empty socket directory. Postflight has no owned runtime/mount.
Production clusters were left untouched. OPEN B6 bulk stays on intel1;
replay of pruned synthetic stores is explicitly forfeited in `PRUNED.txt`.

A local macOS
C build attempt failed on unsupported GNU `fopencookie`; Linux supplies its
acceptance. Its logs remain under
`/Users/mike/Documents/Codex/artifacts/taskAO-20261030/local-preflight/`.
Extracted lock positive/negative controls are additionally retained under
`/Users/mike/Documents/Codex/artifacts/taskAO-20261030/lock-status-preflight/`,
manifest SHA-256 `99f02f9d11971ae12b733ed4b76b4b53635321c5772252aeff01f4ce7522fab0`.
Those are synthetic source controls, not old-binary compatibility gates.

Canonical graph and **9/9 tests PASS** after the final code changes. The
protocol parser warning, 49 files without extracted symbols and unresolved
call targets remain graph coverage limitations, not runtime proof. Final docs
check is **5/5 PASS**. **AO is complete in its private qualification scope**,
with the brief's independently reproduced baseline B6 exception and explicit
native host exclusions. The full native suite remains FAIL. Private ORCD
acceptance, retained evidence and checked cleanup are complete. orcd-mini
deployment, multi-host RDMA, merged-tree and alpha acceptance remain
NOTRUN/outside this branch's private single-host stage.
