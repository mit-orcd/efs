# Task H K38/B6 continuation — October 28, 2026

> Migrated from `agent/task-h-repair` at `cd822e2f7` on Oct 30, 2026, by Task BO. Nothing was shortened: the page already held conclusions, methods, hashes and raw-capture locations only. Scripts and tests it names in backticks exist only on that branch; see [the migration page](task-h-evidence-migration-20261030.md).

**Current reconciliation:** [devel wave 33 and the per-fix inventory](task-h-wave33-reconciliation-20261029.md) supersede this page's branch-head and acceptance recommendation; all evidence below remains historical.

Branch `agent/task-h-repair`; product recovery fix `20c0a2ebe`, followed by
read-routing fix `bddd7376c` and shutdown-demotion fix `85962d8cd`.
**Final repaired-product qualification PASS; attempts 97/98/100 and the nuc
routing-only run remain FAIL.** No merge or push. K36's reproduced allocator
starvation is fixed and ten focused cases passed on the final tested head. A live K33
LOOKUP EIO is now attributed and its routing cycle fixed; the unique callback
of the earlier **fatal read** remains OPEN pending further evidence.

## Reopened cause investigation — October 29

### Further K33 read failure: a draining leader still suppresses election

The `bddd7376c` nuc campaign is **FAIL**: six admitted cases PASS, one FAIL,
57 queued cases NOTRUN. In case 4, `w-13` (300,000 bytes) had closed
successfully but `Path.read_bytes()` failed **inside `io.open` with EBUSY**.
This is not a passing rerun or merely an EIO-to-EBUSY reclassification.
The unchanged-limit qualification stopped further admissions and captured the
Python traceback and surviving daemon stacks before cleanup.

Raw: `nuc:/home/maik/efs-artifacts/taskH-causes-20261029/k33-routing-fixed-1/`.
Closed 221-file failed manifest SHA-256:
`b587153426ad867dd8a456c3c0ccfcf7b7442f55843c10102a5f9135bd32ff48`.
The first sealer assumed only four cases had started and refused to write a
manifest; the corrected classification uses exact per-case exits and records
that assumption failure. No outcome or original evidence was overwritten.

**Baseline first:** all **64** journaled successful closes verify exactly on
hashed on-host clones with the same `bddd7376c` binaries. This includes 54
known at first capture and ten closes completed afterward. No missing or
unexpected names, clean clone shutdown and unchanged original inventories.
`round-4-baseline-clone/result.json` SHA-256:
`08842679f6c713ce98c30d6b594de4dfc3fc8dd7c75909e65456c740a410d26e`.
The failed application read stays FAIL.

**Captured mechanism:** node 2 was group 0's leader when stopping began at
07:17:34.098 UTC. Its handler-stop gate closed new requests, but its pump
kept leadership/heartbeats while one admitted handler repeatedly waited for
fresh read proof. The leader did not exit until 07:18:01.318 (27.220 s);
node 3 then became leader at 07:18:02.136. Meanwhile node 1 answered
GETCHUNKS for inode 221185 with 16 BUSY replies after approximately 400 ms
read-proof waits each plus the existing backoffs; the last was
07:17:54.444, at the fatal OPEN exception. Increasing retries would not fix
the underlying shutdown leadership problem, and no such increase is made.

**Fix `85962d8cd`:** the pump, under its ordinary Raft-owner lock, disables
campaigning and demotes hosted leaders/candidates as soon as a stop is
requested, before beginning further proof/flush/tick work and again after
inbox apply work. This lets remaining voters elect while handlers drain.
The normal core step-down operation still refuses during receive persistence
and is retried next cycle; it never overrides that fence. Committed entries,
apply cursors, log persistence, durability barriers and all shutdown/read
limits remain unchanged. There is no new setting or wire/store format.

`test_shutdown_bounded.py` now executes that helper with the **actual core**
step-down and campaign functions. It checks normal operation untouched,
stopping demotion, no new campaigns, persistence-fenced demotion retried,
idempotence and unchanged term/commit/apply indexes; ordinary and ASan/UBSan
runs PASS. The existing checkpoint/durable-dispatch/cursor and REPORT receipt
regressions also pass. Local unit evidence:
`/Users/mike/Documents/Codex/artifacts/taskH-causes-20261029/shutdown-leadership-unit/`;
manifest SHA-256:
`9cf9acc9bc723374724679514e67d65baacbc06c45f7ea201bc1e2bbce6110e0`.

The shutdown-repaired nuc campaign (`k33-routing-fixed-2`) completed
**64/64 K33 cases PASS**, 9,937 successful closes and **zero recorded write
errors/fatal reads**. Raw:
`nuc:/home/maik/efs-artifacts/taskH-causes-20261029/k33-routing-fixed-2/`.
Its 1,223-file closed-manifest SHA-256:
`f1af27da119902ea93a224a887485fda3b8677f9d63615775fdfa34bddd6a7f7`.
All private processes/mounts ended and binaries remained unchanged. All stores
stay retained. This is same-build fresh-store testing, not compatibility work.

Independent `scripts/check_k33_cleanup_logs.py CAMPAIGN SOURCE_ID` checks both
completed TCP campaigns: each has 256 clean client logs (revoke success, zero
settled tickets) and 960 daemon starts with matching orderly rc=0 exits and
all three completed shutdown barriers. Maximum daemon stop durations were
10,597 ms on nuc and 1,449 ms on fstor007. The original mount helper waits
for client exit but does not journal/check its return code; positive cleanup
logs and absence checks are the supplementary evidence, not a claim that the
helper checked it. Supplementary `cleanup-log-check.json` hashes are
`4dc9ac67da0e049f5514a94188ea77669f4f1abb01291f8900c1e91b2c510d73`
(nuc) and `b85fb5bdc9faeeddc57d487cffee12f6866987ea1df1bb80e660a372f9396bf0`
(fstor007).

Final-product RDMA attempt-100 remains **FAIL**:
`TOTAL 530 FAIL 1 NOTRUN 0 STATUS FAIL (668s) OVERBUDGET 7 BUILD 0s`.
Its two K33, ten K36 and three K41 focused gates all PASS. The only full-suite
failure was case 442, `test_shutdown_blocked_io.py`: its missing-record
write/fsync cases produced the required deadline marker, but the assertion
searched only the last 3,000 log bytes and Raft diagnostics pushed it outside
that tail. Both complete logs retain the marker. All original duration,
exit classification and missing-record assertions passed before this failure.
The failed fixture (`src/tmp/efs-shutdown-blocked-9gapffm8/`) is kept on
node9901. Closed 898-file manifest SHA-256:
`9f8eb0d653f26855f493e2cf2f06e2f2f9fd67aec183702f44210a98248a2060`.

Test-only `ca6a3edbd` searches the complete fresh daemon log for that marker;
the tail is still used for failure diagnostics. It changes no bound or exit
assertion. Attempt-101 uses the same `85962d8cd` product, same RDMA/fault
profile and eight jobs. Three focused blocked-I/O repetitions PASS at about
11,036–11,038 ms against the unchanged 15,000 ms bound, followed by all two
K33, ten K36 and three K41 focused gates PASS. Full qualification is **PASS**:
`TOTAL 530 FAIL 0 NOTRUN 0 STATUS PASS (691s) OVERBUDGET 7 BUILD 0s`.
All 530 complete command transcripts were copied and hash-checked; binaries
were unchanged and no private processes/mounts remained at closure. The
895-file closed-manifest SHA-256 is
`14a55b0f75a8a274e1fb2f7bcfcc8f3a2ed6490ed29bbe71f73595cdcaf9d4a6`.
Seven unchanged soft budgets were exceeded: K35 91.8/80 s, geometry 92.2/60 s,
B6 176.7/125 s, coordinator-kill 54.0/25 s and the three D39 model shards
33.7/25 s, 18.6/15 s and 6.3/5 s. Hard test limits and assertions all passed;
these overruns remain visible, not waived or relabelled. Raw: `node9901:/data1/efs/evidence/taskH-repair-20261026/attempt-101/`.

The exact-original-binary nuc diagnostic (`k33-exact-error-trace-2`) continues
separately, stopping new admissions on its first fatal read and retaining all
stores. The new `scripts/trace_k33_stop_read.py SOURCE OUTPUT [TRACE_SO]`
provides a narrower fresh-store diagnostic: five members, a common leader,
one fully verified 65,537-byte synthetic payload, four readers, then leader
stop with no further writes. It records the Python exception and optional
error-only native FUSE callback stack, uses original product limits and is
never classified as a qualification PASS. Its original-source `5ecd9cefe`
run on free fstor007 is at
`/data1/efs/evidence/taskH-causes-20261029/stop-read-only-original-1/`.
The controlled stop run completed **NOT REPRODUCED**: 15,325 whole-payload
comparisons, no read errors, leader stop in 1,116 ms. This is diagnostic
evidence only. Exact binary hashes and driver identity are recorded in
`reproduction/identity.json`. Neither diagnostic opens any historical store.
Its 31-file closed-manifest SHA-256 is
`f8dd1075c68ccc22f1f21505c40bbf66cb2833c217b60cf51792a61b7f63c8d9`.
Fstor007 now runs a separate original-source canonical 64-case/four-way
control (`k33-exact-error-trace-7`) from that same unchanged source/binary
tree, with `scripts/trace_recovery_failure.py` and the verified interposer,
fresh stores, original 900-second outer bounds and original internal bounds.
The fstor007 control completed **64/64 read checks PASS**, 6,665 successful
closes, zero recorded write errors/fatal reads. Its 1,223-file manifest SHA-256:
`e8a00548fbe621e06114bca16ab1a163c5ee58d59a81f09c0c813e77a567e8b3`.
Original-source non-reproduction is not repaired-build acceptance or proof of
the historical callback. Nuc's 256-case control uses its exact original
failing ELF and remains RUNNING. It has captured a LOOKUP EIO in case 24 and
FLUSH EIOs in cases 25/27, all correlated to **write attempts**, not fatal
reads. `addr2line -e ORIGINAL_SOURCE/efs-fuse -f -i 0x41e68c 0x418480`
resolves the native callback sites. The FLUSH logs show PUT-ticket attempts
1/3/5/7 ending NET before `flush-meta report_rc=-6`, directly confirming the
already-repaired premature-ticket-exhaustion mechanism. The closed-case
on-host analysis `native-write-errors-24-25-27.json` SHA-256 is
`346bf9fc74618c56263c0737efde5315c71b2b28ad0a7b6ba3146ed8da93d41f`;
it includes log/journal/binary hashes and native-to-operation timing.
The nuc diagnostic stops further admissions on the first fatal read; any such fixture must have every journaled
successful close verified on hashed same-binary on-host clones before changes.
Neither is repaired-build qualification. The historical fatal-read callback
remains OPEN until captured or otherwise
causally established; repeated passing cases alone do not close it.

The shutdown-repaired `85962d8cd` fstor007 campaign (`k33-fixed-6`) is complete:
**64 K33 cases and three K41 cases PASS**, 6,482 successful closes, **zero
recorded write errors/fatal reads**. All private processes/mounts ended and
binaries stayed unchanged. Its 1,271-file closed-manifest SHA-256:
`a4e6491fa79f56234a8a5ab4a4c011667263d67fe08c9508e2b472df09aae675`.
This does not substitute for the remaining same-host and final-head full gates.

The preceding routing-only `bddd7376c` full RDMA gate (attempt-99) completed
**PASS**: `TOTAL 530 FAIL 0 NOTRUN 0 STATUS PASS (681s) OVERBUDGET 7 BUILD 0s`.
Its ten K36, two K33 and three K41 focused cases also passed, and all 530
command transcripts were copied/hash-verified. Seven soft overruns remain
recorded, including B6 (167.2/125 s) and stop progress (71.8/50 s); no limits
were raised. Closed 907-file manifest SHA-256:
`1f4b4207177bc588a902c14a041515124d435174e54211c5d683fb35175d0b10`.
This gate **predates the shutdown-demotion fix** and does not erase the nuc
read failure or qualify the final repaired product.

