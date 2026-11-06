# K88: one CAS hint per chunk, not per inode (Task DK)

Task DK, Nov 5 2026, node9900 and fstor007. Purpose: make the K88 CAS hint (the committed full image `cbase_apply` uses as the CAS base of a slot-less record) hold one slot per (inode, chunk) instead of one per inode, and record how far K88's fix really reached. Linked row: K88 (both tables of the [checklist](../alpha-1.0-2plus1-checklist.md)); found by Task DG ([K38 acceptance](k38-acceptance-20261105.md)).

**Verdict: PASS. K88 stays FIXED, blocks alpha: no.** On real daemons, the new fast test `tests/test_k88_chunk_hints_live.py` commits half images of two chunks of one file in one REPORT, then writes both second halves. The old store left chunk 0 at `base=0`. It went STALE twice and `report-stuck` dropped it: 3 of 3 files, writeback error, file not exact. The per-chunk store publishes both chunks: 0 of 3 `report-stuck`, exact, on node9900 and fstor007. How far K88 reached: Task CK's measured shape, sequential `efs-bench` writers, was never in the gap. With the old store, node9900 had 0 `report-stuck` in 2400 runs (4 writers × 2 MiB, 16 chunks each); with the per-chunk store, also 0 in 2400 runs. The gap was a writer with two chunks of one file partly committed at once. That happens when a writer interleaves chunks (several threads on one file, or a write order that is not sequential).

## The slot bug

`src/client/write.c` before this task:

```c
return (uint32_t)((ino * 0x9E3779B97F4A7C15ULL + ci) >> 40) % CBASE_SLOTS;
```

`ci` is far below 2^40, so the shift drops it: every chunk of an inode maps to one slot. The slot stores `ci` and `cbase_apply` checks it, so a sibling's note never gave a wrong base. It replaced the hint, and the chunk that lost it fell back to base 0. That is the pre-K88 path: STALE against the committed image, an identical rebuild, `report-stuck` after two rounds, a writeback error. One REPORT notes every committed chunk in dirty-table order (`cbase_note` after a successful REPORT, and in `stale_repull_replay` for a record found committed), so only the last noted chunk kept its hint.

## The fix

```c
return (uint32_t)(((ino * 0x9E3779B97F4A7C15ULL) >> 40) + ci) % CBASE_SLOTS;
```

The slot is the inode's hash plus the chunk index. The chunks of one file take consecutive slots, so up to 4096 chunks of one file never evict each other. Two files collide only when their hashed runs overlap.

- **Memory bound:** unchanged at 4096 entries of 32 bytes (128 KiB), under `g_putid_mu`.
- **Eviction:** a new note replaces the slot's older hint. That only forgets: the record falls back to base 0, the pre-K88 behaviour. A hint matters only between a commit and the writer's next REPORT of that chunk.
- **DE's rule is kept:** `cbase_reconcile` retires only the exact hint a successful repull disproves (same inode, chunk, FileID and base).

The header comment above `CBASE_SLOTS` says all of this. Commit `13ebe3710`; no wire, store or log format change.

## Tests

- `tests/test_stale_recovery.py`, `cbase_check` (the real `cbase_slot`/`cbase_note`/`cbase_apply`/`cbase_reconcile` spliced from `write.c`):
  - Note (5,3) and (5,4) of one FileID; both apply their own base.
  - Reconcile (5,3) against an observed generation 0: (5,3) goes back to 0 and (5,4) keeps its base.
  - The first 4096 chunk indices of one inode take distinct slots.
  - With the pre-fix `write.c` (`STALE_CLASSIFIER_SOURCE` and `OWNED_RETRY_SOURCE` set to it) the case aborts at `c3.base_gen==40&&c4.base_gen==50`. The fixed tree passes on node9900 and fstor007.
- `tests/test_k88_chunk_hints_live.py` (fast suite, `test-wb-runtime`) runs the driver `tests/test_k88_chunk_hints.c` (built with the tree, in `TEST_SPECIAL_BINS`) once per file. The setup is three private production `efsd` on TCP. The driver uses the same client start as `efs-bench` and the slot-less `efs_client_write`:
  1. Write the first 64 KiB of chunks 0 and 1.
  2. One sync REPORT commits both half images.
  3. Write both second 64 KiB halves: two new full images at base 0.
  4. A second sync REPORT.
  5. Check the writeback error, then read 256 KiB back.

  The REPORT and timing are deterministic: no load and no sleeps.

