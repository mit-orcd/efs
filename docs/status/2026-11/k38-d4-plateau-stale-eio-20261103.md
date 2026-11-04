# K38 signature in `test_d4_plateau`: two mechanisms (Task CV)

Task CV, Nov 3 to 4 2026, node9900. Purpose: find why `tests/test_d4_plateau.py` returned `EIO` with the K38 signature (`flush-meta ... report_rc=-14`) on devel (wave 54 gate, solo rerun 1 of 3), and fix it with proof or reopen K38 with the failing sequence. Linked rows: K38 (both tables), K61.

**Verdict.** **A for the PUT-stage `EIO`**: it is not the K38 REPORT mechanism. It is the K61 retire ring (wave 44) refusing a live PUT with `STALE`; fixed in `aabc6380c`. Rate 4 of 10 runs before (no extra settings) and 11 of 33 over all unfixed runs; 0 of 20 PUT-stage failures after. **B for a second mechanism that remains**: one fixed-build run (1 of 20) ended in a REPORT-stage stall (`publish-stalled`, `pub-stale why=image-base`, `EIO`). It is a K38-family REPORT failure that the K38 replay fixes do not cover. No product change for it; K38 goes back to OPEN, blocks alpha: yes, until it has a fix and a clean real-host run. Gate on the fix: `TOTAL 584 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (812s) OVERBUDGET 1`.

## Source, build, environment

- Tested source: devel `718372cfc` (the wave 54 merge; the wave 54 gate tree `718372cfc` is the same tree). Fix commit `aabc6380c` (`src/server/store.c`, `tests/test_ba_upload_retire.c`, regenerated `graphify-out/`), parent `718372cfc`.
- Build: `git archive` to `/tmp/efs-taskCV*`, plain `make`, gcc 11.5 `-O3`, libfuse 3.16.2 from `/data1/efs/deps/libfuse` (`PKG_CONFIG_PATH`/`LD_LIBRARY_PATH`; `EFS_FUSE_PREFIX` alone does not reach the Makefile, which uses `pkg-config fuse3`), `tests/faults/efsd` built with `make tests/faults/efsd`. `build=unknown` (no git metadata). Binary identity of the gate tree: `efsd` sha256 `39f6ec7b…f108`, `efs-fuse` `a5c2fd93…df5b`, `tests/faults/efsd` `bf365ed8…6a45` (`binary-identity.txt`). The "before" loops ran on the unpatched tree, whose clean-build hashes were not kept (the tree was then rebuilt with the diagnostic overlay below); the overlay only adds log lines.
- Host: node9900, AMD EPYC 9474F (192 CPUs), Linux 5.14.0-687.39.1.el9_8, XFS/`/tmp`. Load average 0.6 at the start of the loops; another session's `screen` was present during the gate. `transport auto: rdma on mlx5_0` (the daemons and both mounts of the fixture run over RDMA on loopback), direct I/O, three `efsd` of the `EFS_FAULTS` build, two `efs-fuse` mounts, 2 GiB quota, GC on. Only private ports (the test picks free ones); no process on 17432-17435 or the `efs` user's mount was touched.
- Test: `tests/test_d4_plateau.py`, default 75 s load, 8 threads, 128 files per writer, drain 150 s; about 100 s per run.

## Reproduction

```sh
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse PKG_CONFIG_PATH=$EFS_FUSE_PREFIX/lib64/pkgconfig LD_LIBRARY_PATH=$EFS_FUSE_PREFIX/lib64
make -j32 && make -j32 tests/faults/efsd
for i in $(seq 1 10); do export TMPDIR=/tmp/cv/run-$i; mkdir -p $TMPDIR
  EFS_D4_KEEP=1 python3 tests/test_d4_plateau.py > run-$i.log 2>&1; echo "run=$i rc=$?"; done   # in a detached screen
# a failing run prints "[Errno 5] Input/output error"; grep the kept mnt-l.log for
#   dcache-flush-fail ino=N stage=put rc=-14   (mechanism A)   or   publish-stalled / report-sync-fail rc=-1   (mechanism B)
# more signal, settings only: EFS_PUT_TICKET_DBG=1 adds "put-upload refused fi=F seq=S rc=R" on the daemons.
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh                # the gate, from the fix commit's archive
```

## Mechanism A: the PUT-stage `EIO` (fixed)

What the original logs showed (solo run `efs-d4p-lms4hqja`, mount `mnt-l.log`, UTC):

