# K105: the packer counted a read error on a file unlinked under it (Task FK)

**Status (Nov 10):** fix committed on branch `claude/wizardly-lehmann-3525bb`, not merged.

**Verdict: PASS for the fix, case C by inference, not reproduced. A packer pass reads a candidate's metadata, then its
fragments; a file unlinked (or republished) in between has its fragments reclaimed, and the packer counted the two
NOT_FOUND reads as a `read_errors` of a live file. The packer now asks, after a failed read, whether the candidate is still
the live inode with the source row it read (`efs_meta_pack_candidate_alive`): a gone or superseded candidate is skipped and
logged `candidate-gone`, a live unchanged one still counts and still fails `tests/test_d4_progress.py`. No data was
unavailable: the run's own verification read every live file exact after the drain. The failure did not reproduce in 40
loaded runs on devel plus a log-only diagnostic and in 80 loaded runs with the fix, so the fix is not shown by a before
and after loop. Proposed K105: FIXED on the branch, blocks alpha: no (wording at the end).**

Task FK, Nov 10 2026, node9900 (private trees and loopback ports only). Linked row: K105 (both tables of the
[alpha checklist](../alpha-1.0-2plus1-checklist.md)); gate item: the wave 71 merge gate's `tests/test_d4_progress.py`
(D4 bounded progress).

## Source and environment

- Base: devel `cff8eb424`. Branch `claude/wizardly-lehmann-3525bb`, two commits: `900b22b90` (log-only diagnostic:
  `packer: candidate-state ...` after a failed candidate read, behaviour unchanged) and `3a86c2e98` (the fix, the unit test
  `tests/test_pack_candidate_alive.c`, its `tests/budgets.tsv` row, the Makefile and `.gitignore` entries). The
  reproduction scripts `scripts/evidence/k105/d4_loop.sh` and `load_loop.sh` come with the evidence commit. Not merged.
- Change: `efs_meta_pack_candidate_alive` in `src/meta/meta_apply/pack.c` (declared in `include/efs/pack_mapping.h`),
  `pk_candidate_gone` and the read-failure loop of `pk_group` in `src/server/packer.c`. No wire, format or setting change.
- Host: node9900 (192 CPUs), `/tmp/efs-taskFK*`, `TMPDIR=/tmp/efs-taskFK-tmp`, loopback ports chosen by the harness,
  `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`; no live cluster touched. Each tree is a `git archive` of the commit; the
  dev build prints `EFS_BUILD_ID unknown`, identity is the commit and the binary hashes below.
- Trees: *diag* = `900b22b90` (packer behaviour of devel): `efsd` sha256 `e81491ec738b`, `tests/faults/efsd`
  `b8df445a7de9`, `efs-fuse` `55587a749ce2`. *fix* = `3a86c2e98`: `efsd` `63ba5cf98791`, `tests/faults/efsd`
  `82b042d98686`, `efs-fuse` `4dd4e2acf284`.
- Test fixture (`tests/test_d4_progress.py`): three `tests/faults/efsd` daemons on loopback, quota 1G, RDMA transport
  available on the host, FUSE mounts L (node 1, small-file load) and F (node 2, foreground), 45 s load with 8 writer
  threads, packer minimum age 1000 ms and pass interval 200 ms (`EFS_FAULT_PACK_MIN_AGE_MS`,
  `EFS_FAULT_PACK_INTERVAL_MS`), 256 KiB memtables. What the load does to files: each writer creates a file of 1 to 8192
  bytes (one `open`, `write`, `close`, never rewritten) and every eighth create unlinks a random older file of its own
  (`scripts/d4_progress.py`, `writer`: the victim is any of its files except the newest eight, so a file one second old can
  be chosen). Nothing is overwritten or truncated in this mode.

## Timeline of inode `0x36bbf` (224191), generation `aff516616d9abb58`

