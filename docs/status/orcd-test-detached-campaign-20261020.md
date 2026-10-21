# ORCD-test detached packing/RDMA/.efs campaign — 2026-10-20

**Verdict: FAIL / controller BLOCKED (exit 125).** The controller stopped after
9,731.7 seconds (~2 h 42 min), at the post-load remount. Failed evidence and
stores are retained; no failed case was retried during this monitoring check.

## Results

Tested source: `1a9b96a3bb0c5f99959edd51790f52aca2ef5070`.
Four distributed storage hosts fcstor003–006, 2 TiB quota each, RDMA on
mlx5_2, direct I/O, background packing and `.efs` enabled; expiry/containers
disabled. Test mounts on those four hosts; the two-client suite used two
independent mounts on fcstor003 against the distributed storage cluster.

| Gate | Verdict | Evidence |
| --- | --- | --- |
| POSIX core | PASS | 216/216, four jobs |
| Two-client consistency | PASS | 67/67; both client processes on fcstor003 |
| Initial persistence / cold restart | PASS | prepare 27/27, verify 27/27 before large load |
| `.efs` commands | FAIL | `find` exits 75: directory not indexed yet |
| `.efs` options | FAIL | same exit 75 / not indexed yet |
| Concurrent queries, before and during load | FAIL | unexpected status -21 (NODATA); harness expects only -13 refusals |
| Large-file-count workload | FAIL | all four host jobs return 1; 117 writer-thread EIO reports |
| Post-load restart verification | BLOCKED / NOTRUN | initial remount cannot discover bootstrap cluster; sample verification never runs |
| RDMA exercised | CONFIRMED | nonzero RDMA data counters on all four nodes |
| Packing exercised | CONFIRMED | positive `packed_total` on all four nodes; not a full packed-data correctness pass |
| W123 loss-report continuity | NOTRUN | absent from pinned source |

Successful workload counters (not an independently crawled/durable file count):

| Host | Files | Writer threads reporting EIO |
| --- | ---: | ---: |
| fcstor003 | 2,035,744 | 26 |
| fcstor004 | 2,200,714 | 32 |
| fcstor005 | 1,967,117 | 27 |
| fcstor006 | 2,234,177 | 32 |
| Total | **8,437,752** | **117** |

The 40 M target was not reached (21.1%); neither was the tens-of-millions
scale gate. Counters exclude operations that threw errors, whose exact
persisted effects have not been inventoried. EIO records do not distinguish
open/write/close failures; root cause remains unclassified.

### Failure interpretation and harness limits

- The `.efs` snapshots advertise a 60,000 ms refresh minimum. Command
  tests give newly-created directories ~20 seconds, and concurrent-query
  tests accept BUSY but not NODATA. The failures are preserved; stale test
  expectations versus a product defect require investigation. No timeouts
  were increased or cases rerun to manufacture a pass.
- The controller continues independent suites and all scale jobs after
  query failures. It stops at the later remount-control failure, rather
  than pretending post-load verification passed.
- Failed remount job `b1f4fc9acb4b4838ba6472342361e122` logs `Could not
  discover cluster from any bootstrap node`. Startup uses only a fixed
  three-second delay per daemon, not a bounded cluster-readiness gate.
  Therefore this is a failed restart qualification, not proof of data loss
  or an unrecoverable store. At inspection all four restarted daemon jobs
  were active and emitting compaction/GC logs.
- Large-load post-restart sample verification did not execute. Full payload
  verification was never part of this scale harness. Only the earlier
  27-case persistence fixture has a verified cold-restart result.
- RDMA evidence scan found 3,759/4,141/2,634/1,289 nonzero-data counter lines
  on fcstor003/004/005/006 respectively in completed daemon logs. Sample
  lines show `post_setup_tcp=0/0 violations=0`; this is not an exhaustive
  audit of every connection. Observed maximum `packed_total` values were
  2,402,276 / 1,072,806 / 588,173 / 1,406,265 (not unique global counts).
- No deployment change, cleanup, restart or retry was performed by the
  monitor. Worker-owned restarted daemons retain their original 15,000 s
  deadlines; the monitor did not extend them. Stores and evidence remain.