The preceding `bddd7376c` fstor007 campaign separately passed 64 read checks,
6,477 successful closes and three real K41 gates, with **one write EBUSY**
kept. Its 1,271-file closed-manifest SHA-256:
`2aac8e4755344bbfe77820130760de4144d81dcec2cdbdf737a1098c9c1706f2`.
That host's passing result does not erase the nuc failure.

At the user's request, the original K33 read EIO and attempt-80 K36 mkdir
failure are active investigations. This section supersedes any implication
that repeating a passing qualification causally closes either failure.
No merge or push. Original K33/K36 failed stores remain on their hosts.
The separately encountered K41 failure has a retention gap detailed below;
its original test deleted its fixture, and only the complete transcript remains.

### K36 reproduced: an elected leader cannot clear a stranded allocator intent

Attempt-95 on `f11d00963` failed the second focused K36 case at the original
phase-8 `churn.mkdir()`. Two K33 cases and the first K36 case passed; the full
suite was **NOTRUN** after the focused failure. The client used its unchanged
30-second operation budget and existing eight-second identified-result
resolution, then returned EBUSY; unmount revocation also failed and stays a
failure. Raw: `node9901:/data1/efs/evidence/taskH-repair-20261026/attempt-95/`.

The post-failure capture shows node 2 leading both groups, node 3 following,
and equal committed/applied indexes (592 and 366), with no persistence,
storage or pressure fence. The pump, KV worker and checkpoint worker were
idle. Thus the earlier suggestion that this was simply a missing election
or a checkpoint stall does not explain this reproduced failure. Repeated
PREPARE refusals identify allocator key `0eec01`, shard **3820**, at MKDIR
stage 6. A KV clone inspected with the same build contains its exclusive
intent, owned by `fbb239bc70e8e218f335c4d427887896`, participants 1/3820,
coordinator 1, with no decision. Its birth was 05:19:32.539 UTC. Node 2 had
restarted at 05:19:23 and acquired the leader role after node 1's compaction
crash. Originals were never opened by the inspector.

The scheduler starts cold discovery at shard zero and scans at most 64 due
shards per GC pass. Startup marks every shard equally, and observing PREPARE
on an already-marked shard did not promote it. Consequently a known stranded
high-shard intent could wait behind dozens of cold passes. This is a
scheduling explanation, not a measured 60-second successful recovery.

**Data baseline:** all **16 known payloads are exact**, with no missing or
unexpected entries, on hashed on-host clones using the exact `f11d00963`
binaries; original inventories unchanged and clean clone shutdown. The failed
`churn` directory was absent in that observed clone state. Result
`baseline-k36-clone2/result.json` SHA-256:
`28377980cc9e8052b3be624417f7481503b1f52d472c2b8929d34e9c5164042a`.
The verifier's first setup used a wrong manifest member-count field and was
NOTRUN before starting any store; that failure is retained separately.
Main-cluster processes were exempted from the private isolation check only by
exact PID, executable, command-line hash and process-start fingerprints, and
were never stopped. Closed 643-file failed-run manifest SHA-256:
`8062ac603452d28cc497b4e40954584332dbaec6a682336654f34fd685982ac1`.

**Fix (`20c0a2ebe`):** successful PREPARE and BUSY-conflict observations mark
known work separately from cold discovery. Fair cursors reserve work for both
classes, lending unused capacity, while keeping the **five-second age and
64-scan cap unchanged**. A generation check prevents an empty scan clearing
a concurrent observation. Cold discovery promotes records it finds for timely
retry. No format change, configuration switch or longer deadline is involved.
The regression executes the production scheduling/mark functions: old code
recovers zero records at shard 3820 in its 64 scans; repaired code recovers it
within the same cap. It also checks concurrent marks, cold progress, borrowed
capacity, age and leadership. Ordinary and Clang ASan/UBSan runs pass.
Stable local evidence: `/Users/mike/Documents/Codex/artifacts/taskH-causes-20261029/recovery-priority-unit/`,
manifest SHA-256 `4a41958a202ec876e3c8cc576af21b507b9286fa9bfd5bfc8315bfb30d9275d7`.

Attempt-96 stopped in strict-GCC unit preflight on a misleading one-line loop,
with no server/test fixture started. `8db51c2c1` adds explicit braces, preserving
all assertions and warnings. Attempt-97 (`8db51c2c1`) passed **ten** unchanged
focused K36 cases and two K33 cases on private node9901/RDMA `mlx5_0`, eight
jobs, faults enabled and original limits. Its full result remains **FAIL**:

`TOTAL 530 FAIL 3 NOTRUN 0 STATUS FAIL (651s) OVERBUDGET 7 BUILD 0s`.

All 530 command transcripts are hash-verified. Failures: the retained-pin
recovery unit's mock lacked the new scheduler fields; the recipe guard rejected
a long-suite budget row for the new **fast** scheduler unit; K41 packed its
batch without recording the required node-6 PUT refusal. The latter's assertion
remains required: a run that does not exercise refusal is not a passing gate.
Its random initial destinations did not guarantee node 6 was selected, and
phase 2 began without waiting for the resumed node's health to recover. Because
its fixture was deleted by the original test's finally block, distinguishing
those two setup possibilities from this transcript alone is **not proven**.

Correction `95b837784` supplies the scheduler mock fields, removes only the
invalid new long-suite budget row (the fast unit keeps its existing runner
classification), and makes K41 fault selection deterministic **in fault builds
only** before its real probes. Previously refused destinations stay skipped;
real PUT refusal, cancellation, live-spare selection, all payload/fragment
checks, and every assertion/deadline remain. Phase 2 requires all six members
up before writing its same 60-file batch. K41 now retains any failed fixture
even without the KEEP variable. No production placement behavior changes.
The unchanged-pin, priority and recipe units pass locally; strict-GCC/Linux
and live K41/full results are checked in the new run.

Attempt-97 raw: `node9901:/data1/efs/evidence/taskH-repair-20261026/attempt-97/`.
The 840-file closed manifest explicitly records the missing K41 fixture and
kept K33/K36 stores; SHA-256:
`9e00bed5d68bb120a4a8b9883afe0b422382bddd90e807311432410e7488b12c`.
Seven soft overruns remain findings; no budget was raised to remove them.
Attempt-98 (`95b837784`) repeats the same ten-K36/two-K33 cases, three K41
cases, and full 530-command suite with EFS_K41_KEEP enabled as well. It is
complete **FAIL**, not a passing gate:

`TOTAL 530 FAIL 1 NOTRUN 0 STATUS FAIL (689s) OVERBUDGET 7 BUILD 0s`.

All ten K36, two K33 and three K41 focused RDMA cases passed; all three
previous failures passed in the full suite too. The sole new failure was
A6 inventory coverage: the deterministic fault-only placement hook needed an
inventory row. `bddd7376c` supplies it without changing the gate. All 530
transcripts are retained; the 906-file closed-manifest SHA-256 is
`a3867237378ddb755653413fc920e42a42a22d4e537544c56a9e71b70d2041aa`.
Seven soft overruns remain visible. Attempt-99 on `bddd7376c` repeats the
same focused and full RDMA gates, with ordinary/ASan metadata-routing and
inventory preflights; its completed PASS is recorded above.

The repaired product (`20c0a2ebe`) also completed a separate 64-case/four-way
TCP campaign on otherwise quiet fstor007: **64/64 read checks PASS**, 6,447
successful closes, zero recorded write errors/fatal reads. Every payload in
each case's acknowledged set is checked by the unchanged original test;
there are no application read retries. All stores remain on-host.
Raw: `/data1/efs/evidence/taskH-causes-20261029/k33-fixed-3/`;
1,223-file closed-manifest SHA-256:
`13eb55132ac88ad3b55192f397ee6007626234559fac25a6bda7db094c97e51c`.

### K33: reproduced live EIO, verified retained data, premature lease exhaustion fixed

**New live attribution and fix (`bddd7376c`).** Exact-original-ELF tracing
completed 64 passing read-check cases, 9,349 successful closes and one
**write-attempt EIO**, retained as a finding. Case 50's `w-27` attempt
(65,537 bytes, node 3 stopped) failed at 07:01:38.028824 UTC. The native trace
106 microseconds earlier resolves `0x41e68c` to **`ll_lookup`**, old
`efs_fuse.c:6116`. Its preceding LOOKUP RPCs alternate stale leader redirects
and failed sends/receive to node 3, with failures at attempts 1/3/5/7/9/11/13
and the last cap failure unlogged. The original reader has no caller deadline;
its 16-attempt loop can exhaust while another voter could serve it. The
existing higher-level deadline changes therefore did not close this route.
This captured error happened during name resolution for a **write attempt**,
which the unchanged test retries; it is not a newly captured fatal read.

The fix makes LOOKUP/GETATTR explore another voter after a responsive peer
re-advertises a hint that already failed transport in that read. It bypasses
the stale read cache for that exploration, then allows the peer to be retried
if it recovers. No permanent blacklist, new timeout, larger retry cap, user
switch, mutation route or receipt/replay change. The actual production RPC
regression reproduces the old cycle: **16 sends, NET**, after 4.5 simulated
seconds (clock 1.0 to 5.5 s);
fixed code reaches the available voter in **four sends**, 0.1 s. Both
LOOKUP and GETATTR test a cached failed endpoint and eventual recovery, and
still stop at the shorter inherited deadline. Before-code runtime failure,
repaired ordinary and ASan/UBSan PASS are retained locally at
`/Users/mike/Documents/Codex/artifacts/taskH-causes-20261029/read-stale-hint-unit-final/`;
manifest SHA-256:
`3d5a704529a56c365cbb2701cc9a11cb3fa34befd002e86f7e9393114794f83a`.
REPORT receipt and identified-operation regressions also pass unchanged.

Trace campaign: `nuc:/home/maik/efs-artifacts/taskH-causes-20261029/k33-exact-error-trace-1/`;
1,222-file closed-manifest SHA-256:
`3805e5581952e3570a9a3226b7a901fa96c301659f1b283611bc71976014cdf4`.
The executable remains the exact failed `5ecd9cefe` ELF, SHA-256
`8e2d3b1fb51daf95ccfa29c7bf809709dd12588facf6bee70f66e75808d7e464`.
All original stores/fixtures remain on-host. New repaired `bddd7376c`
64-case/four-way TCP campaigns use fresh stores on nuc
(`k33-routing-fixed-1`) and fstor007 (`k33-fixed-5`), plus attempt-99's RDMA/full
gates. Their completed outcomes are recorded above and do not turn
non-reproduction into historical causal proof.

Read-only CRC-checked inspection of original fatal case 25 identifies `w-27`
as inode 319489 and records OPEN sequence 743 at group-0 index 1535/term 2,
followed by CLOSE sequence 744 (ACK 743) at index 1537/term 3. Node 2's log
records that CLOSE's apply at 03:38:22.622, the fatal Python exception time.
This correlation is **not** the missing callback trace or a stored-verdict
proof. Reproduce with `scripts/inspect_k33_lease_history.py --fixture FIXTURE
--name w-27 --output NEW_JSON`; original-node files are hashed and unchanged.
The initial inspection's wrong node labels/time-unit label are retained in
`lease-history-inspection-v1.json`; corrected v2 plus the script are next to
the original `round-25` evidence. No KV open, replay or original-store mutation.

The preceding `95b837784` TCP campaign also finished: 64 read checks PASS,
6,621 successful closes, **one write EBUSY**, zero recorded write EIO/fatal
reads; three K41 gates PASS with real refusal/cancellation and exact payloads
after a holder loss. Raw fstor007 `k33-fixed-4`; closed 1,271-file manifest:
`89724f0dabfe40bd8580b71804e7abf35c031a5712b54dfb45b661c812301b54`.
The EBUSY remains a finding, not an error-free campaign.

**Return-site tracing is still OPEN.** The original-source error-only control
on nuc completed 64 passing read cases, 9,970 successful closes and **four
recorded write EIOs**; that is not error-free qualification. Raw:
`/home/maik/efs-artifacts/taskH-causes-20261029/k33-boundary-3/`;
1,225-file closed-manifest SHA-256:
`8f84055e0bad7e852dde1258694429331a95c5c2c721bf52062eea217c5ddae2`.

