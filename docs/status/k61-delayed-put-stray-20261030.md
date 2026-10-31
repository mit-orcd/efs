# K61: a delayed PUT resurrecting a fragment of a superseded generation (Task BH)

**Verdict: FIXED, blocks alpha: no.** Closes K61 ([Task BA audit](ba-deletion-authority-audit-20261029.md)).
Fix commits `5a4ed0280` and `1e6ab77a1` (the tested source) on `claude/bh-k61-f67263` (cut from devel `8b0d128cc`).
Gate result and hot-path measurement below; raw outputs stay on xorinox
(`~/efs-artifacts/taskBH-20261030/`, `SHA256SUMS` there).

## Defect

`server_put_upload` and `server_rebuild_put` read their authority (ticket
ADMITTED, degraded mark) before the fragment mutex that
`server_gc_superseded_delete` takes. A PUT delayed after that read could pass it,
the object be superseded and its fragment deleted, and the PUT then recreated the
dead generation's file. Nothing references it and K36 recovery does not sweep
unreferenced files, so repeated occurrences accumulate.

## Fix

A per-fragment-lock-bucket retire epoch (`frag_retire_epoch[4096]`,
`frag_retire_key[4096][8]`, the last 8 retired identities, `src/server/store.c`), written under the bucket mutex by
`server_delete_fragment_if_sum` (the unlink under every superseded/orphan/inode
delete).

1. The PUT takes `server_fragment_retire_mark()` (one acquire load) **before**
   reading its authority and passes it in `efs_tls_retire_mark` for the store call.
   The writer pool copies it onto the job like `chunk_generation`.
2. `server_write_fragment_sync` / `_with_sum_sync` check it **under the fragment
   mutex they already take**, before the write: unchanged epoch passes; retires of other
   identities pass; a retire of this identity, or more than 8 since the mark, refuses: the PUT answers
   `EFS_ERR_STALE` (fail closed, existing error). No lock is added or widened; no
   I/O is moved; callers without a mark (`EFS_RETIRE_MARK_NONE`) do one compare.

Residual: more than 8 retires in one bucket within one PUT's authority window
refuse a valid PUT (STALE; the client treats the member as missing and rebuild
repairs it). A first version remembered only the last identity and refused on any
second retire; the full gate hit a `test_pack_dup_candidate` EIO under load with
it (passes alone), so the ring replaced it.
No tombstone is persisted: the late PUT is a thread of the same process, so a
restart removes the window.

## Tests

`tests/test_ba_upload_retire put|rebuild` (real `server_put_upload` /
`server_rebuild_put`, session guard, nvme store, **real writer pool**, superseded
delete; the authority reply is held by the test's `server_raft_host_submit`, so no
`EFS_FAULTS` hook was needed). Now in `make test-long` with two `tests/budgets.tsv`
rows (5 s each); `python3 tests/test_make_test_recipe.py` PASS (1646 s of 1650 s).

| tree | `put` | `rebuild` |
|---|---|---|
| devel `8b0d128cc` (xorinox) | FAIL (`access(path) && errno == ENOENT`) | FAIL |
| fix | PASS | PASS |

Mutation check: removing the line that copies the mark onto the writer thread
makes the `put` case fail again (the pool path is covered, not only the inline one).

## Hot-path cost (xorinox, AMD Ryzen 9 7940HS, NVMe, direct I/O)

`efs-bench --bench data --storage <empty root under $HOME> --time 10 --writers 8
--qd 16 --rw write --write-mode overwrite --window 64 --skip-ceiling`, base and fix
built with the same flags, alternated, nothing else running
(`~/efs-artifacts/taskBH-20261030/bench.sh`). An earlier 15 s `create`-mode series
was discarded: it filled the device and both trees degraded to 19 k ops/s.

| run | base ops/s (p50/p99 µs) | fix ops/s (p50/p99 µs) |
|---|---|---|
| 1 | 55696 (235/683) | 55707 (235/695) |
| 2 | 56244 (235/647) | 56078 (234/683) |
| 3 | 55999 (235/659) | 55993 (235/667) |
| median | 55999 | 55993 |

Median throughput difference −0.01 %, p50 identical: **within noise, well under
the 2 % limit.** Limitation: this bench drives the store directly, so it covers the
unmarked branch and the larger critical-section code, not the marked path. The
marked path adds one acquire load and a hash per PUT before the metadata RPC, and a
few loads/compares under a mutex already held across the write, against a PUT that
costs a ReadIndex round trip plus a ~235 µs O_DIRECT write; it was not timed
separately.

## Reproduction

```
make tests/test_ba_upload_retire && ./tests/test_ba_upload_retire put && ./tests/test_ba_upload_retire rebuild
make efs-bench   # then bench.sh as above; scratch roots must be empty
TMPDIR=$HOME/tmp EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
```

Failing-on-base: `git archive 8b0d128cc`, `make tests/test_ba_upload_retire`
(the base already carries the test and its Makefile rule); both modes exit 1.
Cleanup: delete the scratch roots; a failing run keeps `/tmp/efs-ba-upload-*`.
On xorinox set `TMPDIR` to a directory under `$HOME`: `/` is 89 % full and
`test_direct_io_required` fails there (host limit, not this change).

## Gate

`scripts/make_test_summary.sh` (`make test-all` set) at 8 jobs on xorinox, tree
at `1e6ab77a1`: **TOTAL 581 FAIL 0 NOTRUN 10 STATUS INCOMPLETE (938 s)**, OVERBUDGET 4
(`test_b_geometry` 138.6 s vs 120 s budget). NOTRUN = no RDMA device (9) and
`/home/maik/git/cluster` absent for `test_cluster_stop` (1): host limits.
`python3 tests/test_code_graph.py` OK; `docs/check-architecture.py` 5/5.
Earlier attempts, kept outside Git: plain `make test-all` stopped at the first
failure (`test_direct_io_required` with `TMPDIR` on the full root; then
`test_pump_logging`, the known intermittent of K72, which is not touched by this change
and passed in the final run); the first-version run's `test_pack_dup_candidate`
EIO is described above.

Raw outputs: `~/efs-artifacts/taskBH-20261030/` on xorinox; `gate-summary.txt`
sha256 `88dccaadd72ddb729c3bb80e8857f3379a9d760d24db31107902ef9972e1c63f`,
`SHA256SUMS` sha256 `19161b520d4c34757d48f77dabc13982784443e4d43aa19042ec0103b6c24b09`.

Not done: the marked PUT path was not timed separately; no cluster-level run
(three-member Raft with a real delayed client).
