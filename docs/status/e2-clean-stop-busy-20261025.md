# K42: clean unmount on a healthy cluster failed its revoke cleanup with BUSY (Oct 25)

**Purpose.** Find and fix Task A's candidate D
([Task A report](task-a-20261025.md), "Product findings for the coordinator")
for checklist item [E2](alpha-1.0-2plus1-checklist.md#e-full-verification-and-orderly-lifecycle)
(client stop): on a five-member loopback cluster with all servers live, after
POSIX core and the 68/68 two-client suite had passed, the *peer* client's clean
stop failed `revoke rc=-13 phase=cleanup shard=1677 touched=89` and exited 1,
with zero outstanding PUT tickets and no probe failure. A clean stop on a
healthy cluster must exit 0. A stop that really cannot finish must fail
closed and log the shard and the server's reason. K42, work item W164.

**Verdict: PASS (scoped: one host, loopback, TCP).** Cause found and
reproduced on the base: the peer's append cleanup met another session's
in-flight append on a shared file, and the server's refusal reached the
client as a bare BUSY, which it never retries. The first gate run found a
second instance in the lease phase: the cleanup batch was refused at apply
because the other client's close transaction held the inode's last-close
marker. The 20-run loops found two more in the append retirement: a refused
PREPARE whose own ABORT decide failed, and the barrier-retirement
transaction. Fixed: the server certifies every cleanup refusal that committed
nothing (append waits, and a lease batch that applied as a no-op BUSY) and
names its reason. The client repeats those within a 20 s per-shard budget
and otherwise fails with `shard=N reason=...`. **Final, on intel2 at `b9d5f9443`: 20 of
20 clean stops of a loaded peer; `make test-all` 499 commands, 0 FAIL, 14
NOTRUN host limits.** amd went offline before the final runs. Details are in
[Results](#results).

- **Branch:** `claude/great-curie-e4cfb1` on devel `1fc526f66`; not merged,
  not pushed. Code and tests: `e62cc1d07` (append waits), `0b81b7e8b`
  (lease batch), `d393e22a8` (refused PREPARE with a failed ABORT decide)
  and `b9d5f9443` (barrier retirement, stage logging; the tested head);
  `03b5eb42a` fixes a gcc 15 dead-store elision in
  `tests/test_sanitizer_gate.py`'s leak fixture (fails identically on the
  base on amd); this page and the status rows are in the commit after it.
- **Hosts.** Reproduction, diagnosis and the gates up to `d393e22a8` ran on
  amd. **amd went offline (no ping, no ssh) during the `b9d5f9443`
  validation loop and had not come back an hour later**, so the final gate
  and the 20-run loop at `b9d5f9443` ran on **intel2** (8 CPUs, Rocky Linux
  10.2, kernel 6.12.0-211, gcc 14.3.1, Python 3.12.15, `/home` on LVM over one
  NVMe, no RDMA device, no desktop volume monitor). The parent session chose
  the move; the user's cluster on intel2 (17xxx ports, `efs` user) was not
  touched.
- **amd:** AMD Ryzen AI MAX+ 395, 32 CPUs, 119 GB, Ubuntu 26.04.1,
  kernel 7.0.0-38, gcc 15.2.0, Python 3.14.4, `/home` ext4 on one NVMe). Trees
  under `~/e2busy/` (user maik), copied with `git archive` or a tar of the
  working tree (no `.git`), built with `make` and `make tests/faults/efsd`.
  TCP, `O_DIRECT` fragments, `tests/faults/efsd` daemons with 1 G quota.
  Reproduction loops use private loopback ports 18501–18505 (discovery
  18599); the tests use free loopback ports. Nothing under `/data1/efs`, no
  `efs` user, no 17xxx/27xxx port was touched.