| Arm | Host | Files | Driver `rc=0` | `report-stuck` lines | Stuck chunk | Exact |
| --- | --- | --- | --- | --- | --- | --- |
| before (old slot) | node9900 | 3 | 0 | 3 (1 per file) | `ci=0`, `base=0`, `stale_rounds=2` | 0 |
| after | node9900 | 3 | 3 | 0 | none | 3 |
| after (timing runs) | node9900 | 9 | 9 | 0 | none | 9 |
| after (timing runs) | fstor007 | 9 | 9 | 0 | none | 9 |

Before-arm line, for example: `report-stuck ino=4097 ci=0 gen=13af82cfcca3f086 base=0 committed=5ba20baa00c830eb span=0+0 stale_rounds=2: not re-sent, writeback error`. Chunk 1 was noted last and kept the inode's one slot; chunk 0 lost it.

DG's `tests/test_k38_swept_hint_live.py` passes on the fixed tree (node9900, 2 of 2 rounds triggered, exact, 12.6 s): DE's retirement still holds per chunk. Its docstring no longer claims one slot per inode.

TSan, node9900, fixed tree: `EFS_SAN_ONLY=k88_chunk_hints_live bash tests/sanitizer_check.sh tsan`: `PASS k88_chunk_hints_live (7s)`, `VERDICT: PASS (1 run, 0 failed, 7s)`. The entry is now in the TSan daemon list. The driver is in `TEST_SPECIAL_BINS`, so `test-bins` builds it instrumented.

Budget row `tests/budgets.tsv`: 10 s, slot 1, fast suite, route `client/write`. Solo runs at load below 2: node9900 4.1, 5.4, 3.6 s (median 4.1); fstor007 4.2, 3.4, 3.6 s (median 3.6). 1.25 × 4.1 s rounded up to 5 s. `python3 tests/test_make_test_recipe.py`: budgeted gate 1302 s of 1750 s at 8 jobs, PASS.

## How far K88 reached (`efs-bench` under REPORT contention)

`scripts/evidence/k88/bench_chunk_hints.py OUT 8 300 8M 64K 4`, on one private three-node production cluster on node9900 (TCP, 8 GiB quota per store):

- 300 rounds of 8 concurrent `efs-bench --size 8M --io-size 64K --workers 4`. Each writer writes 2 MiB (16 chunks) sequentially.
- Every 64 KiB write PUTs the chunk's image so far, and the 10 ms background REPORT commits half images mid-chunk (CK's race).
- Counted: `report-stuck` lines in every run's stderr.

| Arm | Runs | Files | `report-stuck` lines | Runs with `report-stuck` | Failed runs: BUSY stall (`rc=-13`) | Failed runs: quota full (`rc=-11`) |
| --- | --- | --- | --- | --- | --- | --- |
| before (old slot, `985ef4ac9`) | 2400 | 9600 | 0 | 0 | 85 | 318 |
| after (`13ebe3710`) | 2400 | 9600 | 0 | 0 | 76 | 329 |

A sequential writer has one chunk mid-flight. Its earlier chunks are complete and are not rewritten, so it almost never needs two hints at once, and the one slot per inode was enough. CK's measurement (2 MiB files, 3 writers, sequential) therefore stands as measured. The row's claim is narrower than it read: it covered sequential slot-less writers, not every multi-chunk file. Writers that interleave chunks were exposed until this fix. The driver above is the minimal such writer.

The failures are not K88:

- **Quota full (`rc=-11`).** The harness never deletes its files, so from round 259 (before) and round 258 (after) the stores ran out of quota. This is a harness limit, and both arms are equal in it.
- **BUSY stall (`rc=-13`).** About one round in 12 failed whole, with all 8 copies `rc=-13` after 31 to 41 s. One such run logged a degraded fragment acknowledgement for node 3. The cause is not traced. It is listed below as a follow-up, not as a K88 effect.

No failed run logged `report-stuck` or a writeback error.

## Source, build, environment

