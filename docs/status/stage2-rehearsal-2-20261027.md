# Task P — second Stage 2 rehearsal

`agent/devel-task-p` @ `b8f232f40`: full-gate TOTAL line pending (NOTRUN,
queued); base comparison pending. Evidence: `docs/status/stage2-rehearsal-2-20261027.md`.
Original three-member matrix RUNNING on intel2; current-develop follow-up QUEUED.
No product defect confirmed. Driver defect D-P1 fixed, live check pending.
Physical-host/RDMA release acceptance NOTRUN: single-host TCP rehearsal only.

## Scope and current state

**RUNNING on intel2 after the user authorized any available host.** Task P runs all selected
Stage 2 gates, serially for loopback-3 and loopback-5, then the eight-job
`make test-all` gate. The assigned work excludes product fixes: a product
failure gets a K row and retained trace. Driver, gate and environment defects
may be corrected. No real cluster, shared checkout, Task H/I fixture or
external repository is modified.

The base matrix validates: 26 items, nine topologies. Both requested
loopback topologies select all 24 named gates. Each also records its final
identity check. Raw results will be retained outside Git under
`intel2:/home/maik/efs-artifacts/taskP-20261027/`; the first run is active and no
closed manifest is claimed yet.

A read-only host check found no running EFS daemon/client, but three Task H
mounts remain under `~/efs-taskH-repair-20261027/attempt-{37,40,43}`.
Task H's chat still describes a nuc resync investigation. Those mounts and
retained trees are not Task P's to clear. Task I moved later qualification
to xorinox and node9900; that alone is not a coordinator handoff of nuc.

Intel2 preflight: no EFS processes or mounts, no screen jobs, eight CPUs,
FUSE 3.16.2, approximately 12 GiB available RAM and 838 GiB free disk.
CPU samples were idle; initial load averages reflect earlier work.

The committed `scripts/task_p_rehearsal.py` serializes build, regressions,
both full matrices, manifest verification and the final code gate. It
refuses to advance while Task P processes or mounts remain. Gate failures
remain failures; no product source changes are authorized.

Frozen source `3ca4f7e6b233d381c1236ca5752d7f011cc40cec`, based on devel
`24dad8e4b`. Detached screen `taskP-rehearsal`; source `~/efs-taskP/src`.
Archive SHA256 `d4c6b0ba97fa56c4a125253f90a265fcf2f9cb795364f892e75b82fc9938d912`.
Build: `efsd 3ca4f7e6b233d381c1236ca5752d7f011cc40cec (branch agent/devel-task-p, built 2026-10-28T02:16:24Z on intel2)`.
Build PASS 29.752 s; full Linux driver regression PASS 4.015 s; soak
regression PASS 0.188 s. The Mac limitation below is superseded by this
Linux regression result, not erased.

`identity` passed; `topology` reports the expected one-host failure-domain
NOTRUN. Remaining gates are running/queued, not yet qualification evidence.
The original controller owns both serial matrices and its final code gate.
The corrected current-develop controller waits for it to finish; it never
rebuilds or changes the active original source tree. `scripts/task_p_compare.py ARTIFACT_ROOT`
extracts Task A's corrected wall table and flags doubled observed walls.

## Driver correction prepared

The matrix selected a four-hour endurance soak for both loopback rehearsals.
Task P specifies the rehearsal profile. The loopback fixtures now explicitly
select `soak_hours=0.25`, `soak_profile=rehearsal`; the driver passes that
profile to the existing soak script. All other topology defaults remain
four-hour endurance. This is a test-workload selection, not an EFS product
behavior switch. Rehearsal does not qualify the endurance RSS window.

Checks completed locally:

- `python3 scripts/qualify.py plan --check --matrix scripts/qualify_matrix.json`: PASS.
- `python3 tests/test_soak_driver.py`: PASS, including schedule, refusal and corruption controls.
- `python3 tests/test_qualify_driver.py`: incomplete on macOS. After loopback
  socket permission was granted it reached the Linux-only `/proc/PID/stat`
  identity step and failed there; no driver regression PASS is claimed.
  The spawned stub was checked absent. The complete test must run on Linux.

## Planned method and comparisons

Freeze the current source and record its full commit, archive hash, build
identity and tracked-source manifest. Build privately on idle intel2 with
`EFS_FUSE_PREFIX=/usr`, eight build jobs. Use detached screen, private TMPDIR,
free 19xxx ports and owned fixture roots only. Run:

```sh
python3 scripts/qualify.py plan --check --matrix scripts/qualify_matrix.json
python3 tests/test_qualify_driver.py
python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json \
  --topology loopback-3 --candidate FULL_COMMIT --allow all \
  --out "$HOME/efs-artifacts/taskP-20261027/loopback-3" \
  --work "$HOME/efs-taskP/work-3"
python3 scripts/qualify.py run --matrix scripts/qualify_matrix.json \
  --topology loopback-5 --candidate FULL_COMMIT --allow all \
  --out "$HOME/efs-artifacts/taskP-20261027/loopback-5" \
  --work "$HOME/efs-taskP/work-5"
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
```

Each full matrix continues through gate verdicts; a failed topology is not
retried into PASS. Preserve failure logs and classify each non-PASS. Compare
per-gate wall times with Task A's corrected runs in
[Task A](task-a-20261025.md); doubled walls are findings, with different
hardware/load and added D5/soak scope explicitly separated from regressions.
Record outer process wall and gate-sequence wall separately.

## F3 rehearsal ledger

These cells describe one physical host, not the checklist's distinct-host
release acceptance. Replace with the driver's exact F3 rows after completion.