Raw evidence stays outside Git at
`/home/erbmi1/orcd/scratch/efs/orcd-test/campaign-20261020/`:
`status.json`, `controller.log`, `exit-status`, `*-scale.json`, per-host
`mail/<host>/<job>.log`, and FUSE log snapshots. The source archive SHA256 is
`f4abcd639b3dbc03e9cb76aeac5c2efd28ba375bb12722a51c27817c3a4ce777`;
`binaries.sha256` records the built executables. Reproducer is
`scripts/orcd_test_campaign.py`; preserve its original results and use a
separate explicitly authorized run for any future corrected harness.

## Authorized repair follow-up (in progress)

The user authorized fixes and separate reruns, with existing-payload verification
first. Original failures above remain FAIL. The original build is still serving
retained stores; no repaired daemon has been deployed yet.

- All four read-only mounts recovered once the restarted servers were ready.
  `scripts/orcd_verify_existing.py` reads every acknowledged file against the
  deterministic 256-byte oracle, including exact length/EOF. Target: all
  8,437,752 files. Failed-write paths are audited separately. Verification is
  **RUNNING**, not a payload PASS. At 2,114,639 checked, two reads returned
  EBUSY (fcstor003 and fcstor006), with no observed payload mismatch. These
  failures are retained without retry. Raw progress is under
  `../repair-20261020/existing-payloads/` beside the original campaign.
- All 117 EIOs correspond to `flush-meta` returning BUSY (`efs_rc=-13`)
  after the eight-second publication budget. This identifies the failing path,
  not proof that uncertain writes lost data.
- Thirty-second 99 Hz daemon profiles during verification show ledger decoding
  at 33.43%, 24.29%, 31.71%, 54.00% self CPU on fcstor003–006. Caller stacks
  identify `pack_retire_cb` and `pk_abandon_cb` beneath `lsm_scan_prefix`.
  Those whole-prefix scans hold the KV read lock while decoding every ledger;
  apply needs the writer lock. Quadratic digest-uniqueness validation magnifies
  that stall. Raft terms continue advancing with only verifiers and maintenance.
  This is the repair hypothesis; end-to-end EIO elimination remains **NOTRUN**.
- Candidate fix: pointer-sort duplicate validation (same format/checks), and
  yielding maintenance scans with 128-key ledger / 4096-key index pages,
  including tombstones. These are non-snapshot hints; mutation authority stays
  in serialized apply. No close timeout or durability requirement is relaxed.
- Query tests now derive timing from advertised refresh policy, warm the fixture
  before concurrent reads, and require multiple successful snapshot generations
  over at least two refresh periods. Exact count/byte and payload oracles remain.
  A NODATA response never counts as a successful concurrent query.
- `scripts/orcd_test_ready.py` checks recovered export, full hosted voter sets,
  self-reported leaders, matching term/views, applied leader commits and a fresh
  root read. Two stable observations replace mounting after a fixed startup sleep.
- Initial isolated Linux build and ledger, ledger-recovery, packed-switch and
  bounded-scan tests PASS. Writer-progress regression demonstrates a competing
  KV writer completes before a yielding maintenance scan ends. Python count/byte,
  payload-corruption and readiness regressions PASS. A native macOS build cannot
  compile the Linux `fopencookie` logger; Linux is the validation platform.

Candidate binaries are build artifacts only, under
`/home/erbmi1/orcd/scratch/efs/orcd-test/repair-20261020/source/`; this is not a
second deployment. Raw profiles are `<host>-perf.data` in that repair directory;
mailbox job logs remain in the original campaign mailbox with `profile-`,
`callers-`, `verify-all-`, and `build-repair-` prefixes. Commands:

```sh
# On each fcstor003–006, with the retained filesystem mounted read-only:
python3 scripts/orcd_verify_existing.py /path/to/campaign-20261020 /path/to/existing-payloads
# On a test client, before any remount after restart:
python3 scripts/orcd_test_ready.py 172.16.223.57:17432 172.16.223.58:17432 172.16.223.59:17432 172.16.223.60:17432
# Profiles (daemon PID explicitly identified per host):
perf record -F 99 -g -p "$daemon_pid" -o "$host-perf.data" -- sleep 30
perf report -i "$host-perf.data" --stdio --children --percent-limit 5 -g caller,5,callee,function,percent
```

