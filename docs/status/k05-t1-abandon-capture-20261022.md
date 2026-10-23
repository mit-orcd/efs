# K5/K15 — integration review finding T1: append-abandon owner capture (Oct 22, 2026)

Branch `devel-k05t1`, based on devel `163241584`; **not merged**. Linked
items: Alpha 1.0 Stage 1 rows K5 ([W134](../backlog/work-items.md#w134--unregistered-session-identities-bypass-session-fencing) with W72) and K15 (W64), reopened by
[T1](devel-integration-review-20261022.md#t1--p1-k15-introduces-the-k5-capture-race-in-append-abandon).
By user direction this sub-session ends at the fix, regression tests and
`make test` on one host; no cluster, POSIX suite, load or A/B run.

| Item | Verdict |
| --- | --- |
| T1 (P1) append-abandon uses an unvalidated session row as the owner CAS image | **FIXED** on `devel-k05t1`, regression test PASS, negative control FAIL as expected |
| `tests/test_append_cleanup` (K15 decide-once rules) | **PASS** |
| `make test` on amd | **PASS with three host exclusions** (rc 0; three lines run with `-`, each shown below to fail identically on unmodified devel or to be host-caused) |
| Fence cluster gate (`scripts/k05_session_fence_gate.sh`) | **NOTRUN** (runs in another session) |
| RDMA paths of `make test` | **NOTRUN** (`EFS_TRANSPORT=tcp`, see Environment) |

## Finding

`efs_meta_abandon_append` (`src/meta/append_plan.c`) checked the session
with `efs_session_accept`, then read the local session row again with a
plain `efs_kv_get` and checked only its length before using those bytes as
the owner's EXCL PREPARE image. A FENCE_LOC landing between the two reads
left a row the PREPARE still matched, so a fenced epoch could retire its own
receipt (STALE) and resolve its OPEN reservation as ABORTED_HOLE after
fencing. This is the S2 shape that `468513a96` fixed for mtime, reservation,
completion and barrier retirement; K15's abandon path (`a51832413`) came
later and kept the two-read pattern.

## Change

Commit `ca850951d` (code and test). The abandon path captures the owner
image with `session_owner_capture`, which authorizes from the captured bytes
with the `efs_session_accept` rules (missing row or another established
epoch: UNREGISTERED; epoch below the floor or below established: FENCED).
The captured image stays the EXCL compare base. A fenced or unregistered
owner is refused before any prepare and is left for session cleanup
(FENCED_HOLE), as before. `host_append_abandon` (`src/server/raft_host.c`)
needed no change: like the reserve path it passes FENCED and UNREGISTERED
through to `rc_to_inode_status` and maps only STALE (exact-image conflict)
to the retryable BUSY.

## Tests

`tests/test_session_owned_capture.c` (in `make test`; real session rows in a
memory KV, a barrier KV that injects on the Nth read of the session row)
gains the abandon entry point. Each case first grants a reservation whose
reply is lost (applied receipt, OPEN reservation), then abandons it:

- Fence between accept and capture: FENCED, `retired=0`, no prepare; the
  fingerprint of inode, lane, intent, guard, append cursor, reservation,
  receipt and session rows equals the post-fence state; one OPEN
  reservation remains.
- Row removed between them: UNREGISTERED, same checks.
- Negative control (same case): the pre-fix capture (plain read, length
  check) accepts the fenced row, and an owner PREPARE against those bytes
  succeeds, so the EXCL CAS alone cannot refuse it.
- Fence after capture, before the owner PREPARE: the exact-image CAS
  conflicts (STALE, which the host returns as BUSY), only the owner prepare
  ran, state unchanged, reservation still OPEN; the retry is FENCED with no
  prepare.
- Fence after the owner PREPARE: the fence is BUSY behind the intent, the
  abandon commits (`retired=1`, no OPEN reservation), then the fence lands
  and a repeat abandon of that identity is FENCED with no prepare and no
  state change.

The mtime and append prepare spies share one fence hook (`spy_before`,
`spy_after`); the earlier cases are unchanged.

Pre-fix check: devel `163241584` with only the new test file fails at the
first abandon case: `abandon between mode=1 rc=0 want -27` (the fenced
owner's abandon succeeded), assertion at `test_session_owned_capture.c:313`.
With the fix it passes.

## Environment and build

- Host **amd** (`amd-395`), 32 CPUs, Ubuntu, Linux 7.0.0-38-generic x86_64,
  gcc 15.2.0; ext4 root filesystem. The host also runs the user's live EFS
  cluster (efsd on 17432–17434, `efs-fuse` on `/data1/efs/mnt`); none of it
  was touched, no daemon was started, no port bound.
- Tree: `git archive ca850951d` extracted to `~/efs-k05t1/src`; work and
  `TMPDIR` under `~/efs-k05t1/`, removed afterwards.
- Build: `make -j32`, rc 0, no `make: ... Error` lines. sha256:
  `efsd` `6a95bc9d84302cfa03dfc6cc86fdfad8a8bacd59b7b13ee9f29fac2419610e92`,
  `efs-fuse` `bc648518fbf102b428f0ef1d04226a0e9420e32a323a8d25d802b4536a862e2a`,
  `tests/test_session_owned_capture` `0d12ca8f583bbf9b1256db49189984d3783a385034eedd3f681a7b605c633c2e`,
  `tests/test_append_cleanup` `24d5f77bf02e8299d43d64162028ac50de28eada02f5aa491839c65ce3fe0e83`.

## Commands

```bash
git archive ca850951d | ssh amd 'mkdir -p ~/efs-k05t1/src ~/efs-k05t1/tmp && tar -x -C ~/efs-k05t1/src'
ssh amd 'cd ~/efs-k05t1/src && make -j32'
ssh amd 'cd ~/efs-k05t1/src && ./tests/test_session_owned_capture && ./tests/test_append_cleanup'
# Host-only Makefile edits (not committed), then the suite:
ssh amd 'cd ~/efs-k05t1/src && sed -i -e "s|^\tpython3 tests/test_client_processes.py$|\t-&|" -e "s|^\tpython3 tests/test_direct_read_fork_oracle.py$|\t-&|" -e "s|^\t\./tests/test_reclaim$|\t-&|" Makefile'
ssh amd 'cd ~/efs-k05t1/src && EFS_TRANSPORT=tcp TMPDIR=$HOME/efs-k05t1/tmp make test > ../test.log 2>&1; echo rc=$?; grep -nE "^make(\[[0-9]+\])?: .*Error" ../test.log'
# Pre-fix control: devel 163241584 plus the new test file only
git archive 163241584 | ssh amd 'mkdir -p ~/efs-k05t1/prefix && tar -x -C ~/efs-k05t1/prefix'
git show ca850951d:tests/test_session_owned_capture.c | ssh amd 'cat > ~/efs-k05t1/prefix/tests/test_session_owned_capture.c'
ssh amd 'cd ~/efs-k05t1/prefix && make tests/test_session_owned_capture && ./tests/test_session_owned_capture'   # expect abort
ssh amd 'rm -rf ~/efs-k05t1'
```

## Results

- Targeted: `test_session_owned_capture` PASS, `test_append_cleanup` PASS
  (lost reply retired by fence or abandon, frontier advances, late
  completion/replay refused, completion never overtaken, live owner
  untouched).
- `make test` (final run): rc 0, 139 s, recipe ran to its last line
  (`./tests/test_conn_fd`). Exactly three `make: ... Error 1 (ignored)`
  lines, the three host exclusions; no other `make: ... Error` line.
  NOTRUN lines: RDMA paths (transport forced to TCP), cluster stop/remount
  scripts (`/home/maik/git/cluster` absent), `test_stalled_compactor`
  (needs `EFS_FAULTS=1`), leak gate (valgrind absent).
- Host exclusions, none of which builds or runs `src/meta/append_plan.c`:
  1. `tests/test_client_processes.py`: `client discovery requires root:
     /proc/2580730`. That PID is the user's live `efs-fuse` (user `efs`,
     argv `./efs-fuse 127.0.0.1:17432 fs /data1/efs/mnt`);
     `scripts/client_processes.py` takes `fs` as a relative mount path and
     reads the process's `cwd`, which a non-root user cannot. Host-caused:
     the test needs root or a host without foreign `efs-fuse` processes
     (unprivileged user namespaces are disabled on amd).
  2. `tests/test_direct_read_fork_oracle.py` (builds only
     `tests/live/efs_direct_read_fork.c`): intermittent `FAIL files=6400`
     in the fork+COW case on both trees: 4/6 fixed, 2/6 devel `163241584`
     in isolation; 1/5 each in an earlier sample.
  3. `tests/test_reclaim`: intermittent `old meta/ copy fails closed:
     FAILED` (`server_reclaim_init` assertion at `test_reclaim.c:118`) on
     both trees: 1/3 fixed, 2/3 devel in isolation.
- Without `EFS_TRANSPORT=tcp`, `test_declaration_gate` and `test_rdma_xprt`
  fail at `server_accept` on amd's soft-RoCE device (`rxe0` over Wi-Fi);
  `test_declaration_gate` fails the same way on devel `163241584`.
- Raw logs stayed on amd under `~/efs-k05t1/` and were deleted.

## Limitations and remaining gates

- Not merged into devel; the parent session merges and updates tallies.
- NOTRUN: the fence cluster gate `scripts/k05_session_fence_gate.sh` (runs
  in another session), the K15 live lost-reply gate
  (`append_failover.py --lose-reserve-every`), the RDMA paths.
- The amd host exclusions above are pre-existing and unrelated to T1, but
  `make test` is not clean on amd as checked out: items 2 and 3 are
  intermittent on devel, and item 1 fails on any host running a foreign
  `efs-fuse` with a relative-looking argv field.
