# /tmp hygiene (Task DI)

**Verdict: PASS; code fixed and host leftovers removed** (Nov 5, 2026). Purpose: stop EFS runs from littering the shared `/tmp` on the ORCD gate hosts, then clear what earlier runs left. Not a K row; blocks alpha: no.

- Source: branch `claude/nice-haslett-d41299` cut from `devel` `104eb5e10` (wave 57 merge `86c500d0b` included). Code commit `cac172b6b`; test and docs follow-up `08397211e`. This tree is what was tested.
- Host test: fstor007 (`fstor007-mgmt`), tree `/tmp/efs-taskDI/src` shipped with `git archive`, private libfuse shim, `TMPDIR=/tmp/efs-taskDI-tmp/test`, `EFS_TEST_TARGET=test bash scripts/make_test_summary.sh`. The tree was removed afterwards.

## What leaked and why

On Nov 5, before any clean-up (`*-list-counts-before.txt`), `erbmi1` owned these entries directly under `/tmp`:

| Host | `efs-control-*.stop` | `.sock` | `.stop.release` / `.stop.teardown` | scratch directories | task trees (not touched) |
|---|---|---|---|---|---|
| fstor007 | 7,884 | 0 | 0 | 620 | 26 |
| node9900 | 5,855 | 6 | 1 / 1 | 276 | 30 |

- **Stop records (the live leak).** `efs_stop_path()` in `src/client/stop_control.h` hard-coded `/tmp/efs-control-<hash>.sock`, so a gate's per-case `TMPDIR` never applied. The `.stop` record next to it was written at the end of every efs-fuse run (`main.c`: `done` or `failed`) and was never removed. Only the `.sock` was unlinked. Each mount path hashes to its own name, and every test mount has a fresh path, so each mount left one file. Between the brief's count and this listing the totals grew by 616 on fstor007 (one wave 57 gate) and 637 on node9900 (two wave 57 gates); Tasks DG, DH and CW were also mounting on these hosts in that window. Phases on fstor007: 7,138 `done`, 549 `failed`, 139 `draining`, 39 `serving` (the last two are killed clients).
- **`.stop.release` / `.stop.teardown`.** No product code writes these. `tests/test_client_processes.py` creates them next to its fake client's stop record to sequence the test. Since the record lived in `/tmp`, so did they. The one pair on node9900 is from an interrupted run.
- **Scratch directories.** Mostly from Oct 18 to Oct 22, before K25 (`tests/test_tmpdir.h`, Oct 22) moved C test scratch under `TMPDIR`: `efs-sort-*`, `efs-gc-a/b-*`, `efs-meta-reap-cleanup-*`, `efs-query-*`, `efs-subtree-*`, `efs-session-admission-cold-*`, `efs-retained-audit-*`. The later ones are `efs-k41-*` (Python `tempfile`, kept by failed runs) and one `efs-ba-upload-a/b-*` pair from Nov 4 (`test_ba_upload_retire.c` uses `efs_test_mkdtemp`, which keeps scratch on a failed exit by design). That test was run without `TMPDIR`.
- **Remaining literals** before this change: efs-bench, bench-local and efs-fuse perf and strace defaults (`/tmp/efs-*-perf-<pid>`), three `tests/measure` smokes with raw `mkdtemp("/tmp/efs-...-XXXXXX")`, `dir='/tmp'` in two live Python helpers, a fixed socket in `posix_phase_runner.py`, and fixed scratch roots in seven shell scripts.

## What changed