The next diagnostic uses the **unchanged exact failed original ELF** from
`/home/maik/efs-taskH-causes-20261029/original-full-1/src/`, not another
instrumented EFS build. Tooling at `826a33150` preloads an error-only FUSE
reply interposer into the private test's efs-fuse processes only. Daemons,
request bodies, result codes, errno and deadlines are unchanged. The manual
`check_fuse_error_trace.py` preflight verifies one forwarding of each call,
positive/negative results and errno preservation, silent success/ENOENT, and
no payload bytes in logs. Stack addresses and timing are logged only after an
EIO decision or failed FUSE reply. As with any instrumentation, timing
perturbation remains a limitation; absence of reproduction cannot prove cause.

Reproduce on an isolated Linux/FUSE host with matching original binaries in
`SRC`, fresh evidence `OUT`, and private tracer `TRACE` built from the tool:

```sh
python3 scripts/check_fuse_error_trace.py PREFLIGHT_NEW_DIRECTORY
cc -shared -fPIC -O2 -g -fno-omit-frame-pointer -std=gnu11 -Wall -Wextra -Werror -pthread \
  $(pkg-config --cflags fuse3) tools/fuse_error_trace.c -ldl -o TRACE
EFS_GIT_ID=ORIGINAL_ID EFS_TRANSPORT=tcp timeout --signal=TERM --kill-after=15 900 \
  python3 scripts/trace_recovery_failure.py SRC/tests/test_k33_payload.py OUT \
  --fuse-error-trace TRACE
```

Use fresh stores for each case, up to four concurrent cases/64 total; stop new
admissions after the first fatal failure. Before changing that failing fixture,
verify all acknowledged payloads on hashed on-host clones with its exact binary.
The completed diagnostic is at
`nuc:/home/maik/efs-artifacts/taskH-causes-20261029/k33-exact-error-trace-1/`.
Private tracer SHA-256:
`1353dbc47b6fcd2749922a964f8b66132792bebc45d0a29a7ce3527031f74953`.
It is not a repaired-product gate, and its passing cases do not close K33.

A four-way concurrent control using fresh stores and matching original
`5ecd9cefe` binaries reproduced a fatal read EIO in case 25: `w-27`,
65,537 bytes, immediately after its successful close, with logical member 3
stopped and members 1, 2, 4 and 5 live. This file is above the packing
threshold. The first-failure state/journal was captured before test cleanup;
debuggers attached only after the application operation had failed.
Twenty-seven other admitted cases passed, one failed, and 36 queued cases
were NOTRUN after that failure. All of those outcomes remain evidence.

Raw: `nuc:/home/maik/efs-artifacts/taskH-causes-20261029/k33-original-parallel-2/`.
Closed text-manifest SHA-256:
`b69fe35eb61c99cbebeee8ed2bf3b31a695c6d4b093c9da31a5583fe98463098`.
Its raw outcome dictionary also retains a separately named clone-controller
exit; `case-outcome-summary.json` explicitly excludes that entry when counting
test cases. Classification SHA-256:
`fa80bc0f91f5db70c0c7db9628cf122d4cb86f75cce879d010115ae0b0c94f74`.

Before any new-build test on nuc, hashed on-host clones of case 25 were
started with those same binaries. **All 88 journaled successful-close
payloads were exact**, with no missing/unexpected files, original inventories
unchanged and clean client/daemon stops. This is full-byte verification, not
sampling, and does not erase the live availability failure. Baseline result
`round-25-baseline-clone/result.json` SHA-256:
`2156c6a9ab752c32835a16e63cfe9fd03cd76da8c91745c59391b9b829e5aea8`.
Original and newly failed stores remain on-host; no payload was exported.

The read failure logged no fragment-decode failure despite fragment tracing.
A fresh diagnostic-only build of the original source logged native return
sites in 64 completed four-way cases, all passing the original read checks.
It retained 9,480 successful-close events and four write-side exceptions.
Closed diagnostic manifest SHA-256:
`014f93ff0d114306487e0a54e4644809ad93f5b977c335c9f0521526488df3fe`.
Its traces show NET propagating through the retained
lease owner and FUSE OPEN/adoption path to EIO during a transient exchange;
those write-side exceptions remain findings, even where the original test
subsequently retries the write successfully. The exact fatal `w-27` return
site and historical `w-19` site were not captured, so their unique attribution
must not be inferred solely from this related error chain.

The concrete bug is that `session_command_reply` stopped after eight mixed
redirects, lost replies and temporary refusals even when OPEN still had time
in its existing 30-second caller deadline. Commit `b50f33258` keeps that same
immutable command, UUID/epoch/sequence, receipt proof and inherited deadline
until it completes or the bound expires. Callers without an inherited deadline
still have eight attempts; stored verdicts, malformed replies and fencing
rules remain final. No new timeout or runtime configuration switch was added.

The causal regression (`0b66e1889`, `tests/test_lease_edge_rpc.py`) executes
the production transport, retained lease owner, FUSE OPEN wait and errno
mapping. With pre-fix transport source it returns **NET, errno 5/EIO, unknown
ownership after eight exchanges while time remains**. The repaired transport
reaches a proven success on exchange ten with the identical command. The
simulated clock is explicitly a unit model, not a measured live latency.
The same test enforces shorter inherited deadlines, retains unknown ownership
at expiry, and refuses to replace a stored negative verdict. Ordinary and
ASan/UBSan units pass. Stable local unit evidence:
`/Users/mike/Documents/Codex/artifacts/taskH-causes-20261029/lease-budget-unit/`;
initial transport-only manifest SHA-256
`2151c162c86a53ca47205af595b089149ff8199c32daf5c0cfcd227a9c23ebf8`.
The five pipeline captures are retained in the `pipeline/` subdirectory;
manifest SHA-256:
`1f6c86cda3528b89927b4bf1a6c13859ea636f06ceb0b168806b5af70837d1c9`.
Complete matching-source live gates remain required.

### Follow-up K33 write finding: the ticket loop also stopped prematurely

The pre-ticket nuc stress on `b50f33258` passed all 64 original read checks,
with 10,349 successful-close events, but retained **three write EIO and two
write EBUSY** events. It is not an error-free campaign. Closed 1,225-file
manifest SHA-256: `ecde5ca512263d48d1ee6435f3565580ce77fd58c5eebb6c2fb522b15e915f11`.
Case 44's `w-50` (1,048,579 bytes) logged ticket admission losses at attempts
1/3/5/7 followed by NET and flush EIO. Case 53 separately retained an expired
PUT-window BUSY, including its EIO mapping; the eight-second window was not
extended or ignored.

Commit `f11d00963` makes ticket admission/status/settlement/batches and the
idempotent degraded mark obey an existing inherited deadline, retaining the
finite eight attempts without one. The immutable ticket body and all proof,
refusal and state checks remain. The extracted production regression recreates
the alternating stale-hint/lost-exchange sequence: old code returns NET with
state zero at exchange eight, repaired code obtains proof at exchange eleven.
Short deadlines and final negative verdicts remain errors. Ordinary and
ASan/UBSan units pass. Initial fixture compilation failures remain distinct
from the subsequent runtime-negative control. Local unit manifest SHA-256:
`909fc9baad340c76871a844048f76378e75ab6ef1af2be0445c43ebc5cd86815`.

The ticket-repaired TCP campaign on otherwise quiet fstor007 passed all 64
cases, 6,513 successful-close events, zero reported write errors and no fatal
read. Raw: `/data1/efs/evidence/taskH-causes-20261029/k33-fixed-2/`;
closed 1,223-file manifest SHA-256:
`8ef53840186931699bc732d087f7a7b339f2e95e324fe427eb6e9f2560fe1bbe`.
This predates the server recovery change and does not replace its qualification.

### Earlier K36 client-routing correction (insufficient by itself)

Commits `ce5196205` and `254725fd9` change only failed-endpoint routing in the client's
production dual-group retry loop. After a NOT_PRIMARY redirect, send loss,
receive loss or an unsuccessful connection checkout previously retained the
hint for namespace operations; only REPORT discarded it. MKDIR could spend
its entire existing deadline trying the failed hinted endpoint instead of
another available dual voter. The fix discards that failed hint for every
command, preserving the request/opid, receipt rules, retry backoff, existing
finite cap for callers without a deadline, and inherited caller deadline.
A live REPORT coordinator's BUSY still retains that coordinator.

The extracted-production-function regression in
`tests/test_metadata_retry_budget.py` now redirects an identified MKDIR
named `churn`, loses the hinted peer at each of those three points, and
requires another voter to complete the identical request before the same
30-second deadline. It fails on pre-fix source `776eeceb9` and passes on
corrected source `254725fd9`, including Clang ASan/UBSan. Existing short-deadline, attempt-cap,
REPORT, namespace identity and ambiguous-result checks remain. The local
CREATE retry integration unit could not link without Linux `libefs.a` and
ibverbs; it is not claimed passed on the Mac and remains part of the Linux
full gate.

Retained local unit evidence, containing code/logs only:
`/Users/mike/Documents/Codex/artifacts/taskH-causes-20261029/dual-hint-unit/`.
`sanitized-manifest.json` SHA-256:
`c25d23d79a1841301a8888ff028e858f4ca4b3971c27a10dc753e2b67c8d67ce`.
Reproduce the negative control with the current regression and
`EFS_INODE_RPC_SOURCE` pointing to `git show 776eeceb9:src/client/inode_rpc.c`;
run the same test normally for the repaired implementation. An assertion in
the negative control is expected, not a passing release gate.

The old text-log inference that group 2 never elected again is withdrawn.
Read-only framing/CRC inspection of the original attempt-80 Raft logs shows
both surviving nodes persisted group-2 term 5, with matching final entries
through index 320, after the killed leader's term-4 entry 316. The original
`churn` transaction's birth time is 00:07:58.416 UTC; the finisher logged its
committed abort at 00:07:59.222, while the client exhausted its unchanged
30-second deadline at 00:08:27.940 with last status NOT_PRIMARY. The original
server returned a node-1 hint while that node was crashing. This supports
the failed-hint mechanism; the original per-attempt client target sequence
was not captured, so the exact historical execution is not independently
replayable. The 30-second failure and unclean unmount remain failures.
Raw read-only report:
`node9901:/data1/efs/evidence/taskH-repair-20261026/attempt-80/k36-preserved-raft-history-20261029.json`,
SHA-256 `15b52e3c15ab2aa1c10905763cfcbb7f2b34360f20e1f18d07c87771c14257d0`.

### Unchanged-case diagnostics and current gates

Commits `619b898ea` and `776eeceb9` add a test-only acknowledgement journal
and first-failure capture. The driver traces only target-test function
returns/exceptions, never retries a failed application read, and saves the
successful-close oracle before cleanup. Debugger attachment is attempted
only after the original operation has failed, restricted to that fresh
fixture's exact daemon executable/store paths. An unavailable debugger is
recorded as a capture limitation. The full runner's optional forensic output
root preserves original command identity, budgets, scheduling slots and
outer deadlines. A synthetic unit exercises the actual driver and runner;
an injected read EIO must remain FAIL and preserve its successful-write set.

On otherwise quiet nuc, 15 fresh K33 cases using the original `5ecd9cefe`
binaries passed: 1,620 successful-close records, zero reported write errors.
This is a same-source fresh-store regression control, not a mixed-build or
retained-format compatibility test. Raw:
`/home/maik/efs-artifacts/taskH-causes-20261029/k33-old-1/`;
closed text-manifest SHA-256
`d5b7f40d63cf90066a4b3445b637b18c33784bd641caab2f2e11bfa597216bac`.
The original 70-file failed fixture's exact cold readback remains separate;
these passes do not close its EIO cause.

On private node9901, 15 unchanged K36 cases passed over real `mlx5_0` RDMA
on source `619b898ea` (product code still pre-fix): 403 successful-close
records and two reported write exceptions during the existing deliberate
crash contract. Those exceptions remain records, not additional successful
operations. Raw:
`/data1/efs/evidence/taskH-causes-20261029/k36-1/`;
closed text-manifest SHA-256
`d6031dd0eee8d5902a23747a0253d665a6cb410681b8c8af654e36ea7c693695`.

