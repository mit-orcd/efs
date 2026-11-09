# Stop-file hygiene (Task EP)

**Verdict: PASS; no new code change needed** (Nov 9, 2026). Purpose: stop the test harness from leaving `efs-control-*.stop` files in `/tmp` on fstor007. Not a K row; blocks alpha: no.

Task DI's change ([tmp hygiene](tmp-hygiene-20261105.md), commit `cac172b6b`, merged into devel in wave 58 on Nov 5 at 18:58) already makes the creator remove its file. The 2,858 files on fstor007 are all older: the newest is from Nov 5 at 19:57. A full `make test-all` and a plain `make test` on devel `33cd0a839` left **0** `efs-control-*` files. The user still has to decide what to do with the existing files; this task deleted none.

- Source: branch `claude/modest-matsumoto-1f2d02` on devel `33cd0a839` (wave 65). No source or test change; this page and the ledger paragraph are the only edits.
- Host: fstor007 (`fstor007-mgmt`, 96 CPUs). The tree went over as a `git archive` tar (sha256 `9a1985a8…f735`, the same on both ends) and was built under `/tmp/efs-taskEP`. The private libfuse came through the shim `/tmp/efs-taskEP-fuse`, with `TMPDIR=/tmp/efs-taskEP-tmp`. The trees, the tar and the scripts were removed afterwards.

## Who writes the file, who reads it, when it goes away

The harness does not write the file. **efs-fuse writes it**, and the Python helpers the brief named only call `.stop(...)` methods: `k33_daemons.py`, `d39_model.py`, `test_bench_published.py`, `test_b6_unavailable.py` and `test_b_geometry.py` match the grep through `c.stop_all()`, `self.stop(...)` and similar calls. None of them names a file. The file is a progress record ("stop record") for a mount, not a request:

| Step | Code |
|---|---|
| Path: the control endpoint `$TMPDIR/efs-control-<FNV-1a of the mount path>.sock` with `.sock` replaced by `.stop` | `src/client/stop_control.h:54-92` (`efs_stop_path`), `:308-310` (`efs_stop_start`); Python twin `scripts/client_processes.py:63-68` |
| Writer: efs-fuse, at mount (`serving`) and in every drain phase, as one line replaced by rename | `src/client/stop_control.h:99-130` (`efs_stop_status`), `:313` |
| A DRAIN or FORCE from a stop tool sets `requested`, and RESUME clears it | `src/client/stop_control.h:263-270` |
| At exit efs-fuse writes `done`/`failed` and keeps the file only when a stop tool sent DRAIN/FORCE and is waiting. Otherwise it unlinks the file | `src/client/stop_control.h:337-351` (`efs_stop_exit`), called at `src/client/efs_fuse/main.c:864` |
| Reader and remover for a requested stop: `retire` polls the record until the client exits, then unlinks the final record | `scripts/client_processes.py:105-149` (unlink at `:137-142`), run by `scripts/client.sh:337` after `efs-fuse --stop` (`:306-308`) |
| Every test that requests a stop goes through `client.sh stop` or `retire`: `test_stop_progress.py:80,95,110-123`, `test_bench_published.py:79`, `test_k48_readdir.py:78`, `tests/live/k54_member_down_unmount.py:60`. Every other mount ends with `fusermount3 -u` or a signal, so nobody requested a stop and efs-fuse removes the record itself | |

Before `cac172b6b`, the endpoint was always under `/tmp`, and efs-fuse wrote a final `done`/`failed` record at every exit and never removed it. Every test mount has a fresh path, so every mount left one file. That is the source of all the existing files.

One path still leaves a record: an efs-fuse killed with SIGKILL (or crashing) never reaches `efs_stop_exit`. For example, `tests/k33_daemons.py:453-455` kills a client that does not exit within 30 s after `fusermount3 -u`. Such a record (`serving`/`draining`) lands in the test's own `TMPDIR`, not in `/tmp`, and a later mount of the same path replaces it. No run below hit this path. Removing a record after SIGKILL would take a reaper outside efs-fuse; that is not done here.