1. `02:00:11.118 dcache-flush-fail ino=86356 stage=put rc=-14`, then `02:00:11.119 flush-meta stage=report ino=86356 report_rc=-14 rc=-14` and `flush-meta: stale generation (efs_rc=-14)`. `stage=report` is only the label `efs_fuse_flush_ino` prints when `efs_append_flush_report` returned non-zero; the failing step is the PUT (`dcache_flush_ino_pass`, `efs_dcache_flush_ino`). The same for ino=470698 at `02:00:41.225`.
2. Neither failing inode has a line in any daemon log (`n1`-`n3`), no `pub-stale`, no `unowned staging observation`. The `pub-stale why=authority ... rec_base=ffffffffffffffff span=0+0 flags=261 want=c have=1` lines (inodes 329172, 531698, 524628) and `report rec ... rejected unowned staging observation (putid miss)` are a different thing and are handled: `want` is the inode's content epoch (12 truncates), `have` its generation, the record was captured before a truncate, and the client answers with `report-stale: ... replayed=N` and succeeds (the K38 replay fix working). On the fixed build they still occur ten or more times in a run (run 7 below: 11 and 9) and are not what ends a run.
3. Across the 10 baseline runs the four failing ones (3, 5, 6, 10) each have 4 to 6 `dcache-flush-fail ... stage=put rc=-14` lines and no `publish-stalled`. Run 10 has 0 `pub-stale` and 0 `unowned` lines at all and still 5 `EIO`.

What it is, proven with `EFS_PUT_TICKET_DBG=1` plus an uncommitted diagnostic overlay (`diagnostic-overlay.patch`: log lines only in `put_upload.c` and `store.c`):

1. The client sends the three fragments of a new object. In the failing PUTs the member answers `put-upload refused fi=F seq=S rc=-14` in the same millisecond as the client's `put ino=N ci=0 seq=S rc=-14` (debug run: `05:05:36.224`, ino 30669, seq 20541). The ticket status call did not fail (no `put-upload status rc=` line) and the ticket was ADMITTED.
2. The refusal comes from the store: `CVDIAG put-upload store_put rc=-14` then `CVDIAG frag-write retired_since` and, with the last overlay, `CVDIAG retired_since OVERFLOW bucket=... mark=210 now=220` (further failures: 49 to 58, 234 to 243, 38 to 48, 159 to 168, 102 to 111, 86 to 95, 288 to 297). Every failure has `now - mark` = 9 or 10 against `FRAG_RETIRE_RING` = 8, and the same `(mark, now)` in the 2 or 3 buckets of one chunk's fragments.
3. `fragment_retired_since` (`src/server/store.c`, K61, wave 44) compares the retire epoch of the fragment's bucket with the mark the PUT took before its authority read. The bucket is chosen by `(export, ino, chunk, fragment)`, not by object generation, and `server_delete_fragment_if_sum` pushes one entry per superseded generation. The D4 load rewrites a file about seven times; the packer/GC then reaps all its superseded generations of that chunk in one burst. A new PUT of the same chunk that overlaps the burst sees 9 or 10 retires, more than the ring remembers, and the code refused with `EFS_ERR_STALE` ("more than 8 refuses, fail closed").
4. The wire status for `STALE` is `EFS_PUT_CHUNK_STALE`; the client counts it as a ticket refusal (`put_drain_verdict` -> `stale_errors`, `put_fragments_parallel_once`: "never publish an object any member refused") and returns `STALE` for the whole PUT, even with two fragments acknowledged. `dcache_flush_ino_pass` returns it, `efs_fuse_flush_ino` maps it to `EIO`. The K61 page assumed the opposite ("the client treats the member as missing and rebuild repairs it"); that residual claim was wrong, so K61's "blocks alpha: no" did not hold for it.
5. This is a **second path, not the K38 mechanism** of the October pages (`k38-stale-span-replay-20261025.md`, `k38-task-g-repair-20261027.md`): those are REPORT-time STALE verdicts (stale span base, definite CAS loser, unit BUSY). K61 landed Oct 30, after them. The Task BQ signature (`report-sync-fail rc=-14`, `put-ticket: lost exchange -> NET`, `report-resolve -> EIO`) is REPORT-stage and is not shown to be this.

Fix (`aabc6380c`): `fragment_retired_since` returns BUSY when the ring overflowed (nothing is written, the identity cannot be judged) and STALE only when the ring holds a retire of this very object. `efs_put_reply_status` already maps BUSY to the retryable `EFS_PUT_CHUNK_BUSY` ("leadership and pressure are retryable with the same ticket"); the client resends the fragment with a fresh mark inside the operation budget (the K34 loop for the two-ack case). Fail-closed behaviour is unchanged. Residual: a burst of more than 8 retires in one bucket for the whole PUT retry budget would end as `BUSY`, then `EIO` (`rc=-13`); that is bounded by the budget, not observed.

Regression: `tests/test_ba_upload_retire.c` (already in `make test-long`, run mode `put`) gains `retire-ring-burst`: 9 retires of other generations of one chunk, a write under the old mark must answer BUSY, a fresh mark must write, a retire of the object itself must stay STALE. With the old STALE return it fails at the BUSY assertion; with the fix it passes (`put`, `rebuild`).

## Results