The concurrent pre-fix diagnostic control on `776eeceb9` also passed:
`TOTAL 529 FAIL 0 NOTRUN 0 STATUS PASS (672s) OVERBUDGET 6 BUILD 0s`.
All 529 transcripts and first-failure journals are retained. Raw:
`node9901:/data1/efs/evidence/taskH-repair-20261026/attempt-87/`;
closed 800-file text-manifest SHA-256
`3f4183461ea005145a045579717edf14b968627cadf136d5c99c0cd29d00f1c3`.
This reinforces why another passing run alone cannot establish the cause.

Attempt-88 on `ce5196205` is retained **FAIL**:
`TOTAL 529 FAIL 1 NOTRUN 0 STATUS FAIL (746s) OVERBUDGET 9 BUILD 0s`.
Its K33 and K36 cases passed; the W155 REPORT unit aborted. The first fix
incorrectly also skipped an unhinted peer after a brief reply loss, bypassing
that host's local receipt replay. The existing receipt-replay assertion was
not removed. `254725fd9` narrows rerouting to failed nonzero redirect targets
and retains the original unhinted REPORT route. Both the new MKDIR cases and
all existing REPORT/identified-operation assertions pass locally, including
sanitizers. W155 now prints and preserves its failed native program and full
stdout/stderr instead of losing the assertion inside `CalledProcessError`.
The matching pre-correction native assertion and generated source/binary are
retained in the local unit directory; `scope-correction-manifest.json`
SHA-256 `e910c0c6b86ccc458c8c39900e8ba298eded9da962072d49e8f4d95ca194a5ea`.
The Linux run originally retained only the Python failure traceback; this
stronger matching assertion is a separate Mac reproduction, not recovered
Linux stdout. Attempt-88's 826-file closed text-manifest SHA-256 is
`2dd9fec8e668e36e87b17f4b357339e9fd2ac7d4548619be8485126c57ae6840`.
No failed fixture or result was erased or promoted to PASS.

Attempt-89 (`254725fd9`) is retained FAIL: 529 commands, one failure,
zero NOTRUN, 757 seconds. Its only failure was K43 waiting for a one-shot
`formed: 6 members` line although the cluster had correctly formed three,
then committed additions through six within four seconds. `b21a32614` replaces
that log assumption with matching six-member tables inside the original
120-second setup bound, and selects the actual assigned member 2 as the
coordinator rather than assuming process start order assigned its identity.
No publication/fault, payload, recovery or operation deadline was changed.
The actual setup function is exercised by the existing setup unit, including
reordered/duplicate identities. Failed K43 stores are retained at attempt-89's
`src/tmp/case-472/efs-k43-nz9zivfl/c/` on node9901. Closed 819-file manifest
SHA-256: `7b81dccf8a8500bf6d74472d022ca09b39b68cd6aee0a412f7d52dd73b92748a`.

Attempt-90 (`b21a32614`) passed three focused K43 runs and the full gate:
`TOTAL 529 FAIL 0 NOTRUN 0 STATUS PASS (856s) OVERBUDGET 6 BUILD 0s`.
All transcripts and finding-bearing fixtures remain. Closed 2,576-file
manifest SHA-256:
`bdfbb670a2fbb2de71dd70b9e1816f5fc1f789cd01223d1a0460c80a81d3cb1e`.
This predates the lease-budget fix and is not its qualification.

Attempt-91 (`b50f33258`) passed two focused K33 and two focused K36 RDMA
cases, plus both targets in its full run, but the full gate remains FAIL:
`TOTAL 529 FAIL 1 NOTRUN 0 STATUS FAIL (708s) OVERBUDGET 7 BUILD 0s`.
The only failure was compilation of the extracted session RPC unit: its
fixture lacked the deadline-query mock newly used by the transport. Commit
`f25d98abd` supplies the explicitly deadline-less mock; all existing assertions
remain, and bounded foreground behavior is checked by the lease unit.
The session unit and full-runner unit pass locally. The same commit preserves
the first fatal operation's Python traceback, distinguishing a failing OPEN
from a failing read instead of inferring that from absent fragment logs.
Attempt-91's 908-file closed manifest SHA-256:
`91caff984bf247da423d200678dfac663de6792de81a035e432cdd47cd19200a`.
Attempt-92 was NOTRUN because its preceding full gate failed.

Attempt-93 (`f25d98abd`) is retained FAIL:
`TOTAL 529 FAIL 1 NOTRUN 0 STATUS FAIL (755s) OVERBUDGET 8 BUILD 0s`.
Its only failure was the strict-C11 Linux declaration of `usleep` in the
new extracted production-to-FUSE unit. Commit `f702c1e0c` enables the
fixture's normal libc declaration; no production code or assertion changes.
Both session/lease units and the lease pipeline under Linux ASan/UBSan pass.
Attempt-93's 872-file closed manifest SHA-256:
`1e9a4e359c63fd9520fc20be35b441f85cb37b22eab1daa045e3e84593c44b0e`.

Attempt-94 on `f702c1e0c` **PASS**:
`TOTAL 529 FAIL 0 NOTRUN 0 STATUS PASS (787s) OVERBUDGET 8 BUILD 0s`.
All 529 full command transcripts were copied and hash-checked. Its two focused
K33 cases passed in 29.2/24.3 seconds; two focused K36 cases in 56.2/58.5
seconds. Logs confirm `RDMA transport up segmented-v1 on mlx5_0`.
The eight soft scheduling-budget overruns remain visible in `summary.txt`,
including K38 (36.0 versus 35 seconds), B6 (167.2 versus 125 seconds) and
K43 (53.2 versus 25 seconds); no hard timeout, internal deadline, payload
check or acceptance assertion was changed. The Linux session/lease unit
preflights and actual pipeline ASan/UBSan run also passed.
Raw: `node9901:/data1/efs/evidence/taskH-repair-20261026/attempt-94/`.
The closed 915-file manifest verifies unchanged binaries and no remaining
private test processes/mounts; SHA-256:
`0402e2a9103b2f684833280ed72b5cdcb3eb35e84d7eb795af1647385e22f1e6`.
This is the lease-repaired qualification, not another pre-fix control. It
predates the later ticket-budget and recovery-scheduler product fixes.

The pre-ticket repaired product (`b50f33258`) completed
64 four-way TCP cases on nuc at
`/home/maik/efs-artifacts/taskH-causes-20261029/k33-fixed-1/`.
It was released only after the original-source diagnostic stopped and the
retained failure's 88/88 baseline verification was checked. These use fresh
private stores and original deadlines; no main-cluster changes or
application-read retries. Its passing read checks and retained write errors
are classified in the follow-up finding above.

A further original-source control completed on otherwise quiet fstor007 in
`/data1/efs/evidence/taskH-causes-20261029/k33-boundary-3/`, source
`/tmp/efs-taskH-causes-20261029/k33-boundary-3/src/`. This is `5ecd9cefe`
with the committed diagnostic-only `--boundary-only` overlay (`16e4845b5`):
final FUSE errno conversions and failed read-reply returns only. It uses the
same four-way/64-case maximum, original test bounds and fresh matching-build
stores, stopping new admissions after its first failure. The acknowledgement
and exception driver is `scripts/trace_recovery_failure.py` at `f25d98abd`.
All first-failure data is collected after the application has already failed.
All 64 cases passed their original read checks, with 6,542 successful-close
events and one recorded write EBUSY (case 55, `w-7`, 300,000 bytes). No fatal
read was reproduced. Closed 1,226-file manifest SHA-256:
`1784ec64390089a64287cb017c5b96df20c815f5984740e83a1b47ee806f81da`.

The alternate host's libfuse 3.10.2 is too old for this source. Attempts 1
and 2 both stopped at build prerequisites, with **zero test cases run**;
`boundary-build-prerequisite-summary.json` retains both, SHA-256
`4810b9e4a71289ccd1ac46f636dedeebeb93045f07ba9cfd4b8482f03ae477a9`.
Attempt 3 uses the existing x86_64 EL9 libfuse 3.16.2 dependency in a private
prefix, with explicit FUSE compiler/linker flags and pkg-config path, rather
than changing EFS source for the older system API. Dependency archive SHA-256:
`bb812f59502f5e4582a8e6577662e3d3426dd058af0fbdb385d2f5e78ab92ac7`.
The negative-control build ID is `5ecd9cefe-boundary-diag`; it is never run
against a store belonging to a repaired build. No main deployment is touched.

The separately built original `5ecd9cefe` concurrent full-suite control
completed FAIL: 513 commands, two older harness failures (blocked-shutdown
startup marker and W166 setup), 13 NOTRUN, 1,455 seconds. Its original runner
drops passing stdout, so it is not a new passing release gate. Failed stores,
logs, target journals and binary identities remain; closed 65-file text
manifest SHA-256:
`f44e495446b8e9de679abe265599c088ad22095ddece0c07ff40db944c987bad`.
Two private shutdown-test daemons leaked by that older constructor were
identified by exact executable/store paths, recorded, then stopped with TERM
only; their stores were preserved. The first queued stress attempt's
isolation guard refused before starting any case and remains NOTRUN. The
separate subsequent stress attempt reproduced the EIO as recorded above.
The four-way/900-second stress profile is additional diagnostic evidence,
not a change to the original acceptance profile.

Reproduce an individual unchanged case, from its matching Linux source tree
with all three binaries built, `/dev/fuse` and `fusermount3` available:

```sh
EFS_GIT_ID=SOURCE EFS_TRANSPORT=tcp EFS_RAFT_OBS=1 EFS_READ_FRAG_PROF=1 \
  timeout --signal=TERM --kill-after=15 900 python3 \
  scripts/trace_recovery_failure.py tests/test_k33_payload.py NEW_OUTPUT
# K36 uses tests/test_k36_recovery.py; node9901 additionally selects
# EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 and its private libfuse runtime.
```

For the complete current gate use the standalone prerequisites/commands
below, additionally setting `EFS_RECOVERY_CAPTURE_ROOT` to a fresh absolute
output directory outside Git. Raw `run.sh` and source/binary manifests record
each executed command. Keep failed/finding-bearing fixtures until these
investigations close; never change builds inside an existing fixture.

## Final retained-output and fresh-reader result

Node9901 attempt-86, source `fdfda8cfa`:

```text
TOTAL 529 FAIL 0 NOTRUN 0 STATUS PASS (693s) OVERBUDGET 8 BUILD 0s
```

This is the complete `make test-all` enumeration through the committed runner,
with eight jobs, `EFS_FAULTS=1`, global forced RDMA `mlx5_0` (fixtures keep
explicit TCP where designed), O_DIRECT, private dependencies and actual
TSan/ASan/UBSan/LSan preflight. All operation, workload and suite bounds remain
unchanged. Eight observational recipe wall-time budgets were exceeded and
remain in the summary; no bound or status rule was changed to suppress them.

All **529 complete command transcripts** were copied and independently
SHA-256 verified. The snapshot oracle checked **677 known successful original
payloads through the distinct fresh reader mount seeded at recovered node 3**,
plus **30 clean post-recovery payloads**. Its saved digest manifest has SHA-256
`cd7b71e415bf2df7d1669572373973e25b9e3084ad6656e628e92370cf3ecf31`.
These are full-byte checks, not samples. Normal metadata/fragment routing is
allowed; this is not a physical-node-3-only read claim or a power-loss gate.

The deliberate stalled-follower workload also retained **six payload
operation/close findings and 40 unclean-revocation rounds** across 173 metadata
rounds (zero nonzero producer exits). Those explicit failed operations are not
successful payloads. The existing snapshot contract permits refusals during
that artificial stall and separately requires exact successful payloads and
clean recovered service. Startup errors and missing producer progress remain
fatal. Raw findings, successful-write manifest and result are kept on-host.

Current evidence is
`node9901:/data1/efs/evidence/taskH-repair-20261026/attempt-86/`, including
`command-output/manifest.json`, `final-verification-summary.json` and the
closed 773-artifact manifest. Closed-manifest SHA-256:
`ef25b117f403aa7db9da7d9813a30052ffad0baf443609cf091afa576ebf901f`.
Before/after hashes of all five qualification binaries matched; no owned
processes or mounts remained. Private node9901 and intel2 resources are
released; node9901's main cluster was untouched.