Fixture: the wave 71 merge gate's case 526 (tree `68842d066`, `make test-all`, 8 jobs), preserved on the ORCD home as
`~erbmi1/efs-artifacts/merge-wave71-20261110-node9900-d4fixture/` (`d4-logs.tar.gz` sha256 and the other files: its
`SHA256SUMS`; the cited hash prefix `f81ebeb1254d` is that file's). It holds three daemon logs, two client logs and the
driver's JSON. All times UTC, 2026-11-10.

| Time | Source | Event |
|---|---|---|
| 07:24:16.588 | n1 | packer started, `min_age_ms=1000 interval_ms=200` |
| 07:24:19.87 | driver | load starts (`load_started_epoch_s` 1794295459.87); mount L session `bbe55b49` |
| 07:24:50.379 | n1 | `opid-receipt: replay shard=3007 id=bbe55b49/1/76263 rc=0 ino=224191`: a same-identity retry from mount L, answered from the receipt. The only line before the failure that names the inode. |
| 07:24:55.187 to .259 | n1 | packer passes of group 0 around the failure: `lag_s=11 candidates=4096 eligible=3302 segments=8 scan_ms=14 read_ms=9 pass_ms=47` and `scan_ms=13 read_ms=7 pass_ms=55`. The pass that failed read its candidates' metadata (`scan_ms`) a few milliseconds before the fragment reads (`read_ms`). |
| 07:24:55.209 | n1 | `fragment-read source=local ino=...36bbf generation=aff516616d9abb58 fi=0 node=1 rc=-3 want=65536 got=65536 status=-1 verified=0 reply_type=-1 failure=1` |
| 07:24:55.209 | n1 | `fragment-read source=remote ... fi=1 node=2 rc=-1 want=65536 got=1 status=1 verified=0 reply_type=5 failure=2` |
| 07:24:55.212 | n1 | `candidate-read g0 object=...36bbf generation=0 rc=-1 failure=1` |
| 07:24:55.96 | driver | first sample with `read_errors: 1`, `backlog {0: 4034}` |
| 07:25:07.762 | n1 | last packer pass: `candidates=82 packed_total=34239`, backlog 0 |
| t=51.58 s | driver | last sample (drain): `read_errors` still 1, `put_errors` 0, backlog 0 on both groups |

Everything else the logs say about it: nothing. No log line mentions the inode or the generation after 07:24:55.212, in
any of the five logs. The three failure lines are numbered `failure=1`, `failure=2`, `failure=1`: they are the only
fragment read failures (2) and candidate read failures (1) of the whole run (the packer logs the first 256 of each), so
the file was **not** retried and failed again. A file that stays a candidate is read again by the next pass, 200 ms later,
and the run went on for 12 more seconds and 76 more logged passes of group 0.

Counts of the run (`d4-result.json`): 35,974 files created, 4,479 unlinked, 31,495 live; `files_packed` 34,239; 139
foreground files; `verify_bad []`, `errors []`, `drain_s` 5.8, every live file read back exact (`verify_s` 19.1).
34,239 packed + 1,735 unlinked before they were packed = 35,974: the packer meets candidates that are unlinked unpacked
routinely (1,735 of them in this run, 4.8 % of the files), and drops them at the next scan.

What the logs cannot say, and the line that is missing: whether ino 224191 was unlinked. No daemon logs a client unlink,
and the stores were deleted with the gate's `case-526` directory (the retained path no longer exists on node9900), so
nothing names the inode's fate. The line the fix adds is `packer: candidate-gone ino=... read_rc=...`; the diagnostic
commit `900b22b90` printed `candidate-state ... alive_rc=` for the same moment.

## The read path (packer.c)

- A pass (`pk_group`, `src/server/packer.c:1283`) scans up to 4096 candidate keys, reads each one's metadata with
  `efs_meta_pack_candidate` (inode row, lane, chunk row: the delta list with generation, node ids and checksums), keeps
  the files old enough (`mtime + min_age`) and not open, sorts them, cuts jobs by the token budget, then reads all
  fragments of the chosen files in one parallel batch (`pk_read_files`, 16 threads), and builds and uploads segments.
- `pk_read_file` reads, per file, the base (if any) and every delta with `pk_get_d1`: a small file lies wholly in data
  fragment 0, so it tries fragment 0 (`fi=0`), and only if that fails fragments 1 and 2 XORed. So one intact fragment 0 is
  enough; without it both fragments 1 and 2 are needed, and the third fragment is never tried once the second failed. In
  the fixture fragment 0 (local, node 1) and fragment 1 (remote, node 2) were both absent, so fragment 2 was not read.
- `pk_get_fragment` (`:288`): the local path is `efs_store_get` on this node's store; the remote path sends
  `GET_CHUNK` on a pooled connection and checks `type == GET_CHUNK_REPLY`, `rlen == 1 + hash + len` and `r[0] == OK`.
  Then the payload hash is compared with the checksum in the metadata (`EFS_ERR_CHECKSUM`).
- Fields of `fragment-read` (`pk_fragment_failure`, `:277`): `source` local, remote or checksum; `fi` fragment index;
  `node` the member holding it; `rc` the code returned (-3 `EFS_ERR_NOT_FOUND`, -1 `EFS_ERR_IO`); `want` the fragment
  length asked; `got` the length the store set (`efs_store_get` leaves it at `want` when it fails, which is why local
  `got` equals `want` with `rc=-3`) or, for a remote reply, `rlen`; `status` the reply's first byte (remote only,
  -1 local); `verified` whether the store verified the record and hash; `reply_type` the message type (5 =
  `GET_CHUNK_REPLY`, -1 local); `failure` a process-wide running count (logged for the first 256 and every 1024th).