- Before tree: `985ef4ac9` (devel `104eb5e10` plus DG's branch `claude/upbeat-pascal-0163c6`, fast-forward) with the new tests and the old `src/client/write.c`. After tree: the same with the fix (committed as `13ebe3710`). fstor007 built `git archive 13ebe3710`; node9900 a copy of the working tree, identical in source to `13ebe3710` except the Makefile move of the driver from `TEST_BINS` to `TEST_SPECIAL_BINS` (made after, so that `tests/test_make_test_recipe.py` does not count it as a unit binary; the build is the same).
- Build: plain `make -j48 efsd efs-mgmt efs-fuse efs-bench tests/faults/efsd tests/test_k88_chunk_hints`, gcc 11.5 `-O3 -march=native`, `build=unknown`. sha256 prefixes, node9900 before: `efsd` `817a774e8e5abeb2`, `efs-bench` `a004aa9d79189b1a`, driver `8c392a5c68d0f924`; after: `d707f31bd154a3bb`, `eb0b2a562a684cf1`, `92db723c861cdf76` (server sources are identical; build stamps differ).
- node9900: AMD EPYC 9474F (192 CPUs), Linux 5.14.0-687.39.1.el9_8. fstor007: AMD EPYC 9274F (96 CPUs). The test and bench clusters run over TCP on loopback, with 1 GiB (test) or 8 GiB (bench) quota per store, GC on and 128 KiB chunks.
- Load: other sessions' gates (Task DJ) ran on fstor007 and at times on node9900. Timing runs were started only at a 1-minute load average below 2. node9900's first three timing runs were discarded (load 2.8 to 3.0) and rerun. Only private ports were used; no live cluster was touched.

## Reproduction

```sh
export TMPDIR=/tmp/efs-taskDK-tmp; mkdir -p "$TMPDIR"
# node9900: export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse LD_LIBRARY_PATH=$EFS_FUSE_PREFIX/lib64 (efs-fuse is only for the K38 test)
make -j32 efsd efs-mgmt efs-bench tests/test_k88_chunk_hints
python3 tests/test_stale_recovery.py                 # 0.2 s
python3 tests/test_k88_chunk_hints_live.py           # about 4 s; one line per file, PASS, report-stuck 0
# before: the same tree with src/client/write.c from 985ef4ac9; the live test fails with one report-stuck per file,
# and STALE_CLASSIFIER_SOURCE=OLD OWNED_RETRY_SOURCE=OLD python3 tests/test_stale_recovery.py aborts in cbase_check
python3 scripts/evidence/k88/bench_chunk_hints.py OUT 8 300 8M 64K 4    # about 45 min; OUT/summary.txt
EFS_SAN_ONLY=k88_chunk_hints_live bash tests/sanitizer_check.sh tsan
rm -rf /tmp/efs-taskDK /tmp/efs-taskDK-tmp
```

`K88_CHUNK_FILES` sets the files per run (default 3), `K88_CHUNK_KEEP=1` keeps the cluster directory. Runs in `screen` need `LD_LIBRARY_PATH` exported inside the screen command.

## Limits and remaining gates

- Loopback clusters on one host, TCP only; no RDMA and no orcd-mini run. The FUSE writer has dcache slots and consults the hint only when its slot base is 0; its multi-chunk shape is covered by DG's test, not measured under load here.
- The bench measurement could not show a before/after difference, because sequential `efs-bench` never hit the gap. The difference is shown by the deterministic driver, not by a load rate.
- Hash collisions between files still evict a hint (it only forgets). No load measurement of that rate was made.
- Follow-up, not K88: the periodic cluster-wide BUSY stall under 32 concurrent library writers on one three-node cluster (about 1 round in 12, 30 to 40 s, `rc=-13`). It is untraced.
- `docs/status/README.md`: no change; no current verdict moved (K88 was FIXED and stays FIXED).

Raw output stays outside Git: `~/efs-artifacts/taskDK-20261105/` (shared `erbmi1` home on node9900 and fstor007). `SHA256SUMS` has sha256 `66914c9565dce2dc55c0940d3ebe6e8e118c889ea31f89ca9d578859801f3543`. It holds one `out` tarball per host and the run scripts as run, plus `bench_chunk_hints.py`. The tarballs contain the unit, live, K38, timing and TSan logs, the build logs, and per bench arm `runs.tsv`, `summary.txt` and the kept failure logs. They also hold a first bench attempt at 24 copies × 4 writers (`bench-before-par24-busy`): it saturated the one cluster with BUSY and was stopped, so it gives no K88 data.