Source-equivalent product targets at `4d80689f7` also passed: three original
concurrent four-way TCP rounds, three RDMA K38 rounds, B6 clean/dirty-fsync,
D4 progress/plateau, K45/Raft regressions, and K22 memory/log gates. Their
12 cold fixtures checked **47,377 exact payloads, zero errors**. Product code
and those target bodies did not change afterward; later changes concern test
oracles/capture/cleanup and are covered by the final full run. Their exact
source-specific manifests and the independent native suite's INCOMPLETE
hardware limits remain below, rather than being relabelled as final binaries.
Historical failures, missing-capture/store limits and ambiguous outcomes stay
recorded; no failed operation was promoted to a successful write.

## Current qualification and fixture corrections

Product sources (`src/`, `include/`, Makefile) are identical from `06978bb31`
through `fdfda8cfa`. Changes after `06978bb31` affect test fixtures only;
none changes a workload, operation deadline, admission limit or transport
implementation. Existing source-equivalent product gates are identified
explicitly below rather than presented as a full-suite PASS on the latest head.

- `06978bb31`, intel2 attempt-19: three original concurrent four-way TCP
  rounds PASS, nine cold fixtures, **13,521 exact payload checks, zero errors**;
  250 isolation scans, zero findings. Native full-suite result:
  `TOTAL 529 FAIL 0 NOTRUN 10 STATUS INCOMPLETE (807s) OVERBUDGET 6 BUILD 0s`.
  Closed 216-artifact manifest SHA-256:
  `2b669948ecd204db2784cdd0887ebc4a075131a0469f3f3783507d6a4aa3232c`.
- `06978bb31`, node9901 attempt-77: B6 dirty/clean, three RDMA K38 rounds,
  D4 progress/plateau and K22 memory/log PASS. Three cold fixtures checked
  **30,987 exact payloads, zero errors**. All 14 prerequisite supplement
  commands/builds PASS; this does not relabel the Intel full suite complete.
  Closed 190-artifact manifest SHA-256:
  `8285493277ea4144a517340619aa03dec6de1d8646fbf5b0107fbfd75dfca889`.
- Same source, node9901 attempt-78, full suite with `EFS_FAULTS=1`, forced
  RDMA `mlx5_0`, eight jobs and actual sanitizer preflight:
  `TOTAL 529 FAIL 3 NOTRUN 0 STATUS FAIL (1128s) OVERBUDGET 10 BUILD 0s`.
  Snapshot-import hit its outer 900 s bound; leader-admission failed its
  unchanged 60 s producer join; W166 prefill hit its unchanged 30 s bound.
  Every fixture explicitly selected TCP for its daemons, but its benchmark
  child lacked `env=env`. Admission's four retained producer logs repeatedly
  report segmented-RDMA setup failure against the TCP fixture. The snapshot
  fixture had discarded producer output; its exact stuck phase is not proved.
  These failures remain FAIL, not payload corruption or full payload PASS.
  Admission's retained failure checkpoint reports 11,646 successful FUSE
  writes and no FUSE errors, but its failed fixture has not been cold-verified.
  Four leaked benchmark children were identified by exact binary and seed,
  captured, then terminated with SIGTERM; no KILL was needed. Original
  checkpoint hashes were unchanged and stores remain on-host. A disconnected
  snapshot mount was normally unmounted after recording it; stores were kept.
  Closed 66-artifact failure manifest SHA-256:
  `00337a971609ab3b236240cb8c283cb667dfd995569fc5aa9c5ad2453fe94b5b`.
- `4d80689f7`, node9901 attempt-79: targeted B6/K38/D4/K22 PASS again,
  three cold fixtures, **32,376 exact payloads, zero errors**, supplement PASS.
  Closed 190-artifact manifest SHA-256:
  `7fcc3ff91edb679e60b1d4408b9c3a37255df973ba634f540db2f3f44e760387`.
  Targets-only `FULL_NOTRUN` is not a full-suite PASS. Intel2 attempt-20 independently completed the original concurrent TCP profile:
  three rounds PASS, nine cold fixtures, **15,001 exact payloads, zero errors**,
  261 isolation scans, zero findings. Native full-suite result:
  `TOTAL 529 FAIL 0 NOTRUN 10 STATUS INCOMPLETE (915s) OVERBUDGET 8 BUILD 0s`.
  Closed 216-artifact manifest SHA-256:
  `d1699212775dc17975ffcd760d5b5e5bd74480c5ac11b559bf9a68093e5f1609`.

`4d80689f7` fixes admission's producer environment and tracks/retains child
outcomes. `5328bfbd0` applies explicit fixture environments to snapshot-import
and W166 prefill, retains snapshot producer logs and W166 timeout output, and
checks producers actually completed. On a failed fixture only, registered
children receive bounded cleanup with actions recorded; interruption never
qualifies a successful load. The actual worker regression exercises the
functions with synthetic subprocesses, including startup/refusal/unclean-exit
and cleanup cases. It passes, as do syntax compilation and diff checks.

`761714602` corrects an overbroad assertion introduced in `5328bfbd0`:
snapshot-import's original contract explicitly permits reported operation/close
failures during the intentional follower stall, while requiring exact successful
payloads and clean post-recovery service. The added `bench_errors == 0` did not
match that contract. Attempt-81 retains its FAIL at that assertion: one producer
reported eight deadline failures and 37 rounds reported unclean revocation while
the follower was paused. These are retained findings, never successful operations.
The restored oracle still requires each worker to complete successful metadata
work, rejects startup failures and retains the 60 s join assertion. Producer
outcomes/logs are saved explicitly; existing payload, snapshot, memory, catch-up,
drain and post-recovery assertions are unchanged. No original assertion or limit
is removed. This correction is test scope, not a product availability fix.

`484d7cad6` closes two retention gaps without changing test contracts or limits:

- The full runner now saves every command's complete output to
  `tmp/command-N.log`; failed commands still retain their existing `fail-N.log`.
  The qualification collector copies transcripts write-once under the evidence
  directory. A command whose worker failed to record a verdict/capture cannot
  disappear from the tally as PASS. Actual runner tests inject a blocked output
  path and verify FAIL, alongside existing timeout, scheduling and NOTRUN tests.
- Snapshot-import keeps its work directory when it reports any metadata or
  payload operation/close finding, even if the recovery oracle passes. Actual
  cleanup-branch tests distinguish clean success, retained findings and gate
  failure. Every reported failed operation stays a finding, never a success.

Full node9901 attempt-84 (`a8ecc8e9b`) closed:
`TOTAL 529 FAIL 0 NOTRUN 0 STATUS PASS (675s) OVERBUDGET 8 BUILD 0s`.
Closed 54-artifact manifest SHA-256:
`d36369d1195c2b51cee9436808808753b8ae878d4e014fe0cc7a87e861e7d33c`.
Binary hashes were unchanged and no owned processes/mounts remained.
**Retention limit:** that runner discarded passing stdout and snapshot-import
removed its successful fixture. The count/content of any contract-allowed
snapshot findings in that run cannot be independently recovered; no claim
that the count was zero. The PASS describes executed assertions, not retained
per-command revalidation. Attempt-85 repeats the complete gate with the fixes.

`fdfda8cfa` corrects a separate, demonstrated snapshot payload-oracle gap.
The old test created `mnt-verify` seeded at recovered node 3, but used
`Path(original_writer_path).read_bytes()` for the retained payloads. Its
"through node 3" output was therefore too strong. Earlier PASS labels stand
for their executed assertions, not fresh-reader verification of those original
payloads. The independent target-profile cold verifiers already use their
fresh-reader paths and are unaffected.

The corrected verifier rebases each known successful writer path onto the
fresh reader root and checks its original digest there. A regression executes
the actual function against different writer/reader contents, removes the
writer file, then injects a reader mismatch and missing file. A stale writer
view cannot satisfy it. The gate saves `successful-payloads.json`, including
reported writer errors, before reading, and `verification-result.json` only
after all original payloads and 30 clean post-recovery writes verify. The
fresh mount is seeded at node 3; normal routing may still use other eligible
metadata leaders or fragment holders. This is not a claim that every data
fragment was physically read from node 3. All counts, loads and bounds stay.

Attempt-85 (`484d7cad6`) closed:
`TOTAL 529 FAIL 0 NOTRUN 0 STATUS PASS (779s) OVERBUDGET 8 BUILD 0s`.
All **529 complete transcripts** were copied and independently hash-verified.
Its snapshot workload retained 181 metadata rounds with no nonzero producer
exit, **40 unclean-revocation rounds and six payload operation/close findings**.
Those failed operations are findings, never successful payloads. Its old
writer-path oracle is explicitly limited in `snapshot-evidence-scope.json`.
Closed 778-artifact manifest SHA-256:
`1e78bf91ba6651fd672dbb5b2af78a9701f9c1620878551746b6121d0a74a3e5`.
Unchanged binaries and zero owned processes/mounts were verified at closure.

Latest archive (`fdfda8cfa`) SHA-256:
`74cd37a17698972230e1c5afa4f99ad611f818e08c246eb37f24700c681dd2bf`.
Node9901 attempt-86 is the completed retained-output/fresh-reader gate above.
One staging check refused a partially transferred archive before any fixture
or build was created; after that same transfer finished its SHA was verified,
and the source was launched. It is not a retried test case or a product failure.

Previous archive (`484d7cad6`) SHA-256:
`6dc23689be72c063c75b16097635483f7ad53476bbeed2b515f76c296ee56aa2`.
Node9901 attempt-85 was the retained-output gate; its launcher requires and
hash-verifies one copied transcript per enumerated command before closing.
Its source/build/configuration and all operation/suite limits match attempt-84
except for the test/evidence changes above. Product code remains identical.

Previous archive (`a8ecc8e9b`) SHA-256:

`fe04401ab16c9bd2c870021e48a2ed9d4e3f7e20b17d5b7f64ec40c3637b6bf8`.
Node9901 attempt-84 was the complete full-suite gate on that source.
Its unchanged workload uses `EFS_FAULTS=1`, forced RDMA `mlx5_0`, eight jobs,
actual TSan/ASan/UBSan/LSan preflight, and the existing Raft diagnostic switch.
It began only after all three unchanged K36 RDMA repetitions passed.

`a8ecc8e9b` fixes a shutdown-test readiness defect exposed by those diagnostics.
The test searched only its last 3,000 log bytes for the one listening marker;
tracing could push that marker out before the next poll. The preserved logs
prove the marker exists in the full startup history but not the tail. The
fixture now checks its complete fresh startup log, then the same socket and
owner-record checks, under the unchanged 60 s bound. A failed constructor
also releases its owned child/log, preserving the failed store. The 15 s
shutdown bound and all outcome/durability assertions are unchanged. Intel2
attempt-22, source `a8ecc8e9b`, passed all three full shutdown-test repetitions
with tracing enabled. Closed 13-artifact manifest SHA-256:
`3670e096c67dbce429c9848f8e535602490dfddc99d37a41de0087abd39460f5`.

Node9901 attempt-81 (`5328bfbd0`) closed:
`TOTAL 529 FAIL 2 NOTRUN 0 STATUS FAIL (716s) OVERBUDGET 9 BUILD 1s`.
Its two failures are the overbroad snapshot assertion and the shutdown startup
marker, not erased by later corrections. Its K36 case passed unchanged; the
observer sampled only `/proc` and did not trigger any debugger attachment.
Two leaked startup-failure daemons were identified by exact executable/store,
recorded, stopped with SIGTERM, and their stores preserved. Closed 258-artifact
manifest SHA-256:
`02a9249e5439439ebd710118e0263648ce0250cedb1f50433c33396241ddd3f8`.