- **Remote reply of 1 byte with `reply_type=5`, `status=1`**: the member's `GET_CHUNK` handler (`src/server/handler.c`,
  the `EFS_MSG_GET_CHUNK` case) answers `EFS_GET_CHUNK_NOT_FOUND` (1, `include/efs/protocol.h:820`) with a one-byte body
  whenever `efs_store_get` fails for any reason other than integrity (format, identity or checksum fail with status 3).
  So node 2 had no readable fragment 1 for that generation, exactly as node 1 had no fragment 0.
- After a failed read (`:1451`, "a file that cannot be read stays a candidate"): `pk_read_failure` logs `candidate-read`
  (its `generation=` is the *base* generation, 0 for a freshly written file whose bytes are in the delta), `read_errors`
  is incremented once, the file's buffer is freed, and the candidate key stays in the index, so the next pass reads its
  metadata again. If the file is unlinked by then, `efs_meta_pack_candidate` reports it `STALE` or `nlink == 0` and the
  scan drops the key (`unindexed`). `read_errors` is what `tests/test_d4_progress.py` asserts to be 0.

## Which case

**Case C (the file was being removed or replaced and the packer counted a vanished candidate as an error), by inference.**
For each case:

- **A (live and acknowledged, two fragments truly unreadable): excluded by the run itself.** A file with two of its three
  fragments gone cannot be reconstructed (2+1 needs two), and the driver read every one of the 31,495 live files and the
  139 foreground files back exact after the drain (`verify_bad []`). If 224191 was live, it was readable later.
- **B (fragments present, read at a moment or in a way that fails): no mechanism found, improbable.** For B the
  fragment would have to be absent on two members at once, `NOT_FOUND` from both stores, and present again within seconds
  (the file would have been read on the next pass, 200 ms later, and was not counted again). The packer only considers a
  file 1 s after its mtime. The wave 69 packer changes are not implicated by the code: `7553a4124` replaced nine
  environment reads by constants and `b4066cf4c` renamed the remaining wait variables to `EFS_FAULT_PACK_*` behind
  `EFS_FAULTS`; neither touches a read path.