Do not clear original fixtures/results when deploying the candidate. Full retained
verification, repaired-build identity, loaded EIO/query rerun, 40 M target and
subsequent cold-restart verification remain open until new evidence is recorded.

### Follow-up at 2026-10-21 01:27 UTC

Baseline verification: 2,964,468 of 8,437,752 checked, five EBUSY reads
(1/1/0/3 on fcstor003/004/005/006), no observed mismatch. All workers and
original daemons remain alive. This is still **RUNNING / acceptance open**.

Reviewed newer commit `d7ddcc04510722e203ff52e9d925ab286310bd20` (read buffer
headroom under pending writes). It includes repair `602e67d2c`; its explicit
read allocation budget and deadline are relevant to the subsequent mixed
workload. It does not explain away the read-only baseline's EBUSY failures.
`6a6a2bb2b` changes only another cluster's collection script, not product code.

**Current deployment candidate: `d7ddcc045`, built but NOT deployed.**
Build artifacts: `repair-20261020/source-d7ddcc045/`, built on fcstor003 at
01:28:38 UTC. `make -j8 all tests/test_bufpool`, `test_bufpool`,
`test_pack_ledger`, `test_pack_ledger_recovery`, `test_pack_switch`,
`test_packed_reader`, `EFS_TEST_KV_WORK_ONLY=1 test_kv_lsm`, and
`python3 tests/test_orcd_ready.py` PASS (job exit 0, 18.1 s, no leftovers).
Full KV tests previously passed on `602e67d2c` (unchanged KV code).

| Binary | SHA256 |
| --- | --- |
| efsd | `c7c3ac34c1fb1041e16be93765099769275e230fdbe5b9b98bf9b0e09c7f4795` |
| efs-fuse | `6886db9235e6e72cd1305802baef539e3ba65dfc613c949101b8016e84315089` |
| efs-mgmt | `5dc972da2b166ac634c5f2bd9da8bfb9b20c643e70b4c3497a3ea17c0ef04b2c` |

Verifier job deadlines are around 03:31 UTC; old daemon deadlines around
03:51 UTC. `scripts/orcd_verify_existing.py --resume CHECKPOINT_DIR` now
supports continuing only unchecked records into a **different** output directory.
Prior failure counts/details remain failures; the old checkpoint is untouched.
`python3 tests/test_orcd_verify_resume.py` checks that an already-checked path
is not reread, remaining payloads are verified, and a prior failure cannot
become PASS. The running baseline script has not been replaced or restarted.
If continuation is needed, record the interruption, cleanly restart the
**original build**, and finish baseline verification before deploying repairs.
Do not mistake baseline EBUSY for verified payload content; every such path
must also be read in the later, separately reported fixed-build verification.

### Same-build baseline continuation — 2026-10-21 02:14 UTC

The original verification would exceed its bounded job lifetime. It was stopped
at 4,761,593 checked paths (1,039,045 / 1,688,501 / 917,701 / 1,116,346 per
host), carrying 24 EBUSY failures (3/2/0/19), with no observed payload mismatch.
Original checkpoint files and cancelled-job results remain unchanged. The
worker initially reported verifier PIDs as leftovers; independent process
inspection and explicit `/proc/PID` absence checks established that they had
subsequently exited before unmounts were permitted.

All four read-only mounts stopped cleanly (exit 0). The four original daemon
jobs were deliberately cancelled after unmounting (wrapper exit 130, no
leftovers). The **same `1a9b96a3bb0c` binaries**, storage paths, quotas and
feature/transport profile were restarted. No store was cleared and no repair
binary was deployed. New daemon job limits are eight hours from ~02:16 UTC.

The readiness gate passed in **61.45 s**, after connection refusals, two
10-second status timeouts and temporary absent leaders. It observed both
metadata groups with matching terms and applied commits, then a successful
fresh root read. All four read-only remounts returned 0. This establishes
startup readiness on this restart; it does not establish full payload recovery.