| Build | Loop | Runs | Failures | Notes |
| --- | --- | --- | --- | --- |
| devel `718372cfc` unpatched | `base` (no extra settings) | 10 | 4 (runs 3, 5, 6, 10) | all `stage=put rc=-14`, 4 to 6 inodes each |
| same, `EFS_PUT_TICKET_DBG=1`, `EFS_REPORT_DBG`, `EFS_RECEIPT_TRACE`, `EFS_OPID_TRACE` / diagnostic overlays | `dbg`, `diag`, `diag2`, `diag3` | 6 + 1 + 8 + 8 = 23 | 2 + 1 + 1 + 3 = 7 | all PUT-stage; overlays name the ring overflow |
| wave 54 gate, solo (before this task) | | 3 | 1 | same signature |
| fix `aabc6380c` (patched tree, same code), `EFS_PUT_TICKET_DBG=1` | `fix` | 12 | 0 | |
| same, logs kept on pass | `fixkeep` | 8 | 1 | run 7: mechanism B, not PUT-stage; 25 `put-upload refused ... rc=-13` lines in 4 of the 8 runs (2 to 9 each; 3 of them passed) show the BUSY path engaged and retried |

Unfixed total: 33 runs, 11 failures, all PUT-stage. Fixed: 20 runs, 0 PUT-stage failures, 1 REPORT-stage. At the unfixed rate (1 in 3) 12 consecutive clean runs have probability about 0.2 %. The fix loops used `EFS_PUT_TICKET_DBG=1` (extra log lines) while the 10-run baseline did not; the 6-run `dbg` loop with the same settings failed 2 of 6.

Gate on `aabc6380c` (`git archive`, `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, node9900): `TOTAL 584 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (812s) OVERBUDGET 1 BUILD 10s`. NOTRUN: `test_cluster_stop` (no cluster scripts), `test_rdma_memcheck`, `test_qualify_rdma_live`, `test_leak_gate` (no valgrind). OVERBUDGET: `test_pump_logging.py` 112.9 s of 95 s (assertion passed). Also run alone: `scripts/tests_for.py server/store --run` rc 0, and `tests/test_k38_install_order.py`, `test_k38_protection_now.py`, `test_k38_report_bound.py`, `test_k38_shared_chunk_pressure.py` all PASS (the last: 24 writers, 2 clients, 4800 records exact).

## Mechanism B: the REPORT-stage stall that remains (not fixed)

Fixed-build run `fixkeep` 7 (ino 428264), mount `mnt-l.log` and `n1.log`, UTC:

1. `06:14:22` to `06:15:25`: eleven `pub-stale why=authority ... flags=261 epoch=0 want=2..12 have=1` and nine `report rec ... rejected unowned staging observation ... (putid miss)` for ci 0 and 1, each answered by `report-stale: ... replayed=N`. These are the handled truncate-epoch case.
2. `06:15:28.934` to `06:15:29.020`: eight `pub-stale why=image-base ino=428264 ci=3 rec_base=90938cfcced9d4e1 span=0+0 flags=4 want=90938cfcced9d4e1 have=0`, each followed by `report-stale: chunks=4 ... replayed=4`. The record for ci 3 is an image built on base `90938c…` (no `CAPTURED_EPOCH` flag), but a truncate swept that chunk (`have=0`). `host_pub_pack_inner` accepts that only with `CAPTURED_EPOCH` (D25) and otherwise answers STALE so the client re-pulls and rebuilds; the client rebuilds the identical record every time.
3. `06:15:29.021`: `publish-stalled ino=428264 ci=0..3 ... cycles=8 base=ffffffffffffffff|0 ... pub_flags=4`, `report-sync-fail ino=428264 stage=report total_ms=3243 rc=-1`, `flush-meta stage=report ... report_rc=-1 rc=-1` and `EIO` to the application; a second fsync attempt gets `rc=-13`.

Open part: why the replay keeps the swept base for a record without a captured epoch. I did not trace the client side (`EFS_REPORT_DBG`/`EFS_DCACHE_TRACE` were not on in that run, the failure appeared in 1 of 8 runs of the keep-logs loop and in none of the 12 earlier fixed runs or 33 unfixed ones), so no fix is claimed. Before the fix the PUT-stage failures end the load op first and may mask this. Next step: loop with `EFS_REPORT_DBG=1 EFS_DCACHE_TRACE=1` on the fixed build until a few `publish-stalled` runs are kept.

## Limitations and remaining gates

- Real-host evidence is the loopback `d4_plateau` fixture on node9900, not the shared-chunk or orcd-mini `ior hard` runs the K38 rows owe; those stay owed.
- The baseline loop and the fixed loops differ by `EFS_PUT_TICKET_DBG=1`; the debug-flag unfixed runs show the same rate.
- Mechanism B rate (1 of 20 fixed runs) has a wide interval; its sequence is from one run.
- Raw logs are outside Git: `~/efs-artifacts/taskCV-20261103/` on node9900 (shared `erbmi1` home; `SHA256SUMS` sha256 `46c4638a2da1d6ed17021970c87070fee7107a28c6a3d7fab6747009c1efcda5`). It holds the original kept logs (`solo-kept`, `gate-kept`), every loop's summary and kept failing runs, `diagnostic-overlay.patch`, `product-fix.patch`, the loop scripts and the gate output. `docs/status/README.md` needs no change: its Current table verdicts did not change (the K register effort stays OPEN).