- **C: consistent with every line.** The window is real in the code: metadata is read at scan time, fragments some
  milliseconds later; the unlink commit and the reclaim of a dead inode's fragment directories (`reclaim.c`: the dead
  inode is fenced, then the deleter pool removes the directories on every member) are not ordered against a packer pass;
  the load unlinks 4,479 files, 1,735 of them before they were packed; both fragments read were `NOT_FOUND`, which is what
  reclaim leaves behind on every member; and the read error did not repeat, which is what a dropped candidate does.
- Not proven: no log line names the unlink (above). If a later occurrence shows `candidate-read` without
  `candidate-gone`, the file was live and unchanged, and that is a different defect (A or B) that still fails D4.

## The change

`efs_meta_pack_candidate_alive(kv, seen)` re-reads the candidate (`efs_meta_pack_candidate`) and returns `EFS_ERR_STALE`
when the inode row is gone or has another generation, the chunk row is gone, `nlink` is 0, or the chunk row's digest
(`source_digest`) differs from the one the pass read; `EFS_OK` otherwise. In `pk_group` a failed read of a candidate whose
check returns `STALE` is logged `packer: candidate-gone ino=... read_rc=...` (first 256, then every 1024th), is not added
to `read_errors`, and the key stays for the next scan to drop. The check runs after the failed read: an inode's death is
committed in the metadata before reclaim fences it and removes fragments, so a live-and-unchanged answer after a failure
means the failure was real and is counted. An out-of-memory result is never treated as gone.

## Regression test

`tests/test_pack_candidate_alive.c` (in `make test`, mem KV, about 2 ms, `tests/budgets.tsv` row 30 s ceiling like its
neighbours): a live file read by a pass is alive; the same inode republished with a new generation is `STALE`; a
hard-linked file with one name unlinked is still alive; a file unlinked after the read is `STALE`. It cannot build without
the function, so it fails before the change. It tests the decision, not the call in `pk_group` (the daemon path needs the
race, which no hook controls without a sleep); the wiring is the loop below plus the code reading.

## Runs

Loop of step 4 and 6: `scripts/evidence/k105/d4_loop.sh` runs `python3 tests/test_d4_progress.py` repeatedly in a built
tree; `load_loop.sh` runs the fast suite (`make test` at 8 jobs through `scripts/make_test_summary.sh`) in a second copy
of the same tree as load. The host was quiet of other tasks (three checks 20 s apart) before each set.

| Set | Tree | Streams x runs | Failures | Load | Time per run | 1-min load at start |
|---|---|---|---|---|---|---|
| Reproduce (devel behaviour) | diag `900b22b90` | 1 x 40 | 0 | `make test` x 128 cycles, all `FAIL 0` | 92 to 104 s (median 98) | 5.6 to 19.6 (median 11.1) |
| After the fix | fix `3a86c2e98` | 2 x 40 = 80 | 0 | `make test` x 109 cycles, all `FAIL 0` | 89 to 105 s (median 95) | 2.5 to 23.6 (median 20.4) |

The 2094fea52 (before wave 69) loop was not run: it is for the case of failures on devel, and there were none.

Gates on the fix tree `3a86c2e98` (`make test`, `python3 tests/test_k37_segment_rebuild.py`,
`python3 tests/test_k41_packer_placement.py`, `make asan-check`, `make tsan-check`):

| Gate | Result |
|---|---|
| `make -j8 test` | rc 0 (161 s) |
| `tests/test_k37_segment_rebuild.py` | rc 0 (42 s) |
| `tests/test_k41_packer_placement.py` | rc 0 (50 s) |
| `make asan-check` | PASS, 59 run, 0 failed (428 s) |
| `make tsan-check`, run 1 | FAIL, 31 run, 1 failed: `k37_segment_rebuild` (`wait_packed ... packer moved 0 of 4`, 704 s). Same signature the K103 page records on the unchanged wave 71 tree. No ThreadSanitizer report; the daemon logs of that case have no `candidate-read`, `fragment-read` or `candidate-gone` line, so the changed path did not run. |
| `make tsan-check`, run 2 (after three quiet-host checks) | PASS, 31 run, 0 failed (630 s) |