Verification resumes only unchecked records into
`repair-20261020/existing-payloads-resume1/<host>.json`, with five-hour job
limits from ~02:20 UTC. Counts are cumulative and include the earlier checkpoint;
**do not add the old and new counts**. All 24 earlier read failures remain in
these results. The journal `repair-20261020/baseline-restart-20261021T0214.json`
records old/new job IDs, unmounts, readiness, remounts and resumed verifier IDs.
Use this journal and live mailbox requests for exact deadlines. The old
03:31/03:51 deadlines no longer govern active jobs.

Reviewed subsequent commits `628664b7d` (read-admission regression) and
`5c41111d1` (private-cluster RSS collection). Neither changes product code.
`python3 tests/test_read_admission.py` PASS locally. Candidate remains
`d7ddcc045`, not deployed. Original campaign FAIL and full qualification gates
remain open.

## Prepared run

- Shared evidence: `/home/erbmi1/orcd/scratch/efs/orcd-test/campaign-20261020/`.
- Source `1a9b96a3bb0c5f99959edd51790f52aca2ef5070` built successfully on
  fcstor003; `source/build.exit` is 0. Full build output and binary SHA256s
  are retained under the evidence path.
- Detached `orcd-campaign-worker` screens run on fcstor003–006. Shared-file
  job dispatch avoids dependence on forwarded SSH credentials, since
  fcstor003 cannot SSH to fcstor004 without them.
- Each worker passed success (0), intentional failure (7), and timeout
  (124) smoke jobs, with no leftover processes. These are harness checks,
  not EFS acceptance tests.
- User requests replacement of the existing deployment, not a parallel
  cluster, and large quotas/tens of millions of files. Prepared profile:
  2 TiB per storage node; 32 writer threads per host, up to 10 M files per
  host (40 M total target), bounded by the campaign time window. Actual
  achieved counts must be reported, not inferred from the target.
- User approved stopping old clients on fcstor007–013; stop returned 0 for
  all seven. The unrelated `fstor007` was not accessed.
- Each old daemon's PID and command are retained in `<host>-previous.json`.
  `/tmp/efs` now points at the verified shared source. Old binaries are
  retained offline at `/tmp/efs-before-campaign-20261020`; there is no
  second running deployment. New stores use `/data1/01/efs-orcd-test-campaign`.
- Controller source: `scripts/orcd_test_campaign.py`, staged as `campaign.py`.
  Screen `orcd-campaign-controller` on fcstor003 started at approximately
  2026-10-20 21:00 UTC (16:00 America/Bogota). Status timestamps are UTC Unix
  seconds; use the actual `started` field for exact timing.
- A 30-minute monitoring heartbeat is configured in this chat. It reports
  failures/completion without restarting failed cases.
- If fewer than 40 M files are reached within the window, the controller
  reports `INCOMPLETE_SCALE_TARGET` even when correctness checks pass.

Actual controller launch on fcstor003:

```sh
R=/home/erbmi1/orcd/scratch/efs/orcd-test/campaign-20261020
screen -dmS orcd-campaign-controller bash -c "python3 -u $R/campaign.py controller $R > $R/controller.log 2>&1"
```

Reconnect through `ssh -A erbmi1@orcd-login.mit.edu`, then
`ssh fcstor003`. Use `screen -r orcd-campaign-controller`;
inspect `$R/status.json`, `$R/exit-status`, and per-host `$R/mail/` logs.
The detached screen launch was verified; per-case results remain pending.

## Authorization and scope

Human instruction relayed by the coordinating chat: run 3–4 hours of
server-side, disconnect-safe testing with packing, RDMA and `.efs` enabled.
Existing data on fcstor003–fcstor006 may be discarded; fresh format is
permitted and backward compatibility is not an acceptance requirement.
Limit all daemons, controller and test mounts to those four hosts. Do not
operate on fstor007, or on the usual fcstor007–fcstor013 test clients.
Use direct I/O; containers and expiry remain disabled.

## Source and access preflight

Inspected local devel: `1a9b96a3bb0c5f99959edd51790f52aca2ef5070`.
No remote build identity has been established. Reconfirm and pin the exact
committed source before staging; never deploy concurrent uncommitted work.

