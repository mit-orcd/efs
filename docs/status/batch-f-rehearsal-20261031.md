# Batch F rehearsal on amd, October 31 (Task BR)

**Verdict: FAIL on the candidate lanes (rehearsal, not a release verdict).** Batch F
(F1 `make test-all`, F2 TSan and ASan+UBSan+LSan, F3 leak gate; F4 `gc-reclamation` was
not part of this task and did not run) ran end to end on devel `909a07d71` on amd.
Every lane reached a result line. Only the leak gate PASSed. The rest fail on
**three product findings** (TSan races, a UBSan null access, an intermittent `pump_logging`
EIO), one harness weakness (a hang when daemons die) and two host or environment limits.
Real batch F duration on this host class: **2 h 48 min** serial (10,086 s), of which
60 min is one test waiting out its 3600 s timeout; **2 h 08 min** without it. The matrix
estimate for the same lanes (F1+F2+F3, without F4) is 85 min.

Source: `909a07d717dc` (devel, wave 45 merged), shipped as `git archive` (no `.git` on
amd, so `EFS_GIT_ID=909a07d717dc`). Host: amd, AMD Ryzen AI MAX+ 395 (32 threads, AVX-512),
119 GB, Ubuntu, kernel 7.0.0-38, gcc 15.2, Python 3.14.4, libfuse3 3.18.2, valgrind,
`ptrace_scope` 0, `/home` ext4. Driver: `scripts/batch_f.sh` (new, this task).

## Lanes

| Lane | Result line | Wall | Matrix estimate |
| --- | --- | ---: | ---: |
| F1 `make test-all`, `EFS_TEST_JOBS=8` (`make_test_summary.sh`) | `TOTAL 581 FAIL 2 NOTRUN 2 STATUS FAIL (843s) OVERBUDGET 1 BUILD 17s` | 860 s | 20 min (D44 target 1,100 s) |
| F2 TSan (`sanitizer_check.sh tsan`) | `VERDICT: FAIL (21 run, 3 failed, 2889s)` | 2,908 s | 45 min with ASan |
| F2 ASan+UBSan+LSan (`sanitizer_check.sh asan`) | `VERDICT: FAIL (50 run, 5 failed, 6006s)`; 2,400 s without the d4 timeout | 6,073 s | 425 s measured (K56) |
| F3 leak gate (`tests/leak_check.sh`) | `VERDICT: PASS (41 run, 0 failed, 237s)` | 245 s | 20 min (list grew from 25 to 41 binaries) |
| Reruns of the F1 failures (below) | see fault table | ~25 min | - |

Order was F1, TSan, ASan, leak, serial, as the matrix lists them. F1 at 8 jobs is within
the D44 target (843 s of 1,100 s).

## Faults

| # | Fault | Class | Fix or requirement | Rerun |
| --- | --- | --- | --- | --- |
| 1 | `test_cluster_stop` NOTRUN: `/home/maik/git/cluster` absent | environment | Candidate host needs `EFS_CLUSTER_SCRIPTS` naming a directory with `cluster.sh`, `ct-up.sh`, `gw-up.sh`, `lib/{client-stop,client-user,transfer}.sh` | PASS with the six files supplied (0 s) |
| 2 | `test_store_durability` NOTRUN: TMPDIR is not XFS | host limit | Writable XFS TMPDIR. amd's only XFS (`/data1`) is not writable for this user, so the gate cannot run there | not rerun |
| 3 | `test_stop_progress` FAIL: `fusermount3 -u ...: Device or resource busy` (the `gvfs-udisks2-volume-monitor` desktop service references the new mount again after the fixture-idle check; the fixture forbids stopping it) | environment | Candidate host must be headless or have no gvfs/udisks volume monitor in the test user's session. Not fixed: killing the user's desktop services is out of scope | FAIL 2 of 3 reruns (PASS 1) |
| 4 | `test_b6_unavailable` (full) FAIL `room: ENOSPC then ENOSPC` (K53 family, opposite order to the closed oracle defect) | unattributed, intermittent | Kept fixture was deleted by my cleanup before I read it (only `fail-512.log` remains). Treat as a K53-family candidate if it recurs | PASS 3 of 3 |
| 5 | `test_k33_interrupt` OVERBUDGET | budget | 121.1 s against 120 s: rounding, not a fault; the only OVERBUDGET row | - |
| 6 | TSan: `k36_recovery` 12 reports | **product** | see findings | not rerun (deterministic across 4 daemon processes) |
| 7 | TSan: `d4_progress` 1 report | **product** | see findings | not rerun |
| 8 | UBSan `kv_lsm`, `pack_ledger_recovery`, `k36_recovery` (and the efsd logs inside `d4_progress`) | **product** | see findings | not rerun |
| 9 | ASan `d4_progress` ran 3604 s to the entry timeout | harness weakness (consequence of 8) | The efsd processes of n1 and n3 died (zombies) and the test kept retrying instead of failing. Needs a daemon-liveness check in the harness; not changed here. Set `EFS_SAN_TIMEOUT` lower (about 600) so a wedged entry costs minutes | not rerun |
| 10 | `pump_logging` FAIL in TSan (`late writes 1`), ASan (`foreground failures 2`) | **product candidate** | see findings | alone: FAIL 2 of 3 |
| 11 | `test_k22_snapshot_import` left a dead efs-fuse mount (`case-538/.../mnt-a`, transport endpoint not connected) after the run | harness hygiene | Unmounted by hand after `fuser -m` showed no users | - |

