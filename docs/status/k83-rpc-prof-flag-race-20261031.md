# K83: `rpc_prof_enabled` lazy flag race (Task BX)

Verdict: **PASS** (K83 FIXED, blocks alpha: no). Linked item: K83 in
[the checklist](alpha-1.0-2plus1-checklist.md); found by Task BR
([batch F rehearsal](batch-f-rehearsal-20261031.md), finding 2).

## Race and fix

`rpc_prof_enabled()` (`src/client/inode_rpc.c`) cached `getenv("EFS_RPC_PROF")`
in a plain `static int` (-1 = unset) that any RPC thread could set first, so
ThreadSanitizer reported a read/write race. The fix is the W143 idiom used for
the other lazily set flags: acquire-load the static, compute on -1, release-store,
use the local copy. No new helper.

## Other flags checked (`src/client/`)

Every other lazily set env flag on devel (`write.c` `dbg`/`dcache_trace_on`,
`put_ticket_owner.c`, `ops.c`, `stage_evict.c`, `efs_fuse.c` `env_rpc_prof`/
`env_opid_trace`, `inode_rpc.c` `st_on`/`env_opid_verbose`) already uses the
atomic idiom. Left alone: `read.c` `read_verify` and `reply_buffers.h` use
`pthread_once`; `bufpool.c` init runs once under its own once; `pack_write.c`
reads `getenv` on each call (no cached state); `efs_fuse.c` `g_fuse_ready_wr` is a
pipe fd set at startup and cleared at shutdown, not a lazy flag.

## Tested

- Source: devel `bc2af2bf8` plus the `inode_rpc.c` change (this commit).
  Host xorinox (16 CPU, gcc, libtsan), build `-fsanitize=thread`, harness-chosen
  ephemeral ports (none in 17xxx or 19050-19099).
- Reproduction: `EFS_SAN_ONLY=d4_progress bash tests/sanitizer_check.sh tsan`
  (TMPDIR under `~/efs-taskBX/tmp`; `EFS_SAN_LOGDIR` keeps the reports).
- Base tree (devel without the fix), 3 tries: **PASS, PASS, PASS** (116/100/92 s).
  The race did **not** reproduce here, as expected for a once-only first-use
  race; the fix rests on the code pattern and BR's report, not on a
  before/after flip.
- Fixed tree: TSan `d4_progress` **PASS 2 of 2** (109/108 s, 0 reports).
- `make test` rc=0.
- Full gate `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`:
  `TOTAL 583 FAIL 0 NOTRUN 11 STATUS INCOMPLETE (1321s) OVERBUDGET 22`.
  INCOMPLETE is the host: the 11 NOTRUNs are RDMA (no IB device) and
  `test_cluster_stop` (no cluster scripts). OVERBUDGET 22 came from another
  task's builds sharing the host.

## Limits

Race not reproduced on the base; no live (non-sanitizer) gate beyond the above.
Cleanup: `~/efs-taskBX/src` and `tmp` removed.

## Raw location

xorinox `~/efs-artifacts/taskBX-20261031/` (base/fixed logs, TSan log dirs,
`summary.txt`, `timings.tsv`, `SHA256SUMS`); sha256 of `SHA256SUMS`:
`060e25c9d1364757d44ae0b47940aa8021d859f46513467b839af4be115e0b4f`.