Both commands failed with `Permission denied (keyboard-interactive)`:

```sh
ssh -A -o BatchMode=yes -o ConnectTimeout=15 erbmi1@orcd-login.mit.edu
ssh -A -o BatchMode=yes -o ConnectTimeout=15 orcd-login
```

The earlier authentication failure is resolved by an interactive login. Verify that
server-to-server authentication survives loss of the Mac SSH agent; detached
screen alone does not guarantee that later remote control commands will work.

## Campaign requirements once access returns

- Inventory processes, mounts, IB devices and free space on fcstor003–006.
  Use uniquely named evidence paths and explicit process ownership; replace the existing deployment as requested.
  Never use broad process kills or the standard controller unchanged: it
  includes seven clients outside this scope.
- Stage one pinned source snapshot, record checksums/build IDs and compile
  logs. Verify feature configuration from running processes and actual
  packed mappings/fragment inventory. Verify RDMA data counters increase;
  a transport environment variable alone is insufficient evidence.
- Run controller AND case launchers in detached server-side screen sessions.
  Persist atomic status/heartbeat JSON, per-case stdout/stderr, exit status,
  timeout status, daemon logs, profile and source/build identity outside Git.
- Use a 3.5-hour admission window and a maximum 30-minute case deadline, so
  the final case finishes within four hours. Include bounded cleanup time in
  this budget. Never silently count a truncated case as PASS.
- Exercise metadata/namespace and EOF consistency, two-client visibility,
  fsync/payload persistence through orderly restart, packing transitions and
  reclaim/compaction, and concurrent `.efs` queries during I/O. Add recovery
  and loss-report continuity only where the pinned branch supports them;
  record unsupported cases as NOTRUN.
- On failure or timeout, retain fixture and evidence, stop only that case's
  owned processes/mounts, verify isolation and resources, then continue with
  independent cases. Never retry a failed case until it passes or erase its
  artifacts. Preserve failures in the aggregate verdict. Stop only when
  safe cleanup/isolation cannot be established.
- Reconnect instructions must name the actual controller host, screen name,
  evidence path and status file after launch. They do not yet exist.

## Harness applicability

The coordinating agent's `w123-long-campaign.py` and
`w123_campaign_cleanup.py` provide the continue-after-failure reference.
Their local source is in the `devel-w123-recovery` worktree; those scripts
are not present in inspected devel. Do not import recovery implementation
or claim its private TCP tests qualify this distributed profile.

Existing `tests/live/rdma_posix_gates.sh` is also unsuitable unchanged:
it uses loopback/same-host topology and explicitly disables `.efs`.
Adapt actual distributed tests rather than relabeling that harness.

## Remaining gates

Full disconnect survival, EFS test, final cleanup and RDMA counter
verification remain pending. No final qualification verdict is claimed.

### Baseline progress — 2026-10-21 03:10 UTC

Same original build/profile and resumed verifier described above: 7,650,358 of
8,437,752 acknowledged paths attempted (90.7%). Per-host counts are
1,743,916 / 2,200,714 / 1,612,513 / 2,093,215; preserved EBUSY counts are
4 / 10 / 3 / 31 (48 total). No observed payload mismatch or missing
acknowledged file; unread EBUSY payloads remain unverified.

fcstor004 completed every acknowledged path, returning **FAIL** (exit 1,
no leftover processes) because of its ten read failures. Its separate audit
found all 32 original failed-write paths contain the exact expected 256-byte
payload. This supports uncertain close/publication outcomes for those writes;
it does not turn their EIO reports into successes or establish other hosts'
outcomes. Evidence: `existing-payloads-resume1/fcstor004-failed-write-outcomes.json`
under the repair directory. Three verifiers remain active with over four hours
of bounded runtime remaining; daemons have over seven hours.

New commits through `827758454` add W125 test/collection scripts only; no newer
product changes require rebuilding the pinned `d7ddcc045` candidate. It remains
**NOT DEPLOYED** until baseline completion and review of all failed-write audits.

### Candidate update — 2026-10-21 04:00 UTC

