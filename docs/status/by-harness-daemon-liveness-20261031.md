# Harness daemon liveness and mount teardown, task BY — October 31, 2026

**PASS (harness work, no K row, blocks alpha: no). (a) PASS, (b) PASS, (c) PASS, (d) PASS, (e) 0 FAIL, INCOMPLETE only for the 11 NOTRUNs of this host.**
Linked items: batch F rehearsal faults 9 and 11 ([page](batch-f-rehearsal-20261031.md)).
Tested source: branch `claude/by-harness-daemon-liveness-981e9f` on devel `bc2af2bf8`; changed files `tests/k33_daemons.py`, `tests/test_k22_snapshot_import.py`, `tests/test_k33_setup.py`. Base (negative control): devel `bc2af2bf8` unchanged. Both trees built on xorinox with `make -j6 all tests/faults/efsd` (native `-O3`, build id `unknown`: tree copies without `.git`).
Environment: xorinox (16 CPU, Linux x86_64, `/dev/fuse`, `fusermount3`), TCP loopback, ports from the kernel's ephemeral pool (never 17xxx/27xxx), buffered I/O, fault-build `efsd`, 1 GiB quota per store, TMPDIR under the task directory. Task BX ran beside it on the same host.

## The two mechanisms

- **Fault 9 (3604 s retry).** The daemons are started by `Cluster.start` in `tests/k33_daemons.py` and nothing looked at them afterwards except `wait_log`, which checked only the one node it waited on. Drivers that poll through `efs-mgmt` or the mounts (`scripts/d4_progress.py`, the bench and write loops of the K22/K33 tests) never asked whether a daemon was still alive: a dead member only made their reads and mgmt calls slow or failing, which they retried until the entry timeout. A child nobody `wait()`s for stays a zombie, so `/proc` showed it as present.
- **Fault 11 (dead mount left).** `tests/test_k22_snapshot_import.py` carried a private copy of `Mount` whose `close()` did `fusermount3 -u`, waited 30 s, then `kill()`ed `efs-fuse` with no lazy unmount. With writer threads still using the mount the `-u` fails busy, the kill follows, and the kernel keeps the mount of a dead FUSE server (`transport endpoint is not connected`). `os.path.ismount` is False for such a mount (ENOTCONN), so nothing ever noticed. (The shared `Mount` already had `-uz` in that branch; the private copy did not.)

## The fix (once, in `tests/k33_daemons.py`, used by 72 tests and scripts)

