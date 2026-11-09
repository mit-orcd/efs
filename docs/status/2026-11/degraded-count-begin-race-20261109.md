# Degraded-count begin: pre-commit under-count race (follow-up of Task EI)

**Verdict: PASS (Nov 9). The race is real in the code and fixed.**
`efs_meta_degraded_count_begin` dropped every mark noted while the count
was unknown, but a mark is noted inside the raft apply scope before the
scope commits, while the rebuild reaper's derive scan reads the committed
table: a mark of the apply in flight could be in neither, and the count
could miss it until the next derive. `begin` now keeps the latest apply's
marks, using the apply-start hook Task EI added for the GC watermark
(`946095b28`, `gc.c` plus a header comment and the test). node9900:
`make test` rc 0, `tests_for.py meta/meta_apply --run` rc 0,
`./tests/test_meta_apply degraded` rc 0 and rc 1 with the old rule. No K
row (found by reading, not observed); blocks alpha: no. Merged into devel in wave 67 (Nov 9).**

- Purpose: the candidate recorded under Findings on the
  [blind reader page](blind-reader-20261109.md). Task EI's question 2
  reader found that a GC insert note runs before its apply commits, so the
  k37 rule for `begin` (drop everything noted while the count is unknown)
  can under-count; EI fixed the GC watermark (`6e7f838aa`). The degraded-mark
  count (W154, the k37 fix of
  [Task DY](k37-degraded-count-overshoot-20261108.md)) has the same shape.
- Source: branch `claude/task-ek-degraded-begin-race`, cut from the Task EI
  branch tip `860b51ba8` (EI merged in wave 66; this change reuses its
  `efs_meta_gc_pending_apply_start` hook). Fix `946095b28`.
- Environment: node9900 (192 cpus, gcc, libfuse from
  `/data1/efs/deps/libfuse`), `git archive` of `946095b28` (tar sha256
  `79d18360876470e2e9a9e4523f80412d3dad7a2eec9735acb187231f1b821441`),
  build under `/tmp/efs-taskEK`, `TMPDIR=/tmp/efs-taskEK-tmp`, in
  `screen`. Raw output: `~erbmi1/efs-artifacts/taskEK-20261109/` on
  node9900, `SHA256SUMS` sha256 `9db52e94be0953351033fa27ff73d2f883d815ae9f34e995be6f59f3c9bde25b` (11 files).

## The window, from the code

1. Every degraded-count note runs inside `host_apply`'s LSM apply scope.
   `degraded_note` is called by `efs_meta_apply_degraded_mark` and
   `efs_meta_apply_recovery_mark` (`gc.c`, +1 after `meta_write_batch`),
   `efs_meta_apply_degraded_clear` (−1) and the publish path (`publish.c`,
   net after `meta_write_batch_as`). Their server callers
   (`apply_degraded_mark_cmd`, `apply_recovery_mark_cmd`,
   `apply_degraded_clear_cmd`, the publication apply) are reached only from
   `host_apply_dispatch`, which `host_apply` calls between
   `efs_kv_lsm_apply_begin` and `efs_kv_lsm_apply_end`.
2. Inside the scope a batch is staged; it commits at
   `efs_kv_lsm_apply_end`. Other readers "see the old committed table"
   until then (`include/efs/kv_lsm.h`).
3. The rebuild reaper runs on its own thread. When the count is unknown
   (process start, snapshot import, the invalidations at `raft_host.c`
   3864, 3938, 3962) it calls `efs_meta_degraded_count_begin`, scans the
   degraded prefix and derives. A mark noted by the apply in flight was
   dropped by `begin` and is not in the scan: the derived count is low by
   that apply's marks.
4. Effect: while the count is low, a clear of another mark can take it to
   0, and a pass that finds `have == 0` returns without scanning. The
   missed mark's object stays degraded (two of three fragments) without a
   rebuild until the next derive. Not observed on a cluster; found by
   reading.

## Fix (`946095b28`)

`dg_wm_cur` counts the marks noted while unknown since the latest apply
began; `efs_meta_gc_pending_apply_start` (already called by `host_apply`
under the KV writers lock, which serializes applies) now also zeroes it;
`efs_meta_degraded_count_begin` sets `dg_wm_pos = dg_wm_cur` instead of 0.
A mark noted before the latest apply began is committed, so the scan
counts it if it is live; the latest apply's marks and those noted during
the scan are kept. The count can overshoot by at most those, never miss a
mark. Files: `src/meta/meta_apply/gc.c`, the comment in
`include/efs/meta_apply.h`, `tests/test_meta_apply.c`. No harness splices
the degraded reaper or stubs `degraded_note`, so no stub changed.

Test (`test_degraded_marks`): the existing "derived once, not twice" step
now marks a later apply's start before `begin` (in a real host every
earlier mark is followed by another apply before the reaper derives); a
new block marks one object in "the latest apply", derives from a scan
taken before it, and checks the mark is kept and the count equals the rows
once committed.

## node9900

| Tree | Command | Result | Wall |
|---|---|---|---|
| `946095b28` | `make -j64 all` | rc 0 | — |
| `946095b28` | `./tests/test_meta_apply degraded` (all degraded cases) | rc 0 | — |
| `946095b28` | `./tests/test_meta_apply gc_pending` | rc 0 | — |
| `946095b28` | `python3 tests/test_apply_unsupported.py`, `test_txn_apply_exact.py`, `test_gc_fragment_backlog.py` | rc 0 each | — |
| `946095b28` | `make -j64 test` | rc 0 | 126 s |
| `946095b28` | `python3 scripts/tests_for.py meta/meta_apply --run` | rc 0 | 4 s |
| `946095b28`, `begin` sets `dg_wm_pos = 0` (the old rule) | `./tests/test_meta_apply degraded` | rc 1: `FAIL tests/test_meta_apply.c:5839: the in-flight mark is kept`, `5840: count equals the rows once committed` | — |

Reproduce: `git archive 946095b28`, build with
`PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig` (and its
`lib64` on `LD_LIBRARY_PATH`), run the commands in the table. Variant:
replace `dg_wm_pos[anchor] = dg_wm_cur[anchor];` with
`dg_wm_pos[anchor] = 0;` in `src/meta/meta_apply/gc.c`, then
`make tests/test_meta_apply && ./tests/test_meta_apply degraded`.

## K register

No K row: a latent race found by reading and fixed before any symptom was
seen; the window is one apply. Blocks alpha: no. If the coordinator
registers it at merge, it is the twin of the Task EI GC watermark fix
and both close together.

## Not done

- Not reproduced on a cluster (the race needs the reaper's derive to
  overlap an apply that adds a mark); the unit case models the order, not
  the threads.
- No `make test-all`, no TSan (`test_meta_apply` is on no sanitizer list).
- Depends on the Task EI branch: merge EI first or together.
- `gc.h` still says a count "moves only after its batch committed"; it moves
  after the batch is staged, before the apply commits. Not changed here.