Baseline still running at the 03:57 check: 8,399,213 / 8,437,752 paths
attempted, 49 EBUSY failures, no observed payload mismatch. fcstor004/006
completed; all 64 uncertain-write paths on those hosts match expected payloads.

Reviewed new product changes `f958852fd` (fold a packed base into per-file
storage when a write grows past the threshold) and `0b2802a06` (bound REAP
scan work including tombstones). Both are relevant to packed workload
qualification. Local `test_span_selection.py`, `test_reap_fairness.py`, and
`test_gc_reap_shutdown.py` PASS. No other product files changed since d7ddcc045.

New pinned candidate **643f6104f** is being staged and built, **NOT DEPLOYED**.
The detached build on fcstor003 waits for the complete archive and verifies
SHA256 `a099b46a0b3a3e64c0a76b234482136f677fba84d4a031ad02bd5e999fc43f08`
before extraction. Build identity/job is recorded in
`repair-20261020/candidate-643f6104f.json`; artifacts will be in
`repair-20261020/source-643f6104f`. Its mailbox log records build, buffer-pool,
ledger/recovery, packed-switch/reader, bounded KV, REAP, span and readiness
regressions and binary hashes. Check the job result before using this candidate.
The original build remains live until baseline completion; no store is removed.

### Baseline complete; repair deployed — 2026-10-21 04:15 UTC

Original-build verification attempted **all 8,437,752 acknowledged paths**.
Verdict **FAIL**: 49 EBUSY reads (4/10/4/31); no observed payload mismatch or
missing acknowledged file. The failed reads remain unverified. The separate
uncertain-write audit found all **117** original failed-write paths contain
the exact expected payload (26/32/27/32). All baseline results are preserved.

Candidate **643f6104f** passed the isolated build and every regression listed
in the preceding section (job exit 0, no leftovers). Built at 04:09 UTC:

| Binary | SHA256 |
| --- | --- |
| efsd | `8e70317f9f22d46511115a8787c9b40e250b51aaa6b41abab3de876e6757da16` |
| efs-fuse | `38678ab3aaddff29628b3f64ce43082b22b4aa35d3fa5f2381232c40f2eb0d59` |
| efs-mgmt | `16bdca35c1b3ffa8cd2bc7d821a0b1cc33b42eb6f22f06f9e6df0a94ceb8d575` |

All original mounts stopped with exit 0; all four owned original daemon jobs
stopped deliberately (wrapper 130, no leftovers). The repaired binaries
started at ~04:13 UTC against the **same stores/profile**, with 12-hour job
limits. Readiness passed in **9.58 s** after an initial absent-leader response;
this is gate runtime, not a controlled startup-performance comparison. All four
client mounts and the second mount on fcstor003 succeeded. `/tmp/efs` now
points at the repaired source on these four hosts.

Authoritative deployment/job journal: `repair-20261020/deployment-643f6104f.json`.
New evidence: `repair-20261020/qualification-643f6104f/`. Full verification
started from zero on all four hosts using `scripts/orcd_verify_existing.py`
against the original campaign manifests, output `full-existing/<host>.json`,
with six-hour limits from ~04:15 UTC. Every prior EBUSY path is included.
POSIX core runs concurrently against a separate test fixture. Acceptance is
**OPEN**: repaired full verification, query suites, 40 M scale and subsequent
orderly restart/full verification remain pending. No original failure is erased.

### Repaired-build suites — 2026-10-21 04:30 UTC

On deployed 643f6104f with full retained-payload verification running concurrently:
- POSIX core **FAIL: 215/216**, `mtime_monotonic_many_writes` exceeded its
  unchanged 15-second deadline. Fixture retained at
  `/tmp/efs-mount/posix-fcstor003-nwt0y2yy`; result `qualification-643f6104f/posix.tsv`.
- Two-client **PASS: 67/67** (two independent mounts on fcstor003).
- Focused `.efs` commands **PASS**. Options and concurrent-load suites RUNNING.
- Retained verification reached 1,339,726 paths with zero errors at inspection;
  this remains incomplete, not a full-payload PASS.