## Existing files on fstor007 (for the user's decision)

Taken Nov 9 15:22 EST, before any run, from `/tmp/efs-control-*.stop` (`fstor007-stop-files-before.tsv` lists each path, its mtime and the record):

| mtime day | `done` | `failed` | `serving` | `draining` | total |
|---|---|---|---|---|---|
| Oct 28 | 1 | 0 | 0 | 0 | 1 |
| Nov 4 | 293 | 5 | 2 | 4 | 304 |
| Nov 5 | 2,490 | 50 | 11 | 2 | 2,553 |
| **all** | **2,784** | **55** | **13** | **6** | **2,858** |

- By age at 15:22 on Nov 9: 1 is older than 7 days, 744 are 4 to 5 days old, and 2,113 are 1 to 4 days old. None is younger than a day. The newest file is from Nov 5 at 19:57:44, and the oldest from Oct 28 at 20:29:23.
- Owner: all `erbmi1`. No `efs-control-*.sock` is present. One record's `pid=` was alive at listing time, but that pid belonged to an efs-fuse of this task's own gate in `/tmp/efs-taskEP-tmp` (pid reuse), not to the client that wrote it. None of the files belongs to a running client.
- Attribution: the records do not name a harness, only a pid and the mount-path hash. The phases and times match the old binaries described above. Task DI's clean-up at 16:13 on Nov 5 removed records older than 24 h and left 855 younger ones. Later gates and Tasks DG and DH on pre-wave-58 binaries added more until 19:57. Nothing has been added in the four days since, through waves 58 to 65 and their fstor007 gates.
- The DI clean-up script removes them after the user approves: `ssh fstor007 'bash -s list' < scripts/evidence/task-di/tmp_hygiene.sh`, then `bash -s remove`. It keeps a `.stop` whose pid is alive.

## After: runs on devel `33cd0a839`

| Check | Result |
|---|---|
| `EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` with `TMPDIR=/tmp/efs-taskEP-tmp` (every mount test, each case in its own `case-<n>` directory, which the runner keeps) | `TOTAL 596 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (675s) OVERBUDGET 1 BUILD 13s`, rc 3 because of the NOTRUNs. These are the usual three host NOTRUNs: `test_cluster_stop.py` (no `~/git/cluster`), `test_rdma_memcheck.py` (no valgrind header) and `test_qualify_rdma_live.py` (no RDMA). OVERBUDGET: `test_ao_readdir_live.py` 42.0 s against 30 s (budgets belong to Task EN). |
| `make test` with `TMPDIR=/tmp/efs-taskEP-tmp/maketest` | rc 0 |
| `efs-control-*` under `/tmp/efs-taskEP-tmp` (556 case directories plus `maketest`) | **0** |
| New `/tmp` entries owned by `erbmi1` since the tar arrived, other than `efs-taskEP*` | **0** (no shared `/tmp` path in the tests, so there is no second finding) |
| `/tmp/efs-control-*.stop` after the runs | 2,858, unchanged |

## Raw data and reproduction

The listing, counts, gate summary and timings, and the `make test` log are outside Git at `~/efs-artifacts/taskEP-20261109/` on the NFS home (8 files; `SHA256SUMS` sha256 `36f3981eed241534e3cb0a7f375bfb14b39de7b622327177294bc7e8155411b5`).

```
export TMPDIR=/tmp/efs-<name>-tmp EFS_FUSE_PREFIX=<libfuse shim>   # plus PKG_CONFIG_PATH, LD_LIBRARY_PATH
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
find "$TMPDIR" -name 'efs-control-*' | wc -l       # expect 0
ls /tmp | grep -c '^efs-control-.*\.stop$'        # must not grow during the run
```

Limits: a single gate run on one host. The SIGKILL path above is left as it is. Operator mounts started by `~/git/cluster` and `~/git/devops` use `/tmp` (they export no `TMPDIR`). Their stops go through `client.sh stop`, so they also remove the record.