- `include/efs/tmpdir.h`: `efs_tmpdir()` returns `$TMPDIR` when it is set and nonempty, else `/tmp`. `efs_tmp_path(buf, len, fmt, ...)` builds `<tmpdir>/<name>` without a doubled `/` and returns the full length. `tests/test_tmpdir.h` (`efs_test_path`) wraps it, so tests and products share one rule.
- Stop control: the endpoint and its `.stop` record are under `efs_tmpdir()`. If the endpoint does not fit in `sun_path`, efs-fuse refuses to mount instead of truncating. `efs_stop_exit(rc, why)` replaces the final record write. If a stop tool sent DRAIN or FORCE and did not RESUME, the `done`/`failed` record stays for that tool. Otherwise (an unmount or signal with nobody waiting) the record is removed and the exit status carries the verdict. `scripts/client_processes.py retire` removes the final record after reading it, and derives the endpoint from `TMPDIR` the same way efs-fuse does. A starting efs-fuse already replaces a stale record at its own path, and its existing probe removes a dead `.sock` there.
- Literals routed through the helper, keeping the family names: the perf and strace defaults, the three measure smokes (now `efs_test_mkdtemp`), the live Python helpers (`tempfile` with no `dir`), `posix_phase_runner.py`, and `${TMPDIR:-/tmp}` in `device_fio.sh`, `valgrind_leaks.sh`, `rdma_first_inode.sh`, `run_fs.sh` and five `scripts/evidence` A/B roots.
- Left on purpose: `/tmp/efs`, `/tmp/efs-mount` and `/tmp/efs-perf` in `tests/cluster.sh`, `roll_efsd.sh`, `preflight.sh`, `wipe_cluster.sh`, `clean_cluster.sh`, `fuse_client_remount.sh`, `raft_host_smoke.sh` and `w23_stalled_compactor.sh`. Those are orcd-test deployment roots on the live cluster, which is out of bounds. Also left: task-tree guards such as `[[ $src == /tmp/efs-k35-slow-*/src ]]`, the `orcd_qvote` and `orcd_raft_kv` run roots, the `EFS_FAULTS` trigger file `/tmp/efs/fault` (read by fault builds, and the fault scripts create it there), and usage comments.
- Guard: `tests/test_tmpdir_hygiene.py` now also fails on any `"/tmp/efs..."` literal or `"/tmp/...XXXXXX"` template in any `src/` or `tests/` C file, and on `dir='/tmp'` in any Python under `tests/` or `scripts/`. It is a source lint and never scans a host's `/tmp`. `tests/test_stop_control.c` adds the unit test: the record stays when a stop was requested, and the `.sock` and `.stop` are both gone after exit 0 and after exit 1 with no requester. `TMPDIR` with trailing slashes, empty, and unset all give the expected path, and an over-long `TMPDIR` is refused. `tests/test_client_processes.py` checks that retire removes the record and that the endpoint follows `TMPDIR`.

## Results

| Check | Result |
|---|---|
| `make test` on fstor007 at `08397211e` | `TOTAL 487 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (110s) BUILD 13s`. NOTRUN: `test_cluster_stop.py` (no `~/git/cluster`), `test_rdma_memcheck.py` (no valgrind headers), `test_qualify_rdma_live.py` (no RDMA). These are the same three as the wave 57 fstor007 gate. |
| First run at `cac172b6b` | 1 FAIL: `test_put_drain.py` source-grepped the old `efs_stop_status("done", ...)` lines. Fixed in `08397211e`. |
| Mount tests with a kept `TMPDIR` (`verify.sh`) | `test_put_drain`, `test_client_ready`, `test_stop_dirty_drain`, `test_k90_unmount_enospc` (clean and exit-1 unmounts), `test_fixture_interrupt`, `test_stop_progress` (requested stops, including stalled and failed revokes) and `test_client_processes` all PASS. `efs-control-*` files left in that `TMPDIR`: **0**. |
| `/tmp` records made during the `make test` run | None from this tree. The 20 `done` records that appeared then were all written by Task DG's `test_k38_swept_hint_live.py` arms on the devel binary (two mounts per run; their mtimes match the ends of DG runs 6 to 15). That is the old behaviour this change removes. |
| Budgets | Unchanged: `test-stop-control` 0.5 s, `test_put_drain` 0.2 s and `test_tmpdir_hygiene` 0.1 s, all against 30 s rows. |
| Local (macOS) | `test_tmpdir_hygiene.py` and `make test-stop-control`'s C test PASS. `test_client_stop.py` fails on macOS before and after the change (mocked Linux `client.sh` path), so it was judged on fstor007. |

## Host clean-up

`scripts/evidence/task-di/tmp_hygiene.sh` (`ssh <host> 'bash -s list' < tmp_hygiene.sh`, then `remove`) selects `find /tmp -maxdepth 1 -mindepth 1 -user erbmi1 -mmin +1440` in the families `efs-control-*.stop*`, `efs-control-*.sock`, `efs-gc-*`, `efs-sort-*`, `efs-query-*`, `efs-k41-*`, `efs-meta-reap-cleanup-*`, `efs-ba-upload-*`, `efs-subtree-*`, `efs-session-admission-cold-*` and `efs-retained-audit-*`. It keeps a `.sock` listed in `/proc/net/unix` and a `.stop` whose `pid=` is alive. In remove mode it refuses any path outside those families, deletes directories with `edelete --force --delete` and files with `rm -f`, and writes the after-counts. It never selects `efs-merge*`, `efs-task*`, `efs-mount` or anything under `/data1`. No `wave57` screen was running on either host.

Listed first (`*-list-*`). The user approved removal, and it ran at about 16:10 with `bash -s remove`, which recomputes candidates at run time (`*-remove-*`, `LEFT` count 0 on both hosts):

