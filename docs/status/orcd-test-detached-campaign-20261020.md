# ORCD-test detached packing/RDMA/.efs campaign — 2026-10-20

**Verdict: RUNNING; qualification pending.** Build and detached-worker smoke
checks PASS. User authorized stopping the old fcstor007–013 mounts; all
seven stopped cleanly. Old daemons on fcstor003–006 stopped cleanly and their
disposable stores were removed. The controller is running in detached screen.

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