Diagnostic strace could not run: host rejects `PTRACE_TRACEME` with EPERM;
that diagnostic failure is preserved. `scripts/orcd_mtime_probe.py MOUNT`
reproduces the 80-append pattern with per-operation timings, retains its fixture,
and asserts mtime monotonicity. Its separate bounded 120-second job completed
in 11.64 seconds: close total 8.11 s / max 489 ms, open total 1.95 s / max
222 ms, stat total 1.56 s / max 262 ms. This suggests cumulative publication
latency near the suite deadline; it neither clears the original timeout nor
proves its cause. No acceptance timeout or durability rule was relaxed.
Raw probe/trace-attempt jobs are recorded under `tests` in
`repair-20261020/deployment-643f6104f.json`, with logs in the original mailbox.

### Query gates completed — 2026-10-21 04:42 UTC

On the same repaired build/profile under full verification load, command options
and concurrent queries now **PASS** as well. Concurrent readers completed
5,853 requests, zero BUSY, across snapshot generations 16/17/18. Per-reader
p50 was 17.2 ms, p95 148/154 ms, p99 789/781 ms. The write portion published
19,379 files and verified every payload; create p50/p95/p99 was 39.6/135/553 ms,
with 42.46 s convergence. No refresh error was recorded. These are focused
loaded gates, not the still-open 40 M scale qualification.

Full retained verification reached 2,955,114 checked paths with zero errors.
The unchanged 15-second mtime case **failed again when isolated** while those
readers ran (`mtime-gate.tsv`, retained fixture
`/tmp/efs-mount/posix-fcstor003-1h16u6yi`). The earlier successful timing probe
does not resolve this repeatable loaded-latency failure. Further diagnosis is
open; no acceptance timeout increase or product fix is claimed. Raw job IDs,
including a preserved-fixture/client-log inspection, remain in the deployment
journal.

### Loaded append diagnostics — 2026-10-21 05:00 UTC

Retained verification advanced to 4,630,228 paths with zero errors; more than
five hours remain on verifier jobs. Timed-out mtime fixtures retained 74 and
76 bytes (initial byte plus 73/75 of 80 appends). Client logs include a successful
INODE_APPEND RPC taking 1.37 s. Three diagnostic 80-append runs completed with
monotonic mtime in **18.46 / 9.11 / 17.02 s**. This establishes variable cumulative
latency exceeding the 15-second gate, not a demonstrated mtime regression; the
acceptance failures remain open.

Two 25-second, 99 Hz daemon profiles completed successfully during these probes.
On fcstor003, yielding maintenance scan inclusive CPU was 24.77%, ledger decode
12.46% inclusive / 1.47% self, and sorting 9.50% self; packing object decode
was 21.71% inclusive. fcstor004 showed Raft apply, transport and scheduling work.
CPU percentages overlap and do not measure blocking time or establish the
current leader. No root-cause claim or further product change is justified yet.
Raw files: `qualification-643f6104f/fcstor00{3,4}-mtime-perf.data` and
`mtime-probe-{1,2,3}.jsonl`. Reproduce the captures with the earlier perf command
and `scripts/orcd_mtime_probe.py /tmp/efs-mount`; current daemon PIDs and jobs
are in the deployment journal. New diagnostic `tests` entries use
`{"host": "fcstor003", "job": "..."}`; earlier suite entries are job strings
on fcstor003.

### Next-scale preparation — 2026-10-21 05:12 UTC

Retained verification reached 6,220,146 paths without errors; it remains running.
For the next fresh scale fixture, `orcd_verify_existing.py` now accepts
`--fixture-root /tmp/efs-mount/NEW-DIRECTORY`, containing `scale-HOST` directories.
This keeps existing workload data intact while using the same exact payload
oracle and separate manifests/output. Checkpoints record the fixture root and
refuse resumes from another fixture. The regression verifies both retention
of prior failures and rejection of the wrong fixture. Existing deployed binaries
and running verification scripts are unchanged; new scale writes have not begun.

### Full retained payload gate PASS — 2026-10-21 05:43 UTC