| Host | Before removal (`.stop` / `.sock` / `.stop.*` / scratch dirs) | Removed (files / dirs) | Kept as live | After (`.stop` / `.sock` / scratch dirs) |
|---|---|---|---|---|
| fstor007 | 7,962 / 0 / 0 / 619 | 7,107 `.stop` / 617 | 1 `.stop` (its pid is now a kworker; kept to be safe) | 855 / 0 / 0 |
| node9900 | 5,995 / 6 / 2 / 275 | 5,217 `.stop`, 5 dead `.sock`, 2 `.stop.*` / 274 | `efs-control-4d2bbb54cdde711f.sock` and `.stop` (live efs-fuse pid 3806523, since Nov 2) | 778 / 1 / 0 |

The `.stop` files left are the kept ones plus records younger than 24 h (from today's gates and from Tasks DG and DH on devel binaries); a later pass with the same script clears them once they age. The task trees (25 entries on fstor007 and 29 on node9900, plus the `efs-taskCW`, `efs-taskDG` and `efs-taskDH` trees) were not touched.

Old task trees awaiting the user (listed, untouched; `*-task-trees.tsv`):

| fstor007 tree | Size | mtime |
|---|---|---|
| `efs-taskCW`, `-base`, `-tmp`, `-fuse`, `-gate.sh`, `-tsan*.sh`, `-tsan2.res` | 445M, 68M, 1.1G, 0, 4K x4 | Nov 4 00:47 to 01:26 |
| `efs-taskT-20261028-*` (9 calibration and confirmation trees) | 73M to 685M, 4.5G in all | Oct 28 19:10 to Oct 29 08:45 |
| `efs-taskI-fstor007-b6-b994`, `-final-093`, `-next` | 11G, 11G, 403M | Oct 28 |
| `efs-taskH-causes-20261029` | 2.9G | Oct 29 04:37 |
| `efs-taskBE-tmp1`, `-tmp2` | 0, 0 | Oct 30 |
| `efs-taskBS-artifacts` | 5.0M | Oct 31 01:12 |
| `efs-task-af` | 0 | Oct 28 19:55 |

| node9900 tree | Size | mtime |
|---|---|---|
| `efs-taskI-node9900` and 15 `efs-taskI-node9900-*` runs | 31M to 19G, about 120G in all (largest: integrated-03 19G, trunc-773 17G, lsan-00c 16G, sparse-a08 14G, the base tree 14G) | Oct 27 14:48 to Oct 28 10:27 |
| `efs-taskI-probe-native-20261028`, `efs-taskI-sparse-check-20261028` | 12K, 376K | Oct 28 |
| `efs-taskBB-orcd-02f861e4a`, `-17027103e`, `-791d57787` | 502M, 379M, 379M | Oct 29 |
| `efs-fin-controls-38f`, `efs-fin-dev`, `efs-fin-dev-v2` | 84K, 116K, 52K | Oct 28 |
| `efs-k35-slow-1ab9`, `efs-k35-slow-fin38f` | 831M, 1.4G | Oct 28 |
| `efs-taskX-cleanup-final.log`, `.rc` | 4K, 4K | Oct 28 |

## Raw data and reproduction

Raw listings, logs and the make-test summary and timings are outside Git at `~/efs-artifacts/taskDI-20261105/` (NFS home, visible from both hosts; 28 files, `SHA256SUMS` sha256 `ccd6d5e399bfbb7583da7c09cd59ab6ed566bf8a14cb70cf347ebc78972309a3`).

```
python3 tests/test_tmpdir_hygiene.py
make test-stop-control                      # C unit test: path derivation and exit-time removal
EFS_TEST_TARGET=test TMPDIR=/tmp/efs-<name>-tmp bash scripts/make_test_summary.sh
ssh <host> 'bash -s list' < scripts/evidence/task-di/tmp_hygiene.sh    # then 'bash -s remove'
```

Limits: a client killed with SIGKILL still leaves its `.stop` (phase `serving` or `draining`), and only a later mount of the same path replaces it. Under a gate that file lands in the case's `TMPDIR`, which the runner deletes. A mount and the tools that reach its endpoint (`efs-fuse --stop/--resume`, `client.sh stop`, `client_processes.py`, efs-mgmt's SETTINGS page) must share `TMPDIR`; a mismatch fails closed with "no control endpoint". Long-lived operator mounts started with an exported `TMPDIR` must be stopped with the same value. The cluster tooling (`~/git/cluster`, `~/git/devops`) exports no `TMPDIR` when it starts or stops clients, so the orcd-mini client and its stop both use `/tmp`. `client.sh stop --force-discard` on a client whose endpoint refused (rc 3) still retires it without a DRAIN having reached it; such a client now removes its record at exit, so `retire` reports "exited without a final stop record" where it used to read `done`.