Three unchanged K36 RDMA repetitions on `761714602` (node9901 attempt-82)
also passed. Closed 66-artifact manifest SHA-256:
`2786fa4fd048d8f3edcb879f62fe62cc4418836d8a6d344dffda5cc36ac448a3`.
Three unchanged TCP repetitions on intel2 attempt-21 passed, closed
67-artifact manifest:
`13a6f2cf6904d3dc157b9cee4855401bcddafe382196733411a355d0e4d644fa`.
These repeats do not establish the cause of attempt-80's namespace timeout.
Attempt-83 (`761714602`) was superseded in WAIT_PRIOR, before any build/test,
by attempt-84; its controller/source/journal remain and it is NOTRUN, not PASS.
The intermediate `761714602` archive SHA-256 is
`3e5cce89438d4bf28c0b5932fd1f323443d517170462a698cecf94a368e56ee4`.

Previous archive (`5328bfbd0`) SHA-256:
`11f9e19133bc02faf2606a43b60351de34c37acca0381cc6b6f8fa8c737ea7e6`.
Node9901 attempt-80 (`4d80689f7`) closed:
`TOTAL 529 FAIL 3 NOTRUN 0 STATUS FAIL (1108s) OVERBUDGET 9 BUILD 0s`.
Its K36, snapshot timeout and W166 failures remain FAIL. Closed 61-artifact
manifest SHA-256:
`9bebc910219b833799b29158e44de078d9d2bcf89386f8b7bd6f96a9940d19e4`.
No owned processes/mounts remained; binary hashes were unchanged. Attempt-81
then ran `5328bfbd0`, retaining all predecessor failures. Both use actual
TSan/ASan/UBSan/LSan preflight, private dependencies, `EFS_FAULTS=1`, forced
RDMA `mlx5_0`, eight jobs and unchanged limits; their exact `run.sh` and
source manifests live in their evidence directories. No policy was changed.

Attempt-80 additionally failed K36 step 8: `mkdir(churn)` exhausted its
existing 30 s client deadline after the intentional compaction crash. Earlier
steps 1–7 reported successful recovery and exact payload checks; the whole
case remains FAIL. Its store/logs are retained under
`attempt-80/src/tmp/case-480/efs-k36-x8h2t663`. Election/forwarding causality
is not causally established despite the subsequent unchanged TCP/RDMA
repetitions. This failure is not classified as one
of the producer-environment defects.

Nuc, AMD and intel2 are released by Task H; their private processes/mounts
were checked absent at the cited closures. All Task H private node9901
qualification fixtures are also stopped; their
failed/finding-bearing stores remain kept for the open investigation.
Node9901's main cluster is untouched.
Raw artifacts use the host roots stated below; failed stores are retained for
the open investigation, without bulk export or pruning.

## Earlier failures and repair

Nuc attempt-10, source `32696e72e8dce37b6cacad3d4ac4ac58a46c9563`:
`TOTAL 526 FAIL 2 NOTRUN 10 STATUS FAIL (1506s) OVERBUDGET 40 BUILD 0s`.
Raw evidence: `/home/maik/efs-artifacts/taskH-repair-20261028/attempt-10`;
closed manifest SHA-256
`5a3a62fe8f25cb07c915619ff13a844b95399d5803b101d61bdab034947ac0fe`.
Binary hashes unchanged; zero owned processes/mounts at closure. Both failed
fixtures remain on nuc, without export or pruning:

- `.../attempt-10/src/tmp/case-487/efs-w130-qoybj3p2`: after successful
  snapshot resync, node-1-mounted listing returned 95/96 names, missing f0057.
  Offline inspection opened on-host COPIES only: all three original KVs
  contain all 96 dentries, including f0057 and its child inode. Original KV
  file hashes were checked unchanged after inspection. This is metadata
  presence evidence, not a full payload check or proof of the exact race.
- `.../attempt-10/src/tmp/case-469/efs-k43-3mpbuw3l`: both rewrites returned
  success, but a 64-part transaction reached age-based recovery at 14.7 s.
  The mechanism assertion failed before fresh-reader verification; no final
  payload PASS is claimed for that failed run. Its exact historical record was
not typed, so later pin-ownership fixes/passes do not prove that attribution.

`236a25cb5` fixes a demonstrated READDIR race: a cached used-lane bitmap can
skip a migration's first destination after the local pass has omitted that
name. SPLITTING scans all lanes; source-zero refresh detects LOCAL-to-spread
transitions before EOF. The host covers both Raft groups for migration and
restarts the current RPC's cookie after fencing a newly observed transition.
No wire/store format, workload threshold or deadline changes. The production
scan regression fails against the prior source and passes native and
ASan/UBSan; server-callback tests cover transition restart and forwarding.
This does not establish that the live failure used that exact interleaving.

Fresh nuc attempt-11 on 236a25cb5 completed all three unchanged W130
and all three K43 cases PASS; each K43 fresh reader checked 16 MiB exactly.
These focused passes do not erase attempt-10 or qualify the newer head.
Archive SHA-256 `7bd8a3100201d43745623db940581c5d88fbd81f7aa80e934ac71efc7947b52b`;
source `/home/maik/efs-taskH-repair-20261028/attempt-11/src`, evidence
`/home/maik/efs-artifacts/taskH-repair-20261028/attempt-11`.

Further review found that pending-transaction scans include retained inode
pins, which intentionally outlive successful RESOLVE until RETIRE. The K43
fault parks the finisher and can delay that RETIRE. `dde6564ed` classifies
pin-only records in memory: a known decision keeps its pin under the existing
decision sweeper, rather than re-deciding ABORT and redundantly resolving all
participants. Undecided pins, mixed/payload records and reclaimed history
retain ordinary recovery. I/O errors never authorize ABORT. No pin is dropped
and RETIRE still requires all participant verdicts. The exact attempt-10
record was not typed in its log, so this is a demonstrated source defect and
a plausible explanation, not a retroactive forensic attribution or PASS.
`52d6f5890` makes a partial scan conservative: it never certifies pin-only
ownership when its page overflowed. Actual publication unit tests cover the
classification; the production recovery scheduler regression covers ownership,
unknown history and I/O, passes native/ASan/UBSan, and fails on prior source.
The K43 assertions and recovery age limits are unchanged.

Further unchanged-limit results on 52d6f5890:

- nuc attempt-15 full run has a D4 plateau FAIL: foreground write+fsync
  10.31 s against 10 s (fsync 9.92 s); no application errors or bad payloads
  in that case's verification. Its retained logs show truncate maintenance
  spending up to 23.8 s in one GC pass. Full run continues to retain all
  outcomes. Fixture `.../attempt-15/src/tmp/case-468/efs-d4p-pcl993l8`.
- amd-attempt-01: `TOTAL 528 FAIL 2 NOTRUN 11 STATUS FAIL (831s) OVERBUDGET 6 BUILD 0s`.
  Stop-progress correctly refused a mount opened by external GVFS; its
  failed store remains kept and the foreign process was not stopped.
  The synthetic CREATE fault oracle timed out after 30 s while intentional
  SIGABRT children went through AMD's piped Apport core collector. No
  workload correctness PASS is inferred from either failure.

`9ceececc9` ports devel's truncate-maintenance implementation and regressions
from `6550b678d` and `773284b5d` without merging branches: bounded proposals,
resumable candidate cursor, durable partial proofs, and short retry only
when capped work made actual progress. Their existing tests pass unchanged.
The branch retains its extra SPLITTING lane scan: the new cross-group
regression holds a destination PUT before the parent bitmap/DEL, with no
layout/bitmap change throughout the page. Devel `3426111b4` fails that runtime
case; this branch passes native and ASan/UBSan. This is the specific additional
case covered by the branch's READDIR implementation.

`caf92a618` additionally disables dumpability in the intentional-abort test
process, without changing signal assertions, host policy or the 30 s bound.
A diagnostic on AMD runs that exact fixture against the unchanged CREATE
hook/library dependencies of 52d6f5890; native and ASan/UBSan with LSan enabled
both PASS. Raw diagnostic is `amd-attempt-01/create-fixture-diagnostic2`;
only the fixture root path was substituted, and it is not full candidate
qualification. The prior failed attempt remains FAIL.

Historical `caf92a618` candidate archive SHA-256:
`cac279a1d11f24e7390700ec06a059253c887e0d865b852feb180600dfa52514`.
Superseded qualification schedule (fresh stores, exact run.sh):

- intel2 attempt-17: three unchanged concurrent four-way TCP target rounds
  and exact cold verification, after intermediate attempt-14 closes.
- nuc attempt-18: native full test-all after failed attempt-15 closes and a
  60 s quiet check. Private matching Valgrind headers and mocked cluster
  scripts supplied. All operation and suite limits unchanged.
- node9901 attempt-75: RDMA targets/cold verification and hardware supplement
  after intermediate attempt-72's supplement closes; attempt-76: separate
  complete full test-all, `EFS_FAULTS=1`, RDMA mlx5_0, supplied prerequisites,
  and actual sanitizer preflight. Current ptrace policy was observed as 0;
  the launcher changes no policy. It fails preflight on any NOTRUN marker.

Superseded 52d6f5890 controllers (intel2 attempt-16, node attempts-73/74) were
cancelled in WAIT_PRIOR before tests started; their sources/logs and
superseded.json journals remain. Active intermediate tests were not stopped.
Nuc attempt-13 completed all six focused cases PASS; closed manifest SHA-256
`d44e784f819e4d24d4604d55917f81df861bf7b1cbd5a83bcef5a0a57cf204e9`.
Attempt-11 closed manifest:
`26ab168a25de7e602426bbc959615322d3ab07a669bbd82369c78a223f722a9c`.
Node intermediate attempt-71 (236a25cb5) targets PASS, three cold fixtures,
35,982 exact payloads, zero errors; 149-artifact manifest SHA-256
`7f55d237016a9b6e81d4eea9ff59c1fcb8cbf54e099bb4acdada2952754067d0`.
These intermediate results never substitute for final-source qualification.

Nuc/intel2/AMD source roots are `/home/maik/efs-taskH-repair-20261028/NAME/src`
and evidence roots `/home/maik/efs-artifacts/taskH-repair-20261028/NAME`.
Node source roots are `/tmp/efs-taskH-repair-20261026/attempt-N/src`; evidence
`/data1/efs/evidence/taskH-repair-20261026/attempt-N`.
Original K33 outage read-EIO cause remains OPEN. No merge or push.

## Historical closed results and limitations

- Product d1030297c, intel2 attempt-09: all three concurrent four-way rounds
  and independent K22 log gate PASS; 16,392 exact cold payloads in nine
  fixtures, zero errors. Controlled live isolation: 300 scans, zero findings.
  Full gate: `TOTAL 526 FAIL 0 NOTRUN 11 STATUS INCOMPLETE (821s) OVERBUDGET 6 BUILD 0s`.
  The INCOMPLETE label is retained. Closed 203-artifact manifest SHA-256:
  `84b237ef929af446d7e993c461ce13c973ec34a7a721bf0afe7d83577ec1f6a0`.
- Product d1030297c, node9901 attempt-70: B6 dirty/clean, all three RDMA K38
  rounds, D4 progress/plateau, K22 memory/log and three cold fixtures PASS
  (18,945 exact payloads, zero errors). Separate same-head prerequisite
  coverage: all 14 commands/builds PASS with no NOTRUN markers, covering all
  11 Intel hardware/header/fault-build omissions. Closed 177-artifact manifest:
  `9502cd702d11dcd97ca466519498546fc4631604cca0abeb64f226913e047e8a`.
  The targets-only qualifier's FULL_NOTRUN is not rewritten as a full PASS.
- Both closures verified unchanged tested binary hashes and zero owned
  processes/mounts. **At that historical closure**, Task H released intel2 and private node9901.
  Both were subsequently reacquired for the current qualification above. Node9901's existing main cluster
  remains untouched; this release covers Task H resources only.
- Intermediate 662789008 on nuc attempt-07: all three loaded rounds and 7,488
  exact cold reads PASS, but `TOTAL 525 FAIL 1 NOTRUN 12 STATUS FAIL (1337s) OVERBUDGET 42 BUILD 0s`.
  REPORT step-down saw write max 3.85 s versus its unchanged 3 s bound
  (fsync max 2.39 s). The full failure remains FAIL. Closed manifest, 198
  artifacts and zero owned resources:
  `311fe88a40c2d507928454847679ebb5736ff18cc3390db2f436c214cc4ba9d3`.
  **Second retention limit:** the old REPORT test also auto-pruned its failed
  store. Its detailed timing dump remains in fail-470.log; store replay and
  that case's byte-verification PASS are not claimed.