Repaired build 643f6104f verified **all 8,437,752 acknowledged payloads** with
zero errors and exact contents/length/EOF: 2,035,744 / 2,200,714 / 1,967,117 /
2,234,177 on fcstor003/004/005/006. All four verifier jobs exited 0. This
includes every one of the original 49 EBUSY paths. Separate audits also match
all 117 uncertain-write payloads. Original baseline failures remain preserved.

An unchanged four-worker POSIX core run now runs after all verification readers
finished, in `posix-after-verify.tsv`. It distinguishes load-sensitive deadline
failures from an unconditional regression; it cannot erase loaded failures.
Next is a separate 40 M-target fixture on the same deployed profile, followed
by exact verification and orderly cold restart. The updated full verifier is
staged as `repair-20261020/verify_scale_f6d83c895.py`.

### Unloaded core PASS; new scale running — 2026-10-21 05:44 UTC

After verification readers finished, unchanged POSIX core **216/216 PASS**
in 34.6 s, including the 15-second mtime case. Prior loaded failures remain
open as load-sensitive latency; no extra product fix or relaxed timeout was used.

Started a separate **40 M target** workload: 32 writers per host, 10 M target
each, 18,000-second duration / 18,300-second job limit from ~05:44 UTC.
Fixture root `/tmp/efs-mount/qualification-643f6104f-scale`; each host writes
its own `scale-HOST` subtree. Evidence/manifests:
`repair-20261020/qualification-643f6104f/scale-40m/`. Current job IDs are in
the deployment journal's `scale` section. Old fixtures remain untouched.
The workload's sampled checks do **not** qualify full payload correctness.
After writes, use staged `verify_scale_f6d83c895.py SCALE_RUN NEW_OUTPUT
--fixture-root /tmp/efs-mount/qualification-643f6104f-scale`, then orderly
restart/full verification with preserved checkpoints before bounded job expiry.
Actual new file counts and writer outcome are pending; 40 M is a target only.

### Query refusal during scale — 2026-10-21 06:12 UTC

Scale writers remain active: 2,239,497 new acknowledged files (551,991 /
563,069 / 561,940 / 562,497), no reported writer errors at inspection.
The independent concurrent-query gate during scale **FAILS** with NODATA (-21)
after warmup, despite stats snapshots advancing through generations 106/107
and reporting refresh_error=0. Job `queries-scale-5d55f286091b4e29b23523c2c2a3f87d`
and its fixture/log are preserved. Earlier lighter-load query passes do not
qualify this heavier workload.

NODATA can originate in live-vs-catalog directory binding/ancestry validation;
healthy global refresh alone does not prove the request's root is indexed on
the serving snapshot. The exact failing stage is not established. The harness
now records warmup snapshot and failing reply generation/boot/terms/root plus
success count; it still fails on NODATA. A new diagnostic fixture/job is tracked
as `query-scale-diagnostic` in the deployment journal, using separately staged
`query_load_diagnostic.py` with deployed query helpers.

Reviewed new commit e12e23394: larger idle compaction batches with a ten-second
between-output budget check. Relevant to later reclaim qualification, but not
an established fix for this query refusal; not deployed into the running scale
experiment. Its time bound is checked between outputs and cannot preempt a
single long compaction.

### Intermittent query investigation — 2026-10-21 06:42 UTC

Scale reached 3,745,581 new acknowledged files with no writer errors; all four
writers remain active with ~4.1 h remaining in their bounded jobs. The diagnostic
query run passed without a product change: 4,506 successful queries and 56 BUSY
refusals, three generations, all 5,580 writer payloads verified. This establishes
intermittency, not resolution of the preserved NODATA failure.

Code review shows any dual-group-capable node can serve its own published
catalog. Helper warmup and a separately routed raw request may reach different
catalogs; this is a hypothesis to test, not an established cause. New bounded
`scripts/orcd_query_snapshot_probe.py` creates one retained directory, warms it
through the helper, then records direct per-node status/boot/generation every
two seconds for 45 rounds. It does not accept NODATA as a passing gate.
Run with deployed `tests/live` on PYTHONPATH and the campaign endpoint environment.
Staged as `repair-20261020/query_snapshot_probe.py`, job recorded under
`query-node-snapshots` in the deployment journal. No deployment or timeout change.
