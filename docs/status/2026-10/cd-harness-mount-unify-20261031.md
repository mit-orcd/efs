# Shared Mount for the last four tests; AO live gate stops its cluster, task CD — October 31, 2026

**PASS (harness work, no K row, blocks alpha: no).** Follow-ups 1 and 2 of [Task BY](by-harness-daemon-liveness-20261031.md).
Tested source: branch `claude/cd-harness-mount-unify-b753f9` on devel `8e53122cd`, commit `tests: fold four private Mount copies ...` (changed: `tests/k33_daemons.py`, `tests/test_k33_growth.py`, `tests/test_k33_client_view.py`, `tests/test_k22_leader_admission.py`, `tests/test_w130_dir_fault.py`, `scripts/qualify_ao_readdir.py`). Built on nuc (8 CPU, XFS, Linux x86_64, TCP loopback, buffered I/O) with `make -j8 all tests/faults/efsd`, build id `unknown` (tree copy via `git archive`). Base for comparison: `HEAD~1` built the same way.

## What each private copy did differently

| Test | Private copy | Folded how |
|---|---|---|
| `test_k33_growth.py`, `test_k22_leader_admission.py`, `test_w130_dir_fault.py` | Same code as the shared class minus registration and lazy unmount: module `env` (identical to `c.env`, since each passes it to `Cluster`), fixed 30 s mount wait and 30 s unmount wait, plain `-u` then `kill()`, not in `Cluster.mounts` | Deleted; import `Mount` from `k33_daemons`. Waits gain the sanitizer factor `SLOW` (1 on a normal build) |
| `test_k33_client_view.py` | Also had a private `Cluster` (no watchdog, no `stop_all` unmount, `--quota 2G`, 60 s `wait_log`), and `Mount.name`, `.log()`, `.wait_log()`, 60 s unmount wait | Private `Cluster` and `Mount` deleted; uses the shared `Cluster(work, 'cv', ...)` with `quota='2G'` and `timeout=60` on its three formation waits. The shared `Mount` gained `text()` and `wait_log(pattern, timeout=30)` (the only extension; `name` is `m.path.name`). Behaviour for the other 72 importers is unchanged |

Assertions and fault steps are untouched. All four are now watched and unmounted by `stop_all()` (lazy unmount after `fuser -m` if busy).

## Orphan mechanism in `test_ao_readdir_live.py`

The test runs `scripts/qualify_ao_readdir.py`, which already used the shared `Cluster`/`Mount` but tore down by hand: `client.sh stop`, then `assert`s on exit codes, then a loop over the daemons with `assert p.wait(...) == 0`. Any failure in that chain (mount stop, first daemon) skipped the rest, `stop_all()` was never called, and SIGTERM (a timeout) had no handler: the interpreter died without running any `finally`. SIGINT mid-run was already cleaned up. Fix: the innermost `finally` sends SIGCONT to survivors (node 3 is SIGSTOPped in the test) and calls `c.stop_all()`; `run()` turns SIGTERM into `sys.exit(143)` so it unwinds through the same `finally` as SIGINT.

Check (`sigint.sh`: start the test in the background on nuc, wait until the mount is up plus 4 s, signal the test process, wait 8 s, list leftovers under `efs-taskCD`):

| tree, signal | test rc | efsd | efs-fuse | `mount \| grep efs-taskCD/tmp` |
|---|---|---|---|---|
| base, SIGINT | 130 | none | none | none (already clean) |
| **base, SIGTERM** | 143 | **3 left** | **1 left** | **1 left** (killed and unmounted by PID by hand) |
| fixed, SIGINT | 130 | none | none | none |
| fixed, SIGTERM | 143 | none | none | none |

## Solo runs on nuc (fixed tree, wall seconds; budget)

growth 70, 63, 66 (225); client_view 14, 28, 19 (35); leader_admission 56, 59, 54 (90); w130_dir_fault 35, 27, 33 (50); ao_readdir_live 17, 28, 18 (30). All rc 0, all inside budget. Base solo for comparison: leader_admission 57 s, w130 28 s.

## Gate

`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`: `TOTAL 583 FAIL 0 NOTRUN 12 STATUS INCOMPLETE (1498s) OVERBUDGET 35 BUILD 17s`. No `fail-*.log`. NOTRUN are the RDMA and cluster-script tests of this host. OVERBUDGET is the known nuc load effect; of the changed tests under 8-job load: leader_admission 304.9 s (90), w130 109.2 s (50), ao_live 53.8 s (30), client_view 46.1 s (35), growth 90.9 s (225). leader_admission and w130 run at base speed solo (57 s, 28 s), so the slowness is gate load on a disk-bound host (the test fills a KV budget with writes), not the change; not re-measured under load on the base tree.

## Reproduce

Linux host with `/dev/fuse`, `fusermount3`, `fuser`; `make -j8 all tests/faults/efsd`, `TMPDIR` under your own directory; `python3 tests/<test>.py` for each of the five; SIGTERM check: run `python3 tests/test_ao_readdir_live.py &`, `kill -TERM` it once `mount` shows its `efs-ao-live-*` mount, then `pgrep -a efsd; pgrep -a efs-fuse` must be empty (a background shell ignores SIGINT: restore it with `signal.default_int_handler`). Gate: `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`.

## Raw location

`~/efs-artifacts/taskCD-20261031/` on nuc (summary, timings, gate log, solo and signal-check logs, scripts, `SHA256SUMS`); `sha256sum SHA256SUMS` = `c16db35629b2eca1a6961f1e11a49513a11ae0e09f9ae61e1870de6485bd458c`. Not committed.
