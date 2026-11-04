# K88: a chunk lost to the writer's own earlier REPORT (Task CK)

Task CK, Nov 1 2026, intel1 (8 CPU, XFS, TCP). Purpose: fix K88 from [bench-published-readindex-20261031.md](../2026-10/bench-published-readindex-20261031.md) (a cold FUSE read returns the right length and wrong bytes while `efs-bench` printed `BENCH_OK`). Gate: F1 (`tests/test_bench_published.py`) and the K register. Verdict: **PASS (K88 FIXED, blocks alpha: no)**; remaining acceptance below.

Tested source: devel `9c4abd8cb` (wave 49, with CF's K86 fix) plus Task CI's K87 (`acd241816`) and Task CJ's page (`952c273eb`) plus the Task CK change in `src/client/write.c` and `src/client/efs_bench.c` (this commit). Build: plain `make`, gcc -O3 (`EFS_BUILD_ID` unknown) on intel1.

## The sequence that leaves the final REPORT at `base=0` (proven)

CJ's page left the cause unproven and guessed a dcache ordering bug. Traced runs on intel1 (`EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1`, 24 copies, 3 of 3 failures read) show a different one:

1. `efs-bench` writes through `client_write_run` (`src/client/write.c`), which for every 64 KiB `efs_client_write` assembles the whole 128 KiB chunk, PUTs it as a **full image** and records it with `putid_note(..., seq=0, base=0)`. It never creates a dcache slot: every `dcache_note_committed` line in the trace says `slot=0`, so `dcache_base_gen_of(ino, ci, 0)` has nothing to read and the record's `base_gen` is the fallback `0` ("the row is absent") for every chunk.
2. A chunk is therefore PUT twice (first 64 KiB, then the whole chunk). When the 10 ms background flush (`meta_flush_main`, `sync=0`) REPORTs the first image (`putid-fill gen=A seq=0 dlen=0 putbase=0`) before the writer has PUT the second, the host commits `A` against an absent row.
3. The writer's final sync REPORT carries the second image `B` (also `base=0`). The host's row is now `A`, so it answers STALE (`stale-class committed=A ours=B done=0 base=0`). The acknowledgement of the first REPORT finds no slot, so no base moves.
4. The STALE replay (`dcache_replay_stale_ex` -> `replay_span_without_body`) returns OK without changing anything for a full image that has no slot body; the rebuilt record is again `B`, `base=0`. After two identical STALE rounds `report_rec_stuck` logs `report-stuck ... not re-sent, writeback error` and drops the chunk. The REPORT returns OK, so `efs-bench` printed `BENCH_OK`.

Trace lines (`dbg-base24c/r1-17.log`, ino 7945, ci 3): `putid-fill gen=dd1d... dlen=0 putbase=0` in the async REPORT, `putid-fill gen=e0c4... putbase=0 recbase=0` in the sync one, `stale-class committed=dd1d... ours=e0c4... done=0 base=0` twice, then `report-stuck`. The same shape in `r29-22` (ino 7305 ci 2) and `r30-17` (ino 5793 ci 3), and in the earlier `dbg-base24b` run (`slot=0` on every `note-committed`).

## The fix

`src/client/write.c`: a small direct-mapped table (`g_cbase`, 4096 entries, under `g_putid_mu`) remembers, per chunk and FileID, the full image the host committed (`cbase_note`, called where a REPORT acknowledges its records and where the STALE classifier finds a record already committed). `cbase_apply` runs in both record builders (first build and STALE rebuild) after `report_fill_owned`: a full image still at `base_gen == 0` takes the remembered committed generation as its CAS base. The second image is a superset of the first by the same writer, so it replaces it; a collision only forgets (the old behaviour); a truncate or recreate changes the FileID and the entry is ignored; `UNCOND` and span records are untouched. The STALE round after a lost race then rebuilds with `base=A`, the host accepts, and `report_rec_stuck` no longer matches (`base_gen == committed`).

Limit: a peer that rewrites the same chunk between the writer's two commits still makes the second REPORT STALE; for a slot-less full image that stays a fail-closed `report-stuck` error (now surfaced, below).

## Error surfacing

FUSE already returns EIO: `efs_file_open` opens an `efs_wb_description` and `fsync`/`flush` call `efs_wb_description_sync` (`efs_file_sync_error`, `src/client/efs_fuse.c`); nothing changed there. `efs-bench` did not: it called `efs_client_report_dirty_ino(ino, 1)` and never read the recorded error. `src/client/efs_bench.c` now opens an `efs_wb_description` per data worker (and per metadata create-with-payload) before writing and fails the worker (`EFS_ERR_IO`) when `efs_wb_description_sync` reports an error after the final REPORT.

## Results

Fixture and tool: `tests/test_bench_published.py`, `scripts/evidence/measurement/bench_published_par.sh OUT COPIES ROUNDS` (each copy with its own `TMPDIR`), intel1 load average 25 to 50 at 24 copies. A failure is a non-zero exit; every failure below had `report-stuck`.

| Build | Copies | Runs | Failures (`report-stuck`) | Notes |
|---|---|---|---|---|
| before (unfixed code, trace on) | 24 | 960 + 1440 + 960 | 2 + 7 + 3 (12 of 3360, 0.36 %) | three traced runs |
| before, bench error surfacing only | 24 | 1440 | 4 (all `BENCH_FAIL kind=write rc=-1`, no `BENCH_OK`) | the writer now fails instead of exiting 0 |
| after | 24 | 240 + 240 | 0 + 0 | the two acceptance runs |
| after | 8 | 80 | 0 | |
| after | 24 | 2400 | 0 | load 50; about 8 failures expected at the old rate |

In the "before, error surfacing only" run the same loss that previously ended in a cold-read `AssertionError` now fails at the first `bench(...)` call, with `BENCH_FAIL` and the `report-stuck` line in the log.

Gate (`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, intel1): `TOTAL 583 FAIL 0 NOTRUN 12 STATUS INCOMPLETE (833s) OVERBUDGET 2 BUILD 0s`; the NOTRUN rows are RDMA, valgrind memcheck and the absent cluster scripts; OVERBUDGET `test_k38_protection_now` (54 s of 30) and `test_pump_logging` (111 s of 95). The first run (`TOTAL 583 FAIL 2`) failed `test_stale_recovery` and `test_k38_stale_span_replay` because those harnesses splice `stale_repull_replay` out of `write.c` and had no adapter for the new `cbase_note`; both now stub it and the gate above is the rerun with no other change.

TSan lane for the touched client write path (`EFS_SAN_ONLY="data_commit d4_progress" bash tests/sanitizer_check.sh tsan`): `VERDICT: PASS (2 run, 0 failed, 104s)`.

## Reproduction

Prerequisites: a clone at the tested commit, FUSE usable, 8 CPUs, `screen`; private ports only (the test picks free ports; never 17432 to 17435).

```sh
export TMPDIR=$HOME/efs-taskCK/tmp; mkdir -p "$TMPDIR"
make -j8 efsd efs-fuse efs-bench efs-mgmt
sh scripts/evidence/measurement/bench_published_par.sh OUT/par24 24 10     # about 2 min; expect 0 failures after the fix
sh scripts/evidence/measurement/bench_published_par.sh OUT/par24l 24 100   # about 15 min
# trace variant: add env["EFS_DCACHE_TRACE"]="1"; env["EFS_REPORT_DBG"]="1" after the
# env['EFS_TRANSPORT'] line of a copy of the test; before the fix about 0.3 % of runs show
# `putid-fill ... putbase=0`, `stale-class ... done=0 base=0` twice and `report-stuck`.
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > gate.log 2>&1   # about 15 min
```

Checks: `summary.txt` shows `fail 0` and `report-stuck logs 0`. Cleanup: `rm -rf "$TMPDIR"`.

## Limitations and remaining gates

- Only intel1 and TCP; RDMA and a FUSE writer under the 24-copy load were not run (the FUSE path is unchanged and already returns EIO).
- The interleaving itself is only reproduced under load, but `cbase_check()` in `tests/test_stale_recovery.py` (Task CM) now deterministically drives the real `cbase_slot`/`cbase_note`/`cbase_apply`: same FileID takes the committed base, a different FileID or equal generation stays 0, a slot collision only forgets.
- K88 acceptance on the frozen candidate: `test_bench_published` in F1 and one more loaded run.

## Artifacts

Raw output stays outside Git: intel1 `~/efs-artifacts/taskCK-20261101/` (`dbg-base24*`, `surface-nofix24`, `after*`, trace logs); `SHA256SUMS` in that directory has sha256 `1dd7033ed91e6c0debfacdae462043a7094e092c00480eb50084e99103e81e8b`. Gate summary: intel1 `~/efs-taskCK/gate.log` (and `gate-summary.txt` in the artifact directory).