- **Raw outputs:** amd and intel2 `~/efs-artifacts/e2-clean-stop-20261025/`
  (see [Raw evidence](#raw-evidence)).

## Task A's run

Task A's raw run is on amd,
`~/efs-artifacts/task-a-20261025/gc-followup5/gates/gc-reclamation/harness/p1/gc-direct-2ss0nrdn/`
(the path in the request named a Mac copy that does not exist). The peer's
log ends:

```
16:23:41.669Z efs: put tickets outstanding at unmount: 0 (settle rc=0)
16:23:42.483Z efs: session revoked at unmount: epoch=1 touched=89 rc=-13 ms=814 fence_ms=29 cleanup_ms=703 cleaned=25 probe_failed=0 phase=cleanup
16:23:42.485Z efs-fuse: unmount did not complete cleanly; exit 1
```

All four daemons applied the peer's fence entries (`applied session ...
kind=5 shard=1677` on n1–n3 at 16:23:41.7). No leader change, ReadIndex
refusal or applied-wait BUSY was logged around the stop. The base build logs
nothing on the server side of a cleanup refusal, so the run cannot show which
step refused. The step failed 703 ms into cleanup, well inside its 30 s
budget, so it was a single definite answer and not a timeout.

## Reproduction on the base

`tests/live/e2_peer_stop_loop.py --until-fail --suite` (committed), on base
`1fc526f66` plus temporary stderr diagnostics (a `DIAG` line where
`host_session_append_step`, `efs_meta_retire_append_session` and
`efs_session_cleanup_bounded` return an error; not committed). Five members,
the primary efs-fuse on node 1, a peer on node 2. Both clients append
`O_APPEND` records to four shared files and write and unlink private files.
The 68-case `tests/posix/posix_2client.py` runs on both mounts, the peer's
load stops, and `scripts/client.sh stop` stops the peer while the primary
keeps appending. **It failed on the first run** (posix2 68/68 PASS):

```
run=1 node=2 stop=1 exit=1 touched=91 revoke_rc=-13 revoke_ms=1177 cleaned=48 ok=0
DIAG retire: shard=1612 ino=17996 open=1 busy
DIAG append_step: shard=1612 rc=-13 salt=0
DIAG cleanup shard=1612 appends=1 rc=-13 salt=0 idx=0 term=0 hint=3
efs: session revoked at unmount: epoch=1 touched=91 rc=-13 ms=1177 fence_ms=32 cleanup_ms=1063 cleaned=48 probe_failed=0 phase=cleanup
```

(A first attempt stopped on the load script's own error, an `EBUSY` from
unlinking a private file still being written back. That is not this subject;
the script now tolerates it.)

## Cause

Who answered, and to which command:

1. The peer's revoke reached cleanup. Its probe left shard 1612 pending, and
   the lease phase (`LEASE_LOCK_NEXT`) of that shard was already empty. The
   append phase sent `EFS_MD_REQ_SESSION_APPEND_NEXT` to a node hosting both
   groups.
2. `host_session_append_step` found no OPEN reservation of the revoked
   session. It called `efs_meta_retire_append_session`, which selected the
   session's append **receipt** for inode 17996, one of the shared files.
3. Before capturing a retirement, `efs_meta_retire_append_session` checks
   the inode's append cursor. The cursor had one OPEN reservation (`open=1`),
   the **primary's** append in flight. Foreign OPEN work must never be
   discarded, so it returned `EFS_ERR_BUSY`. **Nothing was proposed.**
4. `host_session_append_step` passed that BUSY on with salt 0. Only
   admission, authority and selection refusals carried
   `EFS_MD_SESSION_APPEND_RETRY_SALT`. A refusal from the retirement call
   carried none, even though it came before any transaction. The same held
   for the step's other pre-proposal refusals: a DONE row past the published
   size (frontier), a PREPARE whose ABORT was decided, and a NOT_FOUND inode
   mapped to BUSY because REAP still owes its rows.
5. `efs_session_cleanup_bounded` repeats a BUSY only with that certificate
   (zero index and term). It treated this BUSY as the step's one result, so
   the revoke stopped after one RPC and the client exited 1.

So the refusal was not the W159 applied-wait, a leader change, a cold
ReadIndex or a lease lock. It was the append-cleanup step waiting, correctly,
on another client's OPEN append. That wait resolves within seconds: the
writer's REPORT completes the reservation. In the K42 test, with that REPORT
held, the writer gave up after its 8 s REPORT bound and 8 s identity lookup
(`EIO` on close), and the OPEN was gone about half a second later. The bug was that the wait reached the client
as a final verdict. Task A's run fits the same pattern: shard 1677 failed
fast (703 ms), with healthy members and the primary still mounted after the
two-client suite. Its base logs cannot name the step.

The client's revoke budget did retry BUSY, but only certified BUSY:
`LEASE_LOCK_NEXT` selection refusals (salt 2) and `SESSION_APPEND_NEXT`
admission, authority and selection refusals (salt 3).

**Second interleaving (lease phase), found by the first gate run.**
In `tests/test_k42_peer_stop_open.py` case 2 (gate at `e62cc1d07`, 8 jobs),
the peer's revoke failed in 1 ms of cleanup with
`... phase=cleanup shard=1 reason=uncertified-busy`. It never reached the
append phase: the fault hook's one-time line for the append step was absent
from all three daemon logs. The lease phase answers a selection refusal with
the certificate, so the uncertified BUSY was the verdict of its proposed
`LEASE_LOCK_BATCH`. `efs_session_cleanup_apply` refuses with BUSY, before
any write, when a key it deletes or creates is under another transaction's
exclusive intent. One of those keys is the inode's last-close marker, which
the batch creates for a retained lease. At that moment the primary was
appending to and closing the same file, and its close-step transaction holds
that marker. The verdict is definite (the entry applied and changed
nothing), but the generic proposal reply carries the entry's index, so the
client treated it as uncertain. `tests/test_session.c` now pins the
marker-intent refusal as a no-op. 16 more loaded copies of the test did not
hit the window again, and the base build logs nothing for this verdict, so
the attribution rests on the logs above and that code path. Those logs were
deleted with the gate's work tree before they were copied to the artifacts
folder; the lines are quoted here.

**Third case (append phase), found by the first 20-run loop at
`03b5eb42a`.** Runs 3 and 7 of that loop (peer on node 4 both times) failed
in 165–182 ms of cleanup with `phase=cleanup shard=603
reason=uncertified-busy`. Within milliseconds the group's other members
logged `raft-host: txn-finish coord=603 abort decided rc=0 (coordinator lost
its group)`. The retirement's PREPARE had been refused (a concurrent append
held a lock), and the coordinator's own DECIDE ABORT then failed too.
`finish_execute` returned that decide error, which carries no certificate,
and the server's ABORT finisher decided the abort later. COMMIT was never
proposed, so the step changed nothing. That finisher path is common under
this load: n2 and n3 logged it 602 and 495 times over the partial loop,
mostly for the primary's own appends to the shared files. `d393e22a8`
records the refused PREPARE's rc in the capture and certifies a BUSY or STALE
refusal (`WAIT_ABORTED`) whatever the ABORT decide answered. The server now
also logs any cleanup BUSY it still sends without a certificate, naming the
step, shard and inode.

**Fourth case (append phase, barrier retirement), found by the 20-run loop
at `d393e22a8`.** Runs 9, 11 and 12 of 12 failed on shard 2171 (one of the
shared files), and the new server line named the retirement path:
`session append cleanup shard=2171 epoch=1 ino=0 BUSY uncertified`. The
failures came more often as the shared files grew. When a REPORT has
finished its inode rows but still owes the lane retirement (watermark set,
no OPEN), `efs_meta_retire_append_session` retires the barriers itself
through `efs_meta_retire_append_barriers`. That transaction's refusals (a
reservation opened after the state read, or a refused PREPARE) came back
without a reason. `b9d5f9443` adds a private `retire_barriers()` variant
that names them for the session cleanup only; the public function keeps its
exact results for its other callers (close step, abandon, catch-up). A
thread-local `efs_meta_append_cleanup_why` names the stage in the server's
uncertified line. A validation loop at `b9d5f9443` (working tree) logged no
uncertified BUSY in 9 of 9 clean runs, then amd went offline.

## Fix

- **Meta** (`src/meta/append_plan.c`, `include/efs/meta_apply.h`):
  `efs_meta_retire_append_session` and `efs_meta_finish_append_fenced` take
  `unsigned *wait` and name why a BUSY step committed nothing:
  `EFS_META_APPEND_WAIT_OPEN` (another session's OPEN reservation),
  `_FRONTIER` (a DONE row past the published size), `_ABORTED` (a PREPARE
  refused with BUSY or STALE, so COMMIT was never proposed; `finish_execute`
  records the refusal, and a failed ABORT decide is finished by the server)
  and `_GONE` (the inode is gone and REAP_MULTI owes its
  rows). A failed COMMIT acknowledgement names nothing.
- **Server** (`host_session_append_step`): a BUSY with a named wait is
  answered with `EFS_MD_SESSION_APPEND_WAIT_OPEN` … `_GONE`
  (`include/efs/meta_cmd.h`, salts 4–7; fresh format, no compatibility).
  An uncertain transaction keeps a bare BUSY.
- **Client** (`src/common/session_revoke.c`, `include/efs/session_revoke.h`):
  `efs_session_cleanup_retry_ok` accepts salt 2 for leases and salts 3–7 for
  appends, always with zero index and term. `efs_session_cleanup_bounded`
  repeats such a step every 50 ms within `EFS_SESSION_CLEANUP_BUDGET_MS`
  (20 s, down from 30 s, so a shard still refused fails inside the stop
  tool's 30 s no-progress window). A revoke that stops in cleanup reports the
  server's reason (`efs_session_cleanup_reason`). The unmount logs
  `... phase=cleanup shard=N reason=append-open-by-other-session` (or
  `append-frontier`, `append-prepare-aborted`, `append-reap-owed`,
  `lease-selection-refused`, `lease-key-locked`, `append-selection-refused`, `uncertified-busy`),
  and the stop record's reason carries it. `efs-mgmt raft-session fence`
  prints it too.
- **Lease batch** (`LEASE_LOCK_NEXT` route in `server_raft_host_submit`): the
  route proposes its batch locally and waits for the verdict. An applied
  BUSY of this exact entry (same index and term in the apply ring) is
  answered with zero index/term and `EFS_MD_SESSION_CLEANUP_WAIT_LOCKED`
  (salt 8, `reason=lease-key-locked`). The lease phase repeats it. A wait
  timeout, ring miss, other term or other verdict keeps the index, so the
  reply stays uncertain. A rate-limited server line logs each certified
  refusal.
- **Fault hook** (`EFS_FAULTS` builds only, `tests/faults/efsd`):
  `EFS_FAULT_APPEND_HOLD_FILE`. While the file holds an inode number, a
  REPORT does not resolve that inode's OPEN reservations. While it reads
  `cleanup`, every append cleanup step answers the OPEN wait. Inventory row
  in [A6](a6-settings-inventory-20261024.md); `env_inventory.py --check`
  PASS.

## Tests

- `tests/test_append_cleanup.c` `foreign_open_wait` (`make test`, real KV):
  a revoked session's receipt on an inode with another session's OPEN
  reservation gives BUSY with `WAIT_OPEN` and no progress, three times. A
  PREPARE refused by a lock gives `WAIT_ABORTED` with nothing applied. After
  the foreign completion, the same step retires everything and
  `efs_meta_append_session_idle` reports idle.
- `tests/test_session.c` (`make test`): a cleanup batch whose last-close
  marker is under a foreign intent applies as BUSY and deletes nothing.
  `tests/test_session_cleanup_cli.py` (actual `LEASE_LOCK_NEXT` route): an
  applied BUSY of the exact entry gets salt 8 with zero index/term; a ring
  miss, another term, or STALE/IO/NOT_PRIMARY/INVAL keep the index. The lease
  phase retries salt 8 and the append phase does not.
- `tests/faults/test_append_cleanup_retry.py` (actual
  `host_session_append_step`): each named wait certifies BUSY (and a
  REAP-owed NOT_FOUND) with salt 3 + wait. Any other error, and a
  transaction BUSY without a wait, stays uncertified.
- `tests/test_session_cleanup_cli.py` (actual `efs_session_cleanup_bounded`):
  the four append waits are retried until the step is empty. The lease phase
  does not retry them. A wait outlasting the 20 s budget fails with BUSY and
  keeps its reason. A wait salt on an indexed reply is not a certificate.
- `tests/test_k42_peer_stop_open.py` (`make test-long`, budgets row 90 s,
  one slot; three loopback daemons, two mounts):
  1. the primary's append to a file the peer appended to stays OPEN for 3 s
     after the peer's unmount begins (REPORT held by the hook). The peer
     waits and exits 0 (amd: revoke 3.7 s).
  2. the hook answers the OPEN wait past the budget. The peer exits 1 with
     `phase=cleanup shard=1 reason=append-open-by-other-session` and
     `unmount not clean ... revoke rc=-13`. After the hook ends,
     `efs-mgmt raft-session fence` completes the cleanup.
  **Negative control:** the same tree with the client's certificate limited
  to the old salts fails case 1 like Task A:
  `rc=-13 ms=158 cleanup_ms=52 ... phase=cleanup shard=1 reason=append-open-by-other-session`.
- Existing E2 and cleanup-probe tests stay green in the gate
  (`test_stop_progress.py`, `test_client_processes.py`, `test_put_drain.py`,
  `test_session_cleanup_cli.py`, `test_append_cleanup_retry.py`).

## Results

Loaded-peer loop (`tests/live/e2_peer_stop_loop.py`, five members, the
68-case two-client suite before every stop):

| Tree | Runs | Clean stops (exit 0, revoke rc=0) | Notes |
| --- | --- | --- | --- |
| base `1fc526f66` + diagnostics | 1 | 0 | failed on run 1: shard 1612, foreign OPEN (above) |
| fix `e62cc1d07` (working tree) | 10 | 10 | revoke 139–833 ms, touched 90–94, cleaned 47–49 |
| `03b5eb42a` (gate tree) | 7 (stopped) | 5 | runs 3 and 7 failed: shard 603, refused PREPARE with a failed ABORT decide (third case, fixed in `d393e22a8`) |
| `d393e22a8` (gate tree, amd) | 12 (stopped) | 9 | runs 9, 11 and 12 failed: shard 2171, barrier retirement (fourth case, fixed in `b9d5f9443`) |
| `b9d5f9443` (working tree, amd) | 9 (amd went offline) | 9 | no uncertified BUSY logged |
| **`b9d5f9443` (gate tree, intel2)** | **20** | **20** | revoke 949–1,848 ms, touched 90–94, cleaned 46–49; `PASS 20/20` |

`tests/test_k42_peer_stop_open.py`: PASS in the gates at `0b81b7e8b`,
`03b5eb42a`, `d393e22a8` (34 s, amd) and `b9d5f9443` (35 s, intel2); 3 of 3 alone and 24 of 24 as eight parallel copies at
`0b81b7e8b` (working tree). It failed once in the first gate at `e62cc1d07`
(the lease-phase interleaving above, fixed in `0b81b7e8b`).

Code gate (`scripts/make_test_summary.sh`, `EFS_TEST_JOBS=8`,
`make test-all`):

**Final gate, intel2, `b9d5f9443`: TOTAL 499, FAIL 0, NOTRUN 14, STATUS
INCOMPLETE (619 s), build 30 s.** All 14 NOTRUNs are host limits: no RDMA
device (8 RDMA unit commands and the declaration gate's RDMA path), no
valgrind (`test_leak_gate.py`, `test_rdma_memcheck.py`), no sanitizer
runtime, no cluster scripts checkout, and `EFS_FAULTS` not set for the
stalled-compactor case. `test_create_retry_fault.py` and
`test_stop_progress.py`, which FAIL on amd, pass on intel2.

Earlier gates on amd:

| Source | Settings | TOTAL | FAIL | NOTRUN | Seconds |
| --- | --- | --- | --- | --- | --- |
| `e62cc1d07` | default transport | 499 | 12 | 4 | 553 |
| `0b81b7e8b` | default transport | 499 | 11 | 4 | 552 |
| `03b5eb42a` | `EFS_TRANSPORT=tcp` | 499 | 2 | 14 | 587 |
| `03b5eb42a` (rerun) | `EFS_TRANSPORT=tcp` | 499 | 2 | 14 | 481 |
| `d393e22a8` | `EFS_TRANSPORT=tcp` | 499 | 1 | 14 | 589 |
| base `1fc526f66` | `EFS_TRANSPORT=tcp` | 498 | 3 | 13 | 502 |

On amd the last code gate (`d393e22a8`) had 1 FAIL, pre-existing and
identical on the base (a load timeout), and 14 NOTRUN host limits. The
`03b5eb42a` runs also failed `test_stop_progress.py` (host interference,
below). The amd-only conditions:

- `tests/faults/test_create_retry_fault.py`: its fault binary runs 13 s alone
  (3 of 3 PASS alone on the branch) and exceeds the test's 30 s subprocess
  timeout when it starts beside the gate's heaviest commands. It FAILs in
  every 8-job amd run (6 of 6, the base included). Follow-up, not this
  change.
- `tests/test_stop_progress.py`: the desktop's `gvfs-udisks2-volume-monitor`
  (pid 3641913) opens new FUSE mounts under `$HOME`, and the stop refuses
  with `mnt-b is in use by: gvfs-udisks2-vo`. It fails the same way on the
  base, alone as well. With a private TMPDIR under `/tmp` it passes on the
  branch and the base (27.3 s).
- The default-transport runs also FAIL the eight RDMA unit tests
  (`test_rdma_xprt`, `test_rdma_segmented`, `test_declaration_gate`'s RDMA
  path). amd's only verbs device is soft-RoCE `rxe0` on the WiFi netdev,
  which cannot accept a loopback connection. The base fails them
  identically; `EFS_TRANSPORT=tcp` turns them into NOTRUN.
- `tests/test_sanitizer_gate.py` failed on the branch and the base until
  `03b5eb42a`: gcc 15 at `-O1` removed the leak fixture's allocation. It
  passes alone after the fix and reports NOTRUN (runtime missing) inside
  the 8-job gate.
- The other NOTRUNs: no cluster scripts checkout, `EFS_FAULTS` not set for
  the stalled-compactor case, no valgrind, TMPDIR not XFS.
- OVERBUDGET (status unchanged): `test_k34_stop_put.py` 65 s of 55, and
  `test_k33_interrupt.py` 91 s of 70 in the rerun.

## Reproduction

```bash
# On a Linux build host, from a copy of the tree (no .git needed):
make -j"$(nproc)" && make tests/faults/efsd
# Unit and fault tests touched by the fix:
make tests/test_append_cleanup && ./tests/test_append_cleanup
python3 tests/faults/test_append_cleanup_retry.py
python3 tests/test_session_cleanup_cli.py
# Deterministic live test (about 60 s, needs fusermount3 and /dev/fuse):
python3 tests/test_k42_peer_stop_open.py
# The loaded loop: five members on 127.0.0.1:18501-18505, discovery 18599
# (--port-base moves them), 20 clean stops of a loaded peer; --suite also
# runs the 68-case two-client POSIX suite before each stop (about 2.5 min a
# run); --until-fail stops at the first failure and keeps the logs.
TMPDIR=$HOME/tmp python3 tests/live/e2_peer_stop_loop.py --runs 20 --suite
# Full code gate (screen, SCREENDIR=$HOME/.screen):
EFS_TEST_JOBS=8 scripts/make_test_summary.sh
```

On a desktop host whose volume monitor opens new mounts under `$HOME` (amd's
`gvfs-udisks2`), point TMPDIR elsewhere: such a monitor can hold the peer's
mount at unmount and fail the stop (amd, one validation run). The loop
deletes its work directory when every run passes. A failing run
keeps the daemon and client logs and prints the path. Remove kept trees with
`~/bin/edelete --force --delete <path>` (one path per call).

## Raw evidence

**intel2** `~/efs-artifacts/e2-clean-stop-20261025/` (`SHA256SUMS` covers
every file; its own sha256 is in the commit message of the commit that adds
this page): `gate-intel2-b9d5f9443/` (`summary.txt`, `timings.tsv`,
`build.log`, runner output) and `loop20-intel2-b9d5f9443.log` (the 20-run
tally).

**amd** `~/efs-artifacts/e2-clean-stop-20261025/`. **No `SHA256SUMS` yet:**
amd went offline before it was written. Write it with
`cd ~/efs-artifacts/e2-clean-stop-20261025 && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS`
when amd is back. The verdict rests on the intel2 runs, not on these files.

- `repro-base-1fc526f66/`: the base reproduction: daemon logs `n1.log` …
  `n5.log`, `primary.log`, `peer001.log` (the failing peer), the posix2 and
  stop outputs, `repro2.log` and the build log.
- `gate2-0b81b7e8b/`, `gate3-03b5eb42a/`, `gate4-03b5eb42a/`,
  `gate5-d393e22a8/` (final), `base-gate-1fc526f66/`: `summary.txt`, `timings.tsv`, `build.log`, the
  failing commands' logs and the runner output. `gate3-03b5eb42a/` also holds
  `alone-reruns.txt` and `stop-progress-tmpdir-tmp.txt`.
- `gate1-e62cc1d07.out`: only the runner's exit line. That gate's summary
  (quoted in the table) and its kept `test_k42_peer_stop_open.py` work tree
  were deleted with the gate tree before they were copied.
- `loop20-03b5eb42a-partial/`: the stopped loop's tally and the daemon,
  primary and failing-peer logs of runs 3 and 7 (the third case).
- `loop20-d393e22a8-partial/`: the stopped loop's tally and the logs of its
  failing runs 9, 11 and 12 (the fourth case).
- `val-gvfs-busy/`: the validation run whose stop failed because the mount
  was still busy at unmount (host interference).
- The `b9d5f9443` validation loop's tally (9 of 9) was in `~/e2busy/val2.log`
  on amd and in a `/tmp` work directory; it was not copied before amd went
  offline. It is quoted here only.

Task A's raw run stays at amd
`~/efs-artifacts/task-a-20261025/gc-followup5/` (its own `SHA256SUMS`).

## Limitations and remaining gates

- Loopback, TCP, one host per run (amd, then intel2 after amd went offline). The qualification topology rerun (Task A's
  `gc-reclamation` on `loopback-5`) and multi-host or RDMA stops are
  NOTRUN; the parent session runs live gates.
- Task A's own failure is attributed by its pattern, not by a server log: the
  base build logged nothing at the refusing step. It is the only
  pre-proposal BUSY the client did not retry, and a healthy cluster with a
  second client mounted reproduces it on the first run.
- A foreign OPEN that stands longer than 20 s (a writer stalled beyond its own
  REPORT bound) still fails the stop. It now fails closed, with the shard
  and `reason=append-open-by-other-session`, and `efs-mgmt raft-session
  fence` resumes the cleanup.
- Separate observation, not fixed here: `unlink` of a private file still
  being written back returned `EBUSY` once under the loop's load.