| Evidence | loopback-3 | loopback-5 |
| --- | --- | --- |
| Candidate and profile | Identity PASS; physical failure domains NOTRUN (one host) | NOTRUN — queued |
| Placement/capacity and decode | B-checks PASS; B6 pending | NOTRUN — queued |
| POSIX and concurrent I/O | core/extras/two-client/durability PASS; remaining gates pending | NOTRUN — queued |
| Packing/shared-fragment GC | NOTRUN | NOTRUN |
| Enabled `.efs` under load | NOTRUN | NOTRUN |
| Full payloads + orderly restart | NOTRUN | NOTRUN |
| Scale/progress evidence (E4, D4) | D4 PASS; soak NOTRUN (D-P1); E4 pending | NOTRUN — queued |

After closure, verify driver manifests, retain exact methods/build identities
and full SHA256SUMS outside Git, then prune only inactive Task P bulk with
`edelete` and record PRUNED.txt. Real-host TCP/RDMA and physical-node release
acceptance remain outside this single-host rehearsal.

## Continuation on current devel — October 27/28

The original freeze predates merged waves 21–22. Its controller stays unchanged
and is allowed to finish both complete matrices and its code gate. The branch
merged devel `e7e72e2674e892e3667c3dbdb1a53eb8911b644c` at `194c7f3d2`,
including the K50 namespace retry fix. The user's authorization to use any
available host was verified from the original Task P session; intel2 remains
the assigned active host. Nuc's Task H fixtures were inspected read-only and
left untouched. No new ORCD stage or duplicate host run is launched.

### D-P1 — standalone soak overlaps the driver's own fixture

**Driver defect, fixed in `b8f232f40`; live corrected matrix pending.** The
original three-member soak returned NOTRUN in 0.322 s: `foreign_jobs()` found
the qualification fixture's live `efsd` processes. This is not an environment
skip to accept for Task P; the standalone soak starts its own cluster and
needs the parent fixture quiesced. Its original result and detailed command
log remain at `intel2:~/efs-artifacts/taskP-20261027/loopback-3/gates/soak/`.

The driver now suspends only a driver-owned fixture, with explicit restart
permission and an in-use mount check, before invoking the standalone plugin.
It requires clean client/daemon stops; after the plugin it checks cluster
readiness and remounts. Plugin FAIL/NOTRUN and exceptions remain failures or
NOTRUN, and failed restoration never becomes PASS. An externally managed
cluster is refused rather than stopped. The soak's process ownership check
is unchanged: no PID/path allowlist or quiet-host bypass was added.

New fast regression `tests/test_qualify_standalone.py` checks stopped-fixture
isolation, refusal without ownership/permission or with a busy mount,
plugin PASS/FAIL/NOTRUN preservation, stop failure, restore failure and
exception restoration. On intel2 the negative-control driver `194c7f3d2`
fails the new test; the fixed driver passes. Source/test copies and both logs
remain in `~/efs-artifacts/taskP-20261027-driver-fix/` (open evidence, no closed
manifest yet). The committed-source matrix plan, standalone regression and
`test_soak_driver.py` also PASS on intel2. The complete Linux
`test_qualify_driver.py` regression then PASSes on committed `b8f232f40`; its
closed log is `taskP-20261027-driver-fix/full-driver-regression.log`.
The full code gate and corrected live matrices remain queued.

### Frozen corrected follow-up

Exact source `b8f232f4023692a3d5161d12ed7e979bf66b29a9`, based on devel
`e7e72e267`. Archive SHA256:
`6c220c88df8e723da40a2abf7cea2e7cd9aeffac43c34fda09ef1d0cf40e98fd`.
Private source is `intel2:~/efs-taskP-current-b8f232f40/src`; evidence is
`~/efs-artifacts/taskP-20261027-current-b8f232f40/`. Detached screen
`taskP-current-b8f232` waits for the original controller's FINISHED marker,
then refuses to start if old mounts, `efsd`, make or gmake remain active.
Its wait is bounded at 12 hours; a stopped or leaking original run blocks the
follow-up with a retained error instead of killing anything or overlapping.

The launcher then executes the committed controller:

```sh
cd "$HOME/efs-taskP-current-b8f232f40/src"
python3 scripts/task_p_rehearsal.py \
  b8f232f4023692a3d5161d12ed7e979bf66b29a9 \
  "$HOME/efs-artifacts/taskP-20261027-current-b8f232f40"
```

That controller builds and checks the frozen source, runs both full selected
matrices serially, verifies their manifests and runs the eight-job code gate.
Unique source/work/output roots keep original evidence intact. Build identity,
full per-gate results, both final F3 ledgers, Task A wall comparison, classified
failures and final hashes remain pending. No qualification PASS or task
completion is claimed. No product fixes were made by Task P.

At the continuation checkpoint nine three-member gates have closed: identity,
B-checks, D4 and four POSIX suites PASS; physical topology NOTRUN; soak NOTRUN
as D-P1 above. D4 took 1102.989 s. The partial Task A comparison flags no
doubled completed gate so far. Append failover is actively creating/verifying
records, not declared stalled. Five-member and both code gates remain queued.
These partial observations do not replace final ledgers or closed evidence.

Later checkpoint: append-failover PASS (747.245 s); the original three-member
matrix has eight PASS and two NOTRUN results and is running K14 torn-read.
The current-source queue is WAITING_FOR_ORIGINAL_CONTROLLER. No FAIL has
closed at this checkpoint; uncompleted gates are not credited as passes.
The partial comparison still has no doubled completed gate. These are
progress markers, not the final task verdict.
