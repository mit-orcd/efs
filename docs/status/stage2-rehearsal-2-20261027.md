# Task P — second Stage 2 rehearsal

`agent/devel-task-p`, base `24dad8e4b`: TOTAL 0 FAIL 0 NOTRUN 2 topology runs, pending nuc handoff; this is not a make-test result. No base comparison yet. Evidence: `docs/status/stage2-rehearsal-2-20261027.md`. Both loopback matrices and final code gate NOTRUN. No product finding yet.

## Scope and current state

**NOTRUN, awaiting the coordinator's nuc release.** Task P runs all selected
Stage 2 gates, serially for loopback-3 and loopback-5, then the eight-job
`make test-all` gate. The assigned work excludes product fixes: a product
failure gets a K row and retained trace. Driver, gate and environment defects
may be corrected. No real cluster, shared checkout, Task H/I fixture or
external repository is modified.

The base matrix validates: 26 items, nine topologies. Both requested
loopback topologies select all 24 named gates. Each also records its final
identity check. Raw results will be retained outside Git under
`nuc:/home/maik/efs-artifacts/taskP-20261027/`; no run has started and no
closed manifest is claimed yet.

A read-only host check found no running EFS daemon/client, but three Task H
mounts remain under `~/efs-taskH-repair-20261027/attempt-{37,40,43}`.
Task H's chat still describes a nuc resync investigation. Those mounts and
retained trees are not Task P's to clear. Task I moved later qualification
to xorinox and node9900; that alone is not a coordinator handoff of nuc.

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
  The spawned stub was checked absent. The complete test must run on nuc.

## Planned method and comparisons

Freeze the current source and record its full commit, archive hash, build
identity and tracked-source manifest. Build privately on released nuc with
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
| Candidate and profile | NOTRUN — host handoff | NOTRUN — host handoff |
| Placement/capacity and decode | NOTRUN | NOTRUN |
| POSIX and concurrent I/O | NOTRUN | NOTRUN |
| Packing/shared-fragment GC | NOTRUN | NOTRUN |
| Enabled `.efs` under load | NOTRUN | NOTRUN |
| Full payloads + orderly restart | NOTRUN | NOTRUN |
| Scale/progress evidence (E4, D4) | NOTRUN | NOTRUN |

After closure, verify driver manifests, retain exact methods/build identities
and full SHA256SUMS outside Git, then prune only inactive Task P bulk with
`edelete` and record PRUNED.txt. Real-host TCP/RDMA and physical-node release
acceptance remain outside this single-host rehearsal.