## Reproduction

```
# on a Linux host with the private libfuse; TREE = git archive of the commit under test
make -j32 EFS_FUSE_PREFIX=/data1/efs/deps/libfuse all tests/faults/efsd
cp -a TREE TREE-load
bash scripts/evidence/k105/load_loop.sh TREE-load TAG &          # 8-job `make test` as load; touch STOP-TAG to end
bash scripts/evidence/k105/d4_loop.sh TREE TAG 40                 # 40 runs of tests/test_d4_progress.py
# expected: runs.tsv rc 0 on every line; a failure keeps its work directory and prints `packer errors: {... read_errors ...}`
python3 tests/test_d4_progress.py                                 # one run
make test; python3 tests/test_k37_segment_rebuild.py; python3 tests/test_k41_packer_placement.py
make asan-check; make tsan-check
```

Cleanup: `edelete --force --delete` of the work trees and `TMPDIR`; failed-run directories are the only retained state.

## Raw output

Not in Git. On the ORCD shared home `~erbmi1/efs-artifacts/taskFK-20261110/` (21 files, `SHA256SUMS`, whose own sha256
begins `285f98960203`): `runs-diag.tsv`, `runs-fixa.tsv`, `runs-fixb.tsv`, `load-diag.tsv`, `load-fix.tsv`,
`d4-run-logs.tar.gz`, the build logs, `binary-sha256.txt`, the gate logs (`gate-*.log`, `gates-rc.tsv`), the first TSan run's
`k37_segment_rebuild` output and daemon logs, and the fixture timeline extract. The fixture itself stays at
`~erbmi1/efs-artifacts/merge-wave71-20261110-node9900-d4fixture/`. `/tmp/efs-taskFK*` is removed.

## Not checked

- Whether ino 224191 was unlinked: no log or store survives; case C is inferred from the fixture's counts, the single
  non-repeating error and the fully verified end state.
- No before and after loop: 0 failures in 40 loaded runs of the unchanged packer behaviour, so the unit test and the code
  path argument carry the fix. The gate that failed ran `make test-all` at 8 jobs (heavier than the `make test` load here).
- Only the packer's candidate read (`pk_group`) is changed. The same race can count a `segment-read` failure in
  `pk_compact` (`read_errors` at `:1089`) for a segment retired meanwhile; not seen, not changed.
- No live-cluster run, no RDMA-specific arm beyond what the fixture's transport used.
- A planned extra set (12 runs per stream keeping every packer `candidate-*` line, to count how often a pass meets a gone
  candidate) was started by mistake while another task's merge gate (`/tmp/efs-merge73*`) was running on node9900, and
  stopped within about two minutes by pid; no result from it is used, and it was not rerun. The rate of the window is
  therefore not measured.

## Proposed K105 wording (the K tables are not edited here)

Full table row, status column: `FIXED on branch claude/wizardly-lehmann-3525bb (Task FK, Nov 10), blocks alpha: no: a
candidate unlinked or republished between the packer pass's metadata read and its fragment reads has its fragments
reclaimed, and the packer counted the NOT_FOUND reads as a read error; case C by inference (the run's verification read
every live file exact, the error did not repeat, 1,735 of 35,974 files were unlinked unpacked), not reproduced in 120
loaded runs, fix not shown by a before and after loop; after the fix a gone candidate logs candidate-gone and is not
counted, a live unchanged one still fails D4; reopen with blocks alpha: yes if candidate-read appears without
candidate-gone (a live file unreadable) (page: `2026-11/k105-packer-read-error-20261110.md`, linked from the checklist rows)`.

Short table row: `FIXED on branch (Nov 10, Task FK), blocks alpha: no: the packer counted a read error on a candidate
unlinked under its pass; it now skips a gone candidate (candidate-gone) and still counts a live one; case C by inference,
not reproduced in 120 loaded runs (page: `2026-11/k105-packer-read-error-20261110.md`, linked from the checklist rows)`.