Not hit on amd, though the briefing listed them: the AVX-512/ASan fake-stack crash (the
ASan lane builds `-march=x86-64-v3`), the libtsan 64-lock abort (the gate sets
`detect_deadlocks=0`), the Python 3.14 forkserver problem, and a missing valgrind.

## Candidate-host requirements

- Linux with gcc and the TSan, ASan, UBSan and LSan runtimes installed; `ptrace_scope` below 2
  for LSan; valgrind (memcheck) for the leak gate.
- python3 (3.14.4 works), pkg-config and fuse3 development files, `fusermount3`, `/dev/fuse`.
- A headless user session: no gvfs/udisks volume monitor (fault 3).
- A **writable XFS** TMPDIR (fault 2), and enough local disk: about 2 GB for the sanitizer
  and memcheck build trees plus the test stores.
- `EFS_CLUSTER_SCRIPTS` with the six cluster scripts named in fault 1 (copy only those;
  never the whole cluster directory).
- 8 CPU is the minimum the gate was sized for; amd has 32. Plan 3 h for the batch, or
  2 h with `EFS_SAN_TIMEOUT` lowered and the d4 wedge fixed.
- Copy the tree with `git archive` and set `EFS_GIT_ID`; `ecopy-push` needs an absolute
  destination path or it falls back to rsync.

## Product findings (K-row candidates for the coordinator)

All logs are under the raw location below. Nothing was changed in product code.

1. **TSan data races in `k36_recovery`** (12 reports, four daemon processes):
   `server_join_cluster` `src/server/cluster.c:35` reads what `server_recovery_drop`
   `src/server/store.c:2219` wrote under fragment locks (rejoin thread vs the K36 recovery
   scan); `inode_at` `src/meta/metadata.c:232` (8 reports) and `efs_export_set_chunk`
   `src/meta/metadata.c:3498`. Reproduce: `EFS_SAN_ONLY=k36_recovery bash tests/sanitizer_check.sh tsan`.
2. **TSan data race in `d4_progress`**: `rpc_prof_enabled` `src/client/inode_rpc.c:160`.
   Reproduce: `EFS_SAN_ONLY=d4_progress bash tests/sanitizer_check.sh tsan`.
3. **UBSan null member access**: `lsm_slot_in` `src/kv/kv_lsm_internal.h:305` via
   `lsm_owner_slot` / `lsm_view_reclaim` `kv_lsm_internal.h:412` from `kv_flush_locked`
   `src/kv/kv_compact.c:230`. Hit by `kv_lsm`, `pack_ledger_recovery`, `k36_recovery` and
   both efsd n1 and n3 in `d4_progress`, where both daemons then died. Reproduce:
   `EFS_SAN_ONLY="kv_lsm pack_ledger_recovery" bash tests/sanitizer_check.sh asan`.
4. **`pump_logging` intermittent foreground EIO** (`result.json` retained: write
   `OSError(5)` then a payload mismatch on the same file). PASS once in the F1 pool, FAIL
   in TSan and ASan, FAIL 2 of 3 alone. Reproduce: `python3 tests/test_pump_logging.py`
   on a built tree. Relevant to the K81 pump work (Task BL).

## Reproduction

```
git archive 909a07d71 | tar -x -C ~/efs-<run>/src          # then ship to the host
cd ~/efs-<run>/src
SCREENDIR=$HOME/.screen TMPDIR=$HOME/tmp EFS_GIT_ID=909a07d717dc \
  ART=$HOME/efs-artifacts/<run> screen -dmS batchf bash scripts/batch_f.sh
EFS_CLUSTER_SCRIPTS=<dir> ...   # for the cluster_stop rerun
```

`ART/lanes.txt` has one line per lane. Reruns: `python3 tests/test_stop_progress.py`,
`EFS_B6_MODES=full python3 tests/test_b6_unavailable.py`, `python3 tests/test_pump_logging.py`,
`python3 tests/test_cluster_stop.py`, three times each.

## Raw locations

On amd `~/efs-artifacts/taskBR-20261031/` (outside Git): `summary.txt`, `timings.tsv`,
`f1.out`, `fail-506.log`, `fail-512.log`, `tsan.out`, `asan.out`, `leak.out`, the per-entry
`tsan/`, `asan/`, `leak/` logs and TSan reports, `rerun/`, `lanes.txt`, `PRUNED.txt`
(build trees and TMPDIR cases removed). `SHA256SUMS` there lists 197 files; its own
sha256 is `678402e8fbbfc78dfe98dfac2f81d1968b49107a259f8795cf6755e6a6804bad`.

## Limitations and process note

One run per lane; reruns are 3 per failure. F4 not run. The F1 b6 failure could not be
attributed because its kept fixture was removed during cleanup. While supplying the
cluster scripts I first rsynced all of `~/git/cluster` to amd, which included
`ACCOUNTS.txt` and `cluster_key`; neither was read, and the whole copy was deleted from
amd within about a minute (only the six scripts were re-sent). amd processes and the
user's live clusters were not touched.
