# K30 — intermittent `make test` failures on amd

**Purpose.** Tell test races from product bugs for the three intermittent
`make test` failures on unmodified devel. Alpha 1.0 register row K30 in the
[checklist](alpha-1.0-2plus1-checklist.md); W126 for the KV case.

**Verdict: PASS.** The three failures are explained. After the change each
case failed 0 of 25 runs (`test_reclaim` 0 of 50; direct fork-oracle runs
0 of 200 per case). Fixing the first failure exposed two more test races in
the same binary (`t_phys`, `t_fail_closed`); both are fixed.
- `test_reclaim`: **product defect, fixed** (aside rename collision),
  plus two test races in later cases (physical guard; fail-closed byte flip).
- Fork oracle: **test defect, fixed** (activity sampling); no client or
  file system read was wrong.
- `test_kv_lsm`: **test race, fixed**. The product limitation behind it
  (W126) is real, not small, and now pinned by a deterministic test case.

Full suite: see [Full suite](#full-suite). Branch `devel-k30`
(worktree branch `claude/devel-k30-4ace7f`), not merged, not pushed.

| Case | Before (`bf16436e8`) | Cause | Class | After (`cadb3b563`) |
|---|---|---|---|---|
| `tests/test_reclaim` "old meta/ copy fails closed" | 12/25 FAIL | `gc-journal.broken.<seconds>` already exists (non-empty) from a failure in the same second; `rename` fails (`ENOTEMPTY`), init returns `EFS_ERR_IO` | product | 0/50 |
| `tests/test_direct_read_fork_oracle.py` fork / fork+COW | 10/25 FAIL | `read_active_forks=0`: one sample after `fork()` misses microsecond page-cache reads | test | 0/25 |
| `tests/test_kv_lsm` `test_peek_beside_reader` | 3/25 FAIL | compactor's first `lsm_lock` runs after the reader parks, holds `mu` queued on `view_rw`; peeks wait on `mu` | test race over an open product limitation (W126) | 0/25 |

## 1. `test_reclaim`: journal aside collides within one second

**Signature.** Every one of the 12 failing runs printed "journal
verification FAILED" exactly three times, then
`test_reclaim.c:118: boot: Assertion 'server_reclaim_init(&S)==EFS_OK'` in
"old meta/ copy fails closed". The three FAILED lines are expected: three
scenarios corrupt the journal on purpose (`t_fail_closed`,
`t_state_missing`, `t_old_meta`). The copy is not the problem. The test
copies `meta/` after `server_reclaim_shutdown` (quiesced, with no fds open),
and verification correctly rejects it: the state record outside `meta/`
expects segments that the old copy lacks. Failing closed is the intended
outcome of this case, and it already happens before the assertion.

**Cause.** On verification failure, `server_reclaim_init`
(`src/server/reclaim.c`) renames `meta/gc-journal` to
`meta/gc-journal.broken.<time(NULL)>`. If that name already exists as a
non-empty directory, `rename(2)` fails with `ENOTEMPTY`, and init returns
`EFS_ERR_IO`. `efsd.c` then refuses startup ("cannot initialize reclaim
journal"). This contradicts the design comment there: "A damaged journal
fails closed, not startup." A kept failing tree showed
`gc-journal.broken.1792736918` and `.1792736920` with the third failure in
second `…920` again. In production the same thing happens when two
verification failures land within one second, for example a fast restart
loop or a restored `meta/` that still carries an aside from that second.

**Change.** Probe `<seconds>`, then `<seconds>.1`, `.2`, … (up to 1000)
while `rename` reports `EEXIST`/`ENOTEMPTY`. Any other error still returns
`EFS_ERR_IO`, so the outcome is still fail-closed. Earlier asides are kept.

**Regression test.** Before booting, `t_old_meta` creates non-empty asides
for this second and the next two, then checks that the earlier aside is
kept. The new test against the old `reclaim.c` failed 5 of 5. With the fix
it passes (rates in the table).

**Second race, uncovered by the fix.** In about half the baseline runs the
binary stopped at the old-meta case. With that case fixed, one of 15 runs on
`5b41c1eda` failed later, in "physical guard":
`test_reclaim.c:446: t_phys: Assertion 'put(30000,1,1)==EFS_ERR_QUOTA'`.
`t_phys` forces `phys_avail` to one fragment's worth and sets
`last_sample_us` to hold the sample. But `bg_main` re-samples
(`server_phys_sample`: `statvfs`, then a store under `S.lock`) whenever
500 ms have passed. A re-sample that the thread decided on before the test's
store overwrites the forced budget with the real free space, so the second
PUT is admitted. This is test-only: production never forces `phys_avail`.
Change: `t_phys` calls `server_reclaim_shutdown` right after `boot()`. PUT
admission needs none of the reclaim threads, and `run()`'s later shutdown is
then a no-op.

**Third race, seen in the full suite.** One full `make_test_summary.sh` run
on `edf1451c7` failed at `test_reclaim.c:282: t_fail_closed: Assertion
'S.reclaim_fail_closed'`, after `segment 4 torn tail cut at 0 records`. The
case corrupts the newest segment by writing the constant `0xff` at offset
20 (inside the random journal epoch) and at the first record. When the
epoch byte already is `0xff`, which happens 1 time in 256, the header stays
valid. The damaged last record then correctly reads as a torn tail, so
nothing fails closed. The kept tree had `0000000000000004.seg` cut to the
64-byte header. Change: read each byte and write its complement.

## 2. Fork oracle: the activity check, not the bytes

**Signature.** All 10 failures printed `files=N expected=N` (every byte and
every EOF verified) with `read_active_forks=0`. Seven were the
`prefill=1 rounds=2 read_repeats=100` COW case (`files=6400`). One was the
`rounds=100` fork+COW case and two were the `rounds=100` fork case without
COW (`files=3200`). A first version of the fix changed only COW mode; the
no-COW case then failed 1 of 25 with the same signature, so the final
change covers both modes.

**Cause.** The oracle runs against a local page-cache fixture in a temporary
directory. No EFS mount, daemon or client read path is involved, so the
product returned nothing wrong. A run lasts about 0.1 s and makes about
16–19 forks. After each fork, the forker samples `active_reads` exactly once
and requires at least one hit. A 64 KiB page-cache `read` lasts a few
microseconds, and right after a multi-threaded `fork()` the workers are
mostly in COW faults or in the verification loop. Direct reproduction:
5 of 10 runs reported `read_active_forks=0`.

**Change** (`tests/live/efs_direct_read_fork.c`). After `fork()`, in both
fork modes, poll `active_reads` for up to 2 ms. In COW mode the held child still shares the
mm for that whole window, so a hit there is a read during COW sharing,
which is the overlap the case targets. The comment still says sampling
proves activity, not exact kernel page-pin overlap. Byte, EOF, fork-count
and prefix-write checks are unchanged.

**After.** 200 direct runs each: `0 4 4 100 1` fork, `0 4 4 100 1` fork+COW,
`1 4 4 2 100` fork+COW. 0 FAIL; minimum `read_active_forks` 9, 10 and 16
per run. The Python test passed 25 of 25.

## 3. `test_kv_lsm`: peek liveness and W126

**Signature.** 3/25: `test_kv_lsm.c:1057: peeks return while a reader holds
the view`.

**Cause.** The test parks a reader inside a scan callback (holding
`view_rw` shared), then requires the `lsm_peek_lock` peeks to return. Peeks
take `mu` only. However, `lsm_lock` takes `mu` and then `wrlock(view_rw)`.
If the compactor thread (started by `efs_kv_lsm_open`) reaches its first
`lsm_lock` only after the reader has parked, it holds `mu` while queued
behind the reader, and every peek waits on `mu` until the reader leaves.
K26 saw the same thing under memcheck. So the failure is a scheduling race
in the test, over a real product limitation. In efsd, a peek such as
REPORT's `efs_kv_lsm_l0_hot` waits for the longest in-flight reader
whenever any writer (Raft apply, flush, compactor) is queued on `view_rw`.

**Why the ordering fix is not small.** A writer must not hold `mu` while it
waits for readers. Every way to arrange that changes the KV lock protocol:
- Reversing the order (`view_rw` then `mu`) deadlocks against
  `lsm_view_yield`/`lsm_view_reclaim` (W126 step 3: the flush keeps `mu`
  and retakes `view_rw`). It also deadlocks against `lsm_cond_wait`, which
  returns holding `mu` and then takes `view_rw`.
- Taking `mu`, `trywrlock`, and backing off to wait for readers without
  `mu` removes the writer preference. That preference exists so Raft apply
  cannot starve behind a stream of reads (`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`).
  Back-off also lets two writers livelock.
- Moving the peeked fields off `mu` (atomics, seqlock or a separate lock)
  touches every writer of the stats, flags and revision arrays.

Each of these needs its own design and a concurrency gate
(`test_kv_lsm_concurrent`, the orcd-mini A/B). That is W126 step 4
territory, not a K30 patch.

**Change** (`tests/test_kv_lsm.c`, test only; no silent skip in `make test`).
`test_peek_beside_reader` now runs twice:
- run 0 stops the compactor (`kv_compactor_stop`) before parking the
  reader, so no writer exists, and asserts that the peeks return within 5 s;
- run 1 starts a writer (`lsm_lock`) after the reader parks and waits until
  that writer holds `mu`. Once taken, `mu` cannot be released until the
  reader leaves, so this is deterministic. It then asserts that the peeks
  have *not* returned after 200 ms ("W126 known limitation"). Both runs
  then check that the peeks complete once the reader is released.

When W126 is fixed, run 1 must flip to `done == 1`.
`make leak-check` still sets `EFS_TEST_KV_SKIP_PEEK_LIVENESS`. Both runs are
now deterministic, so that skip can probably be dropped, but this was not
tried under memcheck here.

**After.** 25/25 PASS.

## Full suite

`scripts/make_test_summary.sh` on `cadb3b563` (per command, continuing past
failures): `TOTAL 1039 FAIL 10 NOTRUN 5 STATUS FAIL (393s)`. No K30 case
failed. `grep -E '^make(\[[0-9]+\])?: .*Error'` over the build logs, the
runner output and every kept failure log matched nothing. The 10 FAILs:

- `tests/test_client_processes.py`: "client discovery requires root"
  (W156, expected on amd).
- `tests/test_declaration_gate`, `tests/test_rdma_xprt` and the seven
  `tests/test_rdma_segmented` modes: `server_accept` /
  `efs_rdma_client_upgrade` fail on amd's soft-RoCE `rxe0` (on `wlp194s0`).
  The same two binaries built from unmodified `bf16436e8` failed 3 of 3 and
  1 of 1 standalone at the same time, so this is host state, not this
  change. It is not investigated here, and nothing in K30 touches RDMA.
  The K30 discovery run listed only four FAILs, so these passed or were
  NOTRUN there. Follow-up for whoever owns the amd RDMA setup.

NOTRUN (5): the `make_test_summary` runner self-test marker, cluster stop
scripts absent, async durability (TMPDIR not XFS), the `EFS_FAULTS`
compactor stall, and the leak gate (no valgrind on amd).

An earlier full run on `edf1451c7` had the same 10 plus `test_reclaim`
(`t_fail_closed`, fixed in `cadb3b563`; see section 1).

## Environment and reproduction

- Host amd: 32 CPU, 119 GB RAM, Ubuntu 26.04.1 LTS, kernel 7.0.0-38-generic,
  gcc 15.2.0, Python 3.14.4. Not exclusive: the user's live EFS cluster
  (17432–17434) and another session's strace-wrapped private cluster
  (ports 32432–32434) were running; load average 1.5–2.5. No private daemon
  was started; these tests need none (unit binaries and a local fixture,
  buffered I/O, no transport).
- Sources: `git archive <commit>` into `~/efs-k30/src`, `make -j32`,
  `TMPDIR=$HOME/efs-k30/tmp`. Build ID is `unknown` (no `.git` in an
  archive tree, and the build time is embedded, so hashes are per build).
  Before: `bf16436e8`. Intermediate: `5b41c1eda` (first 25-run pass, which
  found `t_phys`); `edf1451c7` (full suite found `t_fail_closed`). After:
  `cadb3b563`, `efsd` sha256
  `08cb17f442c038f1648ff342bf03bb18e7f87858de4f8c3a43521158d3327404`.
- Loop (25 sequential runs each, 50 for `test_reclaim` after the change;
  failing outputs kept outside Git):

```bash
cd ~/efs-k30/src && export TMPDIR=$HOME/efs-k30/tmp
make -j32 && make tests/test_reclaim tests/test_kv_lsm
for i in $(seq 50); do ./tests/test_reclaim >/dev/null 2>&1 || echo FAIL; done
for i in $(seq 25); do ./tests/test_kv_lsm >/dev/null 2>&1 || echo FAIL; done
for i in $(seq 25); do python3 tests/test_direct_read_fork_oracle.py >/dev/null 2>&1 || echo FAIL; done
```

- Direct oracle runs use the Python test's 32-file fixture and
  `EFS_DIAG_FORK=1 [EFS_DIAG_COW=1] read-fork FIXTURE <args>`. Look at the
  `read_active_forks=` value in the printed line.
- Raw logs were not kept; `~/efs-k30` on amd was deleted after the run.

**Limitations.** These are rates from 25 runs on one shared host, not
probabilities. The KV fix makes the test deterministic but does not fix
W126. NOTRUN: other hosts, sanitizer builds, `make leak-check` (no valgrind on
amd), and the RDMA tests (host `rxe0` broken during this session).
