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