- `f1ed2ccd8` moves the strict REPORT fault-latency case into the existing
  serial section. Six slots had allowed a two-slot snapshot importer onto
  the same disk, contrary to the latency test's documented quiet-I/O setup.
  Four writers, 1,600 appends, 200 fsyncs, fault/step-down sequence and the
  3 s per-operation bound are unchanged. The actual scheduler regression
  passes; unchanged d103 runner fails its REPORT ordering assertion.
  Declared gate budget is 1514 s under the unchanged 1650 s limit.
- `32696e72e` additionally keeps failed REPORT stores automatically and
  exports its existing retention flag in qualification. Actual cleanup-branch
  checks confirm failure retention and normal successful cleanup.

The original-host 32696e72e full check is closed FAIL above. Its archive
SHA-256 is `d19853ca82c8370890e31c9b98774b455ce0e5cf5ab3a0deea2b41b9816f0893`.
It used eight TCP jobs, unchanged test limits, private matching Valgrind 3.26
headers, and pinned external cluster scripts with mocked transports only.

This last change affects only runner ordering and fixture retention.
Production `src/`, `include/` and Makefile are byte-identical to tested
product d1030297c; the source manifest records their Git tree/blob identities.
Recheck: `git diff --exit-code d1030297c 32696e72e -- src include Makefile`.
The already-passing unchanged product targets are retained; the modified
full runner failed on the original nuc as recorded above.

## Fixes and proof

- `4c7eaaa67`: B6 now counts one segment-publication event containing the
  actual live ledger member count, upload start on CLOCK_MONOTONIC,
  acknowledged-node mask, and the missing node/fragment whose degraded mark
  committed. It no longer subtracts independently logged upload and pass
  counters. Both facts needed by the oracle are in one completion event.
  A three-ACK upload begun before the fault may finish switching afterward;
  a new upload begun after node 3 is down/refused must have exactly the other
  two ACKs and node 3's committed mark. Missing ACKs without a mark, an
  inconsistent mark, conflicting events and disappearing history fail.
- `121e5fb24`: GETCHUNKS reused output records across collect retries. The
  collector wrote delta counts only when the new record had deltas and ORed
  packed flags without clearing the old slot. A fold or a differently shaped
  record in the next attempt could retain a previous delta count or packed
  flag, producing a reply the strict client rejects with EFS_ERR_PROTO.
  The collector now zeros each output slot before constructing its current
  record. No client validation, retry budget or ownership rule was weakened.
- `c92271a5f`: the new regression's stub array parameter now exactly matches
  the production declaration, retaining GCC's -Werror checks. The earlier
  compiler-only failures on intel2/node9901 remain retained, not counted as
  completed qualifications.

`tests/test_b6_log_accounting.py` reproduces the 39 ms event/counter cut,
checks in-flight healthy uploads, and rejects genuinely unprotected
post-fault publication. It fails against the unchanged previous oracle.
`tests/test_getchunks_collect_retry.py` executes the production collector
with a reused reply, first with spans then without, and first packed then
ordinary. The unchanged collector fails the strict client validator at
runtime; the fixed collector passes native and ASan/UBSan checks on Linux.
The test is part of `make test` and therefore `make test-all`.

The original live K38 capture identifies PROTO during background pressure
but lacks its exact rejection site. The collector defect above is directly
reproduced and fixed; attribution of that particular historical EIO remains
bounded by that missing trace. Passing repeats alone are not a causal proof.
The separate original K33 `w-19` read EIO remains OPEN.

## Historical full-suite failures and candidate

The original-host c92271a5f run completed with:
`TOTAL 524 FAIL 4 NOTRUN 12 STATUS FAIL (1771s) OVERBUDGET 49 BUILD 0s`.
Its three four-way target rounds and 7,935 cold reads passed, with 423
isolation scans and zero findings; that does not override the full FAIL.
Closed evidence: nuc attempt-05/final-closed-manifest.json, SHA-256
`11756361cde37d89ccf7ddc56de6c823e8575a87d88a020fe038d0b4863be4fb`,
195 artifacts, unchanged target binary hashes, zero owned processes/mounts.
A second failure-details-manifest.json closes 24 detailed daemon/client/status
logs, SHA-256
`9ac5cb1fc56f21a21250dde84a8ccb7610a69205fa0ba5c2b49bfe3912fda8fe`;
it records the empty duplicate-packing case directory explicitly.

- D4 plateau: one close/flush EIO reported BUSY, and one foreground operation
  took 11.71 s against the unchanged 10 s bound. This is distinct from the
  earlier PROTO signature. The failing last write remains an error; the
  test's one skipped failed-write verification is not a payload PASS.
- Packing duplicate candidate: its round took 9.3 s against the existing 8 s
  setup/age-window bound. **Evidence limit:** the test unconditionally removed
  this failed synthetic store in finally. The full failure log remains, but
  store replay is unavailable. This loss is not described as preserved data.
- W130: raft-resync returned BUSY during a receive-persistence cohort. The
  CLI issued one request; the Raft guard correctly refused it before effects.
- W166: the parked-compactor setup did not fill compressed L0 within 90 s.
  The compactor did park; retained status samples reached 673,452 B of its
  1 MiB cap. The close/fault oracle did not run, so this is a setup FAIL.

Repairs retained in the current product, with no workload or deadline changes:

- `662789008`: data-cache bucket selection masked a multiplied inode. Inodes
  allocated 4096 apart consequently used only 16 of 65,536 buckets and one
  I/O mutex per chunk index. The mutex covers network I/O, serializing
  unrelated files sharing a metadata shard. Mixing allocation bits before
  bucket/lock selection removes that structural choke point. This is
  process-local cache state, not a store/wire change. The actual production
  selector regression fails at runtime on unchanged c922 code and passes
  native plus ASan/UBSan; it checks independent-file progress while the
  first I/O mutex is held, and unchanged same-key exclusion. Ownership,
  pressure-index and fold-observation regressions also pass. Failure-stage
  logs distinguish writeback, append-lock, PUT and PUT-window failures.
  **Causality limit:** the old D4 log lacks the exact wait stage, so the hash
  defect is proven but attribution of that EIO awaits new live evidence.
- `16c0e10c0`: raft-resync retries only a definite pre-acceptance BUSY under
  one existing 30 s absolute network deadline, honoring a shorter outer one.
  Unknown transport outcomes, malformed replies and leader refusals are not
  retried. The actual CLI regression checks these cases with a fake clock;
  unchanged source fails at runtime, fixed native/ASan/UBSan passes.
- `ea2b9a342`: packing's strict age-window setup and W166's bounded L0 prefill
  now run in the runner's existing serial section, after pooled I/O. Their
  internal concurrency, records, bounds, faults and assertions are unchanged;
  the three four-way concurrent qualification rounds are unchanged too.
  The actual scheduler self-test proves both wait for the pool; unchanged
  runner fails that test. Recipe budgets remain 1404 s of the unchanged
  1650 s bound; no duration or budget was raised.
- `d1030297c`: duplicate-packing keeps failed fixtures automatically, and
  qualification explicitly retains its stores. A direct execution of the
  actual cleanup branch checks failure retention, successful cleanup and
  explicit retention. No product behavior or oracle changes.

Historical d1030297c frozen source/archive SHA-256:
`d1030297c9e41852cf2ee8ce7652d0226b8da7e5` /
`ed67455106dbb94472d29162fe0a23773615113b6d80b74b942352b640959aa4`.
Those historical controllers used that identity, native/ASan/UBSan positive checks and
unchanged-source negative controls, fresh stores and the existing profiles:

- intel2 attempt-09: final TCP profile plus full test-all, after a 60-second
  quiet check. Source `/home/maik/efs-taskH-repair-20261028/attempt-09/src`;
  evidence `/home/maik/efs-artifacts/taskH-repair-20261028/attempt-09`.
- node9901 attempt-70: final RDMA targets, queued strictly after intermediate
  662789008 attempt-69 succeeds. Source
  `/tmp/efs-taskH-repair-20261026/attempt-70/src`; evidence
  `/data1/efs/evidence/taskH-repair-20261026/attempt-70`.
- Intermediate hash-only 662789008 node9901 attempt-69 and nuc attempt-07
  were already started. They remain separately labelled and never substitute
  for final-head gates. Nuc attempt-07 has a recorded retention-only export
  in its full runner so its older packing test cannot delete failed stores;
  no product/workload/deadline change is counted as final qualification.
  The exact before/after script hashes are in retention-only-overlay.json.
  Nuc attempt-06 was cancelled before qualification;
  its preflight/controller files remain. No foreign process was stopped.

Task T independently reported that its intel2 daemon PID 1472455 remained
from 19:44:52.370 to 20:30:47.554 UTC on October 28, then was stopped by its
owner. This matches the PID this session observed. Intel2 attempt-03 was
already an isolation FAIL and attempt-04 waited/cancelled; neither was
counted as quiet timing evidence. The report does not establish causality
for any failure. Task T's reported records are under
`/home/maik/efs-artifacts/taskT-20261028/intel2-9638c34e0/`.

## Workload and source provenance

The target profiles use three logical voters on one physical host, private
paths/free ports, and two mounts for K38. RDMA uses real mlx5_0 verbs on
node9901; these are not cross-host fabric, physical-host-loss or power-loss
gates. All tests use freshly formatted private stores and the existing committed
`scripts/qualify_publication_repair.py` profiles, quotas, payload oracles,
concurrency and deadlines. O_DIRECT remains mandatory; packing, query
indexing and KV maintenance stay enabled. K38 has two writing mounts and
24 writers under its original body limits. D4 has its original 45/75-second
loads, 10-second foreground and 150-second drain bounds. The short fixed-set
run is not the separate long F1 RSS acceptance. No limits or product flags
were relaxed. Original failed stores are not opened by the new binaries.

Earlier c92271a5f combined archive SHA-256:
`95bf348e5c246dcf71f144d42229c0a7a5ab50ae1602ec3bab53d7655736942a`.
The unchanged pre-fix host source is from `c5acf58dd`, SHA-256
`6ce8e95ec7a0f6997ae66f4cc0b002b6f2bb37b24f38b1d104f603a6030e65aa`.
Linux controllers run the new regression against both, with native and
ASan/UBSan positive checks and an expected failing negative control.
Early node9901 target controllers excluded the then-unavailable LSan runtime;
those checks are not leak-check PASS. The current complete full-suite profile
unsets that exclusion and requires the actual sanitizer preflight to pass. Controller manifests, run.sh, exit codes and logs stay on
host, outside Git. All historical setup failures and diagnostic samples stay
separate from qualification results.

## Runs

- Focused B6 `4c7eaaa67`, node9901: **PASS** for all four modes, exact payloads
  after whole-cluster restart, and dirty-fsync. The closed evidence manifest has SHA-256
  `06aaf715e590db6803bca154fa17e24f511a08bd64b9427dc4bf69a3d920ce74`,
  with 13 retained text artifacts and zero owned processes/mounts. Source and fixtures:
  `/tmp/efs-taskH-repair-20261026/b6-atomic-20261028`; raw evidence:
  `/data1/efs/evidence/taskH-repair-20261026/b6-atomic-20261028`.
- Combined native/ASan/UBSan collector checks and unchanged-source negative
  control `c92271a5f`, node9901 attempt-68: **PASS**. All original RDMA
  targets also **PASS**: three K38 rounds, D4 progress and plateau, K22
  memory and log-watermark gates, B6 clean/dirty-fsync and setup checks.
  The three cold fixtures verified 34,923 payloads with zero errors.
  Source/fixture root `/tmp/efs-taskH-repair-20261026/attempt-68`; evidence
  `/data1/efs/evidence/taskH-repair-20261026/attempt-68`. RDMA uses mlx5_0,
  the existing private libfuse, and TCP for K22/setup. The main cluster's
  `/data1/efs/n1`–`n3` and ports 17432–17434 remain untouched.
