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