- `Cluster` starts a daemon watchdog thread at the first `start()`. Every 0.25 s it calls `Popen.poll()` (which also reaps zombies) on every efsd the cluster started and every `Mount` it made. A process that is dead on two consecutive ticks, still registered, not expected, is a failure: the watchdog prints `[harness] efsd node 2 died unexpectedly: exit signal SIGKILL; log <path>` (daemon name, exit status or signal, log path), and interrupts the main thread (`_thread.interrupt_main`). If the main thread is still running 15 s later (a blocked FUSE call) the watchdog unmounts, kills the cluster's processes, reports and `os._exit(1)`s. No sleep was added anywhere on the test's own thread.
- The wait loops that poll a cluster (`wait_log`, `status`, `wait_voters`, `wait_members`, `leader_placement`) call `check_alive()` inside their existing iterations and raise `AssertionError` with the same message.
- Expected deaths: a daemon leaves the watched set when the test pops it, which the tests already do (`stop()`, `wait_exit()`, `wait_any_exit()`, and the explicit `c.procs.pop(i)` of K33 autoid, K43, W153 formation). A daemon started with any `EFS_FAULT_*(EXIT|CRASH|DIE)` variable set (`EFS_FAULT_MEMBER_EXIT[_ONCE]`, `EFS_FAULT_CRASH`, `EFS_FAULT_PACK_CRASH[_NODE]`, `EFS_FAULT_COMPACT_DIE`: K33 growth/interrupt/join-retry/autoid/seed-loss/bookkeeping/client-view/payload, K35, K36 recovery and segment recovery, W153 formation) is expected automatically; `c.expect_exit(i)` is the explicit form for any other test. Exit status 0 is a clean stop (tests stop daemons with `efs-mgmt` and signals themselves); an `efs-fuse` that ended with no mount left was unmounted by its test (K42's fail-closed unmount exits 1). Only a signal or non-zero daemon exit, or an `efs-fuse` that died with its mount still in `/proc/mounts`, counts. `SIGSTOP`/`SIGCONT` tests (K22, K41, K33 seed loss) are not deaths. `tests/test_server_processes.py` does not use the helper (no change). `tests/test_k54_member2_unmount.py` runs `tests/live/k54_member_down_unmount.py`, which uses `Cluster` and `Mount` and stops its victim with `c.stop(victim, sig)` (pops it: expected); it passes in the final gate.
- Mounts: `Mount` registers itself in `Cluster.mounts` and `stop_all()` unmounts whatever is left. `close()` does `fusermount3 -u`; if that fails and the mount is still in `/proc/mounts` it runs `fuser -m`, lazy-unmounts (`-uz`) at once and waits only 5 s for `efs-fuse` (not 30 s) before killing it; a mount still present afterwards is lazy-unmounted too. Each such step is a note in `Cluster.notes`, printed as `[harness] cleanup: <path>: <why> (efs-fuse exit N, users [pids]): lazy unmount, now gone|MOUNTED`. A passing run with clean unmounts prints nothing.
- `tests/test_k22_snapshot_import.py` now uses the shared `Mount` (private copy deleted). `tests/test_k33_setup.py`'s stub cluster gained `mounts`/`notes` and asserts a failed mount constructor leaves nothing registered.

Not changed: `Mount` copies in `test_k33_growth.py`, `test_k33_client_view.py`, `test_k22_leader_admission.py`, `test_w130_dir_fault.py` (own `Popen`, `kill()` without lazy unmount, not registered with the cluster). They keep the old behaviour and are the remaining places for the same hygiene fix.

## Verdicts

- **(a) PASS** `python3 tests/test_d4_progress.py` on the fixed tree: PASS three times, 84 s, 84 s, 90 s wall; unfixed base 76 s, 87 s, 84 s (budget 195 s). The difference is inside run-to-run noise (the host was shared with Task BX); the checks cost no sleeps.
- **(b) PASS** `kill -9` of the node 2 `efsd` (a process under the task TMPDIR, verified by command line) 35 s into `test_d4_progress.py`: fixed tree FAIL 16 s after the kill with `[harness] efsd node 2 died unexpectedly: exit signal SIGKILL; log .../d/n2.log`, no process or mount left, and the lazy-unmount cleanup note for the busy `mnt-l`. Earlier iterations of the fix gave 21 s (watchdog fallback only, a 30 s `efs-fuse` wait in `close()`) and 12 s with two daemons left running (grace shorter than the teardown): both found and fixed before the final run. Negative control on the unfixed base: the test ran on for 194 s after the kill with n1 and n3 alive until the driver's own bounds ended it (rc 1); it never named the dead daemon. The 3604 s of the rehearsal needs the quorum gone (n1 and n3 dead); not reproduced, the mechanism is the same: nothing watches the daemons.
- **(c) PASS** `python3 tests/test_k22_snapshot_import.py` on the fixed tree: PASS, `mount | grep efs-taskBY` empty afterwards, no cleanup needed (clean `-u`). The busy-mount path was exercised by (b) (cleanup note above).
- **(d) PASS** `python3 tests/test_make_test_recipe.py`: 610 commands, 102 budget rows, budgeted gate 1750 s of 1750 s at 8 jobs.
- **(e) 0 FAIL** `TMPDIR=... EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`: `TOTAL 583 FAIL 0 NOTRUN 11 STATUS INCOMPLETE (1023 s) OVERBUDGET 3`. The 11 NOTRUN are the RDMA and cluster-script tests that cannot run on this host (no IB device, `/home/maik/git/cluster` absent); unchanged by this work. Two earlier gate runs of the work in progress failed 6 then 3 tests, every one a false positive of the first rules (a `efs-fuse` or `efsd` that exited 0 after the test stopped it, K42's deliberate exit-1 unmount, the K33 stub cluster) and led to the exit-status and mount rules above; OVERBUDGET counted 7, 3 and 3 in the three runs, status does not change with it.

## Findings (not fixed)

- `tests/test_ao_readdir_live.py` leaves its private efsd processes running when it is interrupted: four orphan `efsd` stayed after its failed gate run (killed by hand, only under the task directory). Own teardown, not the shared helper.
- Limits of the check: a daemon that hangs but stays alive (SIGSTOP, deadlock) is not detected; an `efs-fuse` or daemon that exits 0 by itself is read as a clean stop.

## Reproduce

Prerequisites: a Linux host with `/dev/fuse`, `fusermount3`, `fuser`; `make all tests/faults/efsd` in two trees (base = devel `bc2af2bf8`, fix = this branch).

```bash
# (a) positive
python3 tests/test_d4_progress.py
# (b) fault: from a second shell, ~35 s after start, kill -9 one efsd of this run
pkill -9 -f 'efs-d4-.*/d/n2'          # only under your own TMPDIR
# (c)
python3 tests/test_k22_snapshot_import.py; mount | grep "$TMPDIR"
# (d), (e)
python3 tests/test_make_test_recipe.py
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
```

Cleanup: remove the two trees and `TMPDIR` (`edelete --force --delete`).

## Raw location

`~/efs-artifacts/taskBY-20261031/` on xorinox (run scripts, per-run logs, gate summaries and timings, `SHA256SUMS`). `sha256sum SHA256SUMS` = `7810ab4cc64aa91a09ee686ef9dcc24af5ee000efe904d675e56a47d9d5435f0`. Not committed (project rules).