- Same-head full test-all completed sequentially after that target controller,
  with results under attempt-68/full-test-all. Exact line:
  `TOTAL 524 FAIL 0 NOTRUN 13 STATUS INCOMPLETE (683s) OVERBUDGET 7 BUILD 0s`.
  The existing build was reused, and its hashes still match the target build.
  This is INCOMPLETE, not a blanket full-suite PASS. Seven budget overruns
  remain reported; no budget or test deadline changed.
- Same-head supplemental prerequisites under attempt-68/supplemental: **PASS**,
  all 13 command/build exits 0 and no NOTRUN markers. These cover mocked
  external cluster scripts, Memcheck DMA annotation controls, RDMA declaration,
  transport and all seven segmented modes on mlx5_0, plus the stalled-compactor
  case from a separate EFS_FAULTS=1 build. These are separate scoped results;
  the original native full-suite line remains unchanged. Node9901's target,
  full and supplemental controllers are closed, with zero owned processes or
  mounts and unchanged target binary hashes. The 175-artifact closed manifest
  is attempt-68/final-closed-manifest.json, SHA-256
  `6cdff9477c546e03e20de5be587bf426e91d0705a5f5a7dda508a7ad72cc484b`.
- The remaining full-suite NOTRUN was the sanitizer self-test's LSan fixture
  under node9901's ptrace policy. The unchanged complete self-test **PASS** on
  intel2, including TSan race, ASan overflow, UBSan and LSan leak controls.
  This creates no EFS process or mount and does not require the live TCP
  profile's whole-host isolation. Its log/exit stay under intel2
  attempt-04/sanitizer-supplemental; log SHA-256
  `269ab72ebf37fe59f531662fbc967e45211b8786c04dfdce53b72d25875b63a4`. This tests the sanitizer harness; it is
  not a leak qualification of every EFS executable.
- TCP four-way profile and full test-all, intel2 attempt-03: **FAIL, isolation**.
  Collector native/ASan/UBSan positive and unchanged-source negative controls,
  plus native Raft early-read off/on, passed. The controller's 60-second quiet
  precheck passed, but Task W resumed before live qualification; the monitor
  recorded foreign workloads and refused the automatic-ID stage despite its
  process exit 0. Remaining stages and full suite are NOTRUN for this attempt.
  This is a preserved isolation failure, not a product failure or a PASS.
  Its 90-artifact closed manifest SHA-256 is
  `222c1fad71c2914e031621294360a291a900a1d9bbf570b6e7200ca071e15c31`;
  owned process/mount counts are zero.
  Source root `/home/maik/efs-taskH-repair-20261028/attempt-03`; evidence
  `/home/maik/efs-artifacts/taskH-repair-20261028/attempt-03`. The `nuc`
  profile name denotes the unchanged three four-way TCP rounds; actual host
  is intel2 (8 logical CPUs). No foreign process was stopped.
- Intel2 attempt-04: **CANCELLED before live qualification** while waiting
  for foreign workloads to finish. Its standalone sanitizer self-test above
  ran separately and passed. The cancelled waiter's closed-manifest SHA-256
  is `258ff071dd15aa3079db8325be09a61c3038132ac8a693b327274c4751991fcf`,
  with zero owned processes/mounts. No foreign process was stopped.
- Original-host TCP run, nuc attempt-05: **targets PASS; full suite FAIL (4)**. Nuc
  became free, with no foreign EFS processes or task-directory controllers;
  the same 60-second quiet precheck then passed. Source/fixtures are under
  `/home/maik/efs-taskH-repair-20261028/attempt-05`, evidence under
  `/home/maik/efs-artifacts/taskH-repair-20261028/attempt-05`. Exact source
  remains c92271a5f and all three loaded rounds, cold checks, independent
  log-watermark gate and full suite remain in the unmodified profile.
  All three concurrent four-way rounds and the independent log-watermark
  check passed. Nine exact cold fixtures checked 7,935 payloads with zero
  errors. Full test-all failed as detailed above. Isolation monitoring covered
  the controlled live stages: 423 scans, zero findings.
  Read-only checks found other workloads
  on xorinox, intel1, intel2, amd, node9900 and fstor007; those were untouched.

## Reproduce the retained-output complete gate

Use a fresh Linux source tree at `fdfda8cfa`, usable FUSE, RDMA `mlx5_0`,
compiler/sanitizer runtimes, and the pinned private headers/cluster-script
prerequisites below. `HEADERS` and `CLUSTER` are their absolute export paths;
`TASK_H_FULL_OUT` is a fresh evidence directory outside the source tree.
The cluster scripts run mocked transports, never production infrastructure.
No old failed store is opened by this reproduction.

```sh
(
: "${TASK_H_FULL_OUT:?set a fresh evidence directory outside the source tree}"
export TASK_H_FULL_OUT
export EFS_GIT_ID=fdfda8cfa EFS_GIT_DESCRIBE=fdfda8cfa
export EFS_GIT_BRANCH=agent/task-h-repair EFS_FAULTS=1
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
export EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8
export EFS_RAFT_OBS=1 EFS_READ_FRAG_PROF=1
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse
export PKG_CONFIG_PATH="$EFS_FUSE_PREFIX/lib64/pkgconfig"
export LD_LIBRARY_PATH="$EFS_FUSE_PREFIX/lib64"
export CPATH=HEADERS EFS_CLUSTER_SCRIPTS=CLUSTER
export EFS_D4_KEEP=1 EFS_K38_KEEP=1 EFS_PUFA_KEEP=1 EFS_B6_KEEP=1
export EFS_OPS_KEEP=1 EFS_K33_KEEP=1 EFS_K36_KEEP=1 EFS_PDC_KEEP=1 EFS_RSD_KEEP=1
unset ASAN_OPTIONS
mkdir "$TASK_H_FULL_OUT" || exit "$?"
python3 tests/test_sanitizer_gate.py > "$TASK_H_FULL_OUT/sanitizer-preflight.log" 2>&1 || exit "$?"
if grep -Ew 'NOTRUN|SKIP' "$TASK_H_FULL_OUT/sanitizer-preflight.log"; then exit 3; fi
make -j8 FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$EFS_FUSE_PREFIX/lib64" all tests/faults/efsd test-bins > "$TASK_H_FULL_OUT/initial-build.log" 2>&1 || exit "$?"
sha256sum efsd efs-fuse efs-mgmt efs-bench tests/faults/efsd > "$TASK_H_FULL_OUT/binaries-before.sha256"
bash scripts/make_test_summary.sh > "$TASK_H_FULL_OUT/runner.log" 2>&1
task_h_suite_rc=$?
# Preserve FAIL/INCOMPLETE even when evidence collection succeeds.
python3 - <<'PYCOLLECT' || exit "$?"
import os, runpy, hashlib, json
from pathlib import Path
source, output = Path.cwd(), Path(os.environ['TASK_H_FULL_OUT'])
runpy.run_path('scripts/qualify_publication_repair.py')['collect_full_outputs'](source, output)
files = sorted((output/'command-output').glob('command-*.log'))
expected = int((output/'summary.txt').read_text().split()[1])
assert len(files) == expected
rows = []
for copied in files:
    digest = hashlib.sha256(copied.read_bytes()).hexdigest()
    assert digest == hashlib.sha256((source/'tmp'/copied.name).read_bytes()).hexdigest()
    rows.append(dict(name=copied.name, bytes=copied.stat().st_size, sha256=digest))
with (output/'command-output/manifest.json').open('x') as f:
    json.dump(dict(source=os.environ['EFS_GIT_ID'], expected=expected, copied=len(rows), files=rows), f, indent=2)
PYCOLLECT
sha256sum efsd efs-fuse efs-mgmt efs-bench tests/faults/efsd > "$TASK_H_FULL_OUT/binaries-after.sha256"
cmp "$TASK_H_FULL_OUT/binaries-before.sha256" "$TASK_H_FULL_OUT/binaries-after.sha256" || exit "$?"
exit "$task_h_suite_rc"
)
```

The run-86 launcher also records before/after hashes of `efsd`, `efs-fuse`,
`efs-mgmt`, `efs-bench`, and `tests/faults/efsd`, verifies one copied transcript
per command against its source SHA-256, and writes a transcript manifest.
Before declaring closure, check no fixture processes or mounts remain.
Keep failed/finding-bearing stores, logs and checkpoints on-host; do not
prune them or export payloads. Fully clean disposable fixtures may follow
their normal test cleanup. Comparison fixtures stay kept for the open
historical K33/K36 investigation. These are three logical voters per private
fixture, not cross-host hardware-loss or power-loss qualification.

## Earlier deterministic and target-profile reproduction

Reproduce the deterministic checks from this commit:

```sh
python3 tests/test_b6_log_accounting.py
python3 tests/test_getchunks_collect_retry.py
CC='cc -fsanitize=address,undefined -fno-omit-frame-pointer' python3 tests/test_getchunks_collect_retry.py
git show c5acf58dd:src/server/raft_host.c > /tmp/pre-fix-raft_host.c
# Expected runtime assertion failure, not a passing product test:
python3 tests/test_getchunks_collect_retry.py /tmp/pre-fix-raft_host.c
```

Target-profile reproduction uses the committed qualification driver on Linux
with FUSE and build prerequisites and an exact candidate identity in
`EFS_GIT_ID` (latest source `fdfda8cfa`; historical examples used `d1030297c`
and `c92271a5f`),
`EFS_TEST_TARGET=test-all`, and fresh ROOT/OUT outside the source tree:

```sh
python3 scripts/qualify_publication_repair.py --profile nuc --root ROOT --output OUT
# RDMA host, mlx5_0 + private libfuse; full native suite run sequentially:
python3 scripts/qualify_publication_repair.py --profile rdma --targets-only --root ROOT --output OUT
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 bash scripts/make_test_summary.sh
```

The supplemental run uses release Valgrind 3.26 headers privately, without
installing packages: header archive SHA-256
`e0fee1635ecfdb04cb01fa55b46f0a43f933b6650fe3eee48b823cb8f3ff2646`.
The official source tarball checksum was verified against sourceware.org's
published SHA-512. The six external scripts are exported from cluster repo
`301e21671afec9025e8a1f08735e49bf6cac2a0c`, archive SHA-256
`43e9908e960c10beb9513a4ea171e10a040d4515e7e4e26c6806e04a38ec37b2`.
They run with mocked transports only; no live cluster command is issued.
The supplemental run.sh and manifest retain exact commands and prerequisites.
Reproduce in fresh same-source Linux trees; HEADERS is a private include root
containing the verified Valgrind release's `valgrind/{valgrind,memcheck}.h`,
and CLUSTER is the six-script export of the pinned cluster commit:

```sh
EFS_CLUSTER_SCRIPTS=CLUSTER python3 tests/test_cluster_stop.py
CC='cc -IHEADERS' python3 tests/test_rdma_memcheck.py
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
./tests/test_declaration_gate
./tests/test_rdma_xprt
for mode in normal interrupt tcp-violation deadline burst burst-asym churn; do
    ./tests/test_rdma_segmented "$mode" || exit "$?"
done
# Separate fresh source tree, leaving the native qualification binaries intact:
EFS_TRANSPORT=tcp make -j8 EFS_FAULTS=1 tests/test_follower_bound
EFS_TRANSPORT=tcp ./tests/test_follower_bound
# A host with usable TSan/ASan/UBSan/LSan runtimes:
python3 tests/test_sanitizer_gate.py
```

Diagnostic-only attempts are separate: integrate-proto-diag3 failed before
workload because its private libfuse path was missing; diag4 restored that
path and completed 24 K38 repetitions without reproducing the original
rejection. Neither is a product qualification or proof of historical cause.
Their closed-manifest hashes are respectively
`929681950b5cf880ea7ceff6351e9cc2add75c51e2be1340bd38fe96b663ebca` and
`45507ce1025af8f54b7823b28ac177f804a49264d813896fe70be32cb86dc3d5`,
under `/data1/efs/evidence/taskH-repair-20261026/`.

Full suites retain NOTRUN and overbudget entries as limits, never convert
failures into passes, and retain failed fixtures. The final retained-output and fresh-reader full result is stated at the top.
Historical failures and evidence limits remain separate from passing reruns;
no pruning or payload export is claimed.
