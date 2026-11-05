# K38 image-base stall: forced on real daemons (Task DG)

Task DG, Nov 5 2026, node9900 and fstor007. Purpose: prove on real `efsd` daemons and a real `efs-fuse` mount that `619eceb30` (Task DE, [evidence](k38-report-stall-20261104.md)) removes the K38 `pub-stale why=image-base` stall, by forcing its trigger instead of waiting for `test_d4_plateau` to hit it (2 of 40 unfixed runs). Linked row: K38 (both tables of the [checklist](../alpha-1.0-2plus1-checklist.md)).

**Verdict: PASS for the fix on real daemons, K38 stays OPEN, blocks alpha: yes.** A new fast test, `tests/test_k38_swept_hint_live.py`, reaches the trigger in 79 of 80 rounds of the fixed tree (40 of 40 runs). Pre-fix tree, node9900: 20 of 20 runs (40 of 40 rounds) met the trigger and stalled, each round with eight `image-base` lines, `publish-stalled` and fsync `EIO`. Fixed tree: node9900 40 of 40 rounds met the trigger and published, fstor007 39 of 40 met it and 40 of 40 published, 0 `publish-stalled`, 0 `EIO`. Every triggered round of the fixed tree shows exactly one `image-base` line and then publishes, and the file reads back exact. K38 stays OPEN for the orcd-mini `ior hard` and RDMA shared-chunk reruns, which the user owes.

## How the trigger is forced

DE's six one-off attempts missed for three reasons, each confirmed by a trace on the pre-fix tree (`EFS_DCACHE_TRACE=1`):

1. **The sweep had not run.** A truncate's rows are removed by the GC thread only once the truncate is `TRUNC_SWEEP_MIN_AGE_NS` (2 s) old (`src/server/raft_host.c`). DE waited 0 or 1.5 s. Until the sweep, the row is still there and the hint matches it. The test waits until `efs-mgmt gc-status` shows `trunc_pending` above 0 and back to 0 on every node that saw it.
2. **A full-chunk write never consults the hint.** Its slot base is `EFS_CHUNK_BASE_UNCOND` (aligned last-writer-wins) and `cbase_apply` only replaces base 0. Only a partial write, whose flush finds the swept row gone, gets base 0.
3. **A sibling chunk's commit replaces the hint.** `cbase_slot` is `(ino * K + ci) >> 40`, which drops the chunk index: every chunk of an inode shares one hint slot. When chunk 1 committed on its own (two write requests, two units), `cbase_note` for chunk 1 overwrote chunk 2's hint and the next round published. In `d4_plateau` the stuck chunk shared a W77 unit with the loser, so nothing else committed.

The sequence, per round (a fresh file each), three `efsd` of the `EFS_FAULTS` build, mount A on node 1, mount B on node 2:

1. A writes three full chunks (128 KiB each) and fsyncs. The REPORT notes the hint for chunk 2 (the last noted chunk of the inode).
2. B opens the file, `ftruncate`s it to one chunk, fsyncs, closes. The FileID is kept. Wait for the sweep (above).
3. A writes chunk 1 whole plus 4 KiB at the start of chunk 2 in one `pwrite` (one W77 unit), and fsyncs.

Observed in the trace (pre-fix tree, the same up to the trigger on the fixed tree): round 1 of the REPORT loses `why=authority` on chunk 1 (its record carries A's view at content epoch 0, the inode is at 1). The repull finds chunk 2 absent and replays it without a captured view (`flags=4`, slot base 0). Round 2: `cbase_apply` replaces base 0 with the swept hint and the host answers `pub-stale why=image-base ino=… ci=2 rec_base=<hint> span=0+0 flags=4 epoch=0 want=<hint> have=0`. That is the trigger. Fixed: the round-2 repull runs `cbase_reconcile`, which retires the hint, and round 3 publishes. Pre-fix: every round sends the identical record, eight times, then `publish-stalled ... ci=1 ... cycles=8 base=ffffffffffffffff` and `publish-stalled ... ci=2 ... cycles=8 base=0 ... pub_flags=4` (the whole unit), and fsync returns `EIO`. This is DE's before-arm signature (run 20, ino 394501).

Pass, per round: fsync succeeds, no `publish-stalled`, the file reads back as the first chunk, chunk 1's new image and the 4 KiB, then zeros (nothing of the truncated first image comes back). Per run (one test invocation, two rounds): at least one round met the trigger, else "trigger not met". A round can miss it: 1 of 40 rounds on fstor007 had no STALE round at all (fsync in 0.00 s, correct and exact), most likely because A's view had caught up with the truncate's epoch before its REPORT. The run log of that round has no `report-stale` line for the inode; the server log was not kept.

## Results

| Arm | Host | Tree | Runs | Runs `rc=0` | Rounds with trigger | `image-base` lines per triggered round | Rounds with `publish-stalled` | fsync `EIO` rounds | Exact rounds | Seconds per run (median) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| before | node9900 | `42bc2f046` + test | 20 | 0 | 40 of 40 | 8 | 40 | 40 | 0 | 42.5 |
| after | node9900 | `68bfe2b9a` | 20 | 20 | 40 of 40 | 1 | 0 | 0 | 40 | 12.2 |
| after | fstor007 | `68bfe2b9a` | 20 | 19 | 39 of 40 | 1 | 0 | 0 | 40 | 12.8 |

Arms ran test revision `68bfe2b9a`, which failed a run on any round without the trigger: fstor007 run 18 failed only for that (round 1 triggered and published, round 0 was correct and exact). The committed test (`d46f7a69c`) asks for one triggered round per run; under it, and under the task's pass criterion, all 40 after-runs pass. Timing runs of `d46f7a69c` for the budget: node9900 11.7, 11.6, 13.6 s (median 11.7); fstor007 11.8, 11.6, 11.6 s (median 11.6), each 2 of 2 rounds triggered.

TSan, fstor007, `d46f7a69c` tree: `EFS_SAN_ONLY=k38_swept_hint_live make tsan-check`: `PASS k38_swept_hint_live (19s)`, `VERDICT: PASS (1 run, 0 failed, 19s)`. The entry is now in the TSan daemon list (`tests/sanitizer_check.sh`).

`tests/budgets.tsv` row: 15 s, slot 1, fast suite (`make test`, `test-wb-runtime`), route `client/write`: 1.25 × the slowest median (11.7 s, node9900) rounded up to 5 s. `python3 tests/test_make_test_recipe.py`: 102 rows, budgeted gate 1302 s of 1750 s at 8 jobs, PASS.

## Source, build, environment

- Fixed tree: devel `104eb5e10` (holds `619eceb30`; `git merge-base --is-ancestor 619eceb30 HEAD` true) with the test of `68bfe2b9a`; fstor007 built an archive of `68bfe2b9a` itself, node9900 an archive of `104eb5e10` with the test file copied in (the commit changes no source the binaries are built from). Timing runs used the test of `d46f7a69c`. Pre-fix tree: devel `42bc2f046` (wave 56, before the wave 57 merge `86c500d0b`) with the same test file copied in. `git archive` to `/tmp/efs-taskDG/{before,after}`, plain `make -j32` plus `make tests/faults/efsd`, gcc 11.5 `-O3 -march=native`, `build=unknown` (no git metadata). fstor007: `FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,…"` with a private libfuse shim directory (`lib64`, `include` symlinks to `~/orcd/scratch/efs/orcd-test/deps/libfuse`).
- sha256, node9900 pre-fix: `efsd` `87fbf1de…`, `efs-fuse` `646db5ad…`, `tests/faults/efsd` `ee0d5ec2…`; node9900 fixed: `663217a3…`, `23bac354…`, `2ad40a3c…`; fstor007 fixed: `50b37ce3…`, `9f4b6dd3…`, `421470b0…`.
- node9900: AMD EPYC 9474F (192 CPUs), Linux 5.14.0-687.39.1.el9_8, XFS `/tmp`. fstor007: AMD EPYC 9274F (96 CPUs), Linux 5.14.0-687.36.1.el9_8, XFS `/tmp`. Both: `transport auto: rdma on mlx5_0` (RDMA on loopback, TCP carries setup), buffered FUSE I/O through the page cache (no `O_DIRECT`), 1 GiB quota per store, GC on, default 128 KiB chunks. Fixture size per round: one 384 KiB file.
- Load: arms ran serially per host, in detached `screen`s. node9900 load average below 1 at the start. On fstor007 another session's `taskDI` builds (`make -j96`) and tests ran during the arm and the timing runs (load average 1.2 to 1.3 at the timing runs). No `wave57` screen on either host. Only private ports; no live cluster process was touched.

## Reproduction

```sh
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse PKG_CONFIG_PATH=$EFS_FUSE_PREFIX/lib64/pkgconfig LD_LIBRARY_PATH=$EFS_FUSE_PREFIX/lib64
make -j32 && make -j32 tests/faults/efsd
python3 tests/test_k38_swept_hint_live.py            # about 12 s; prints one line per round
# pre-fix: the same test file in an archive of 42bc2f046 (or 86c500d0b^ with 619eceb30 reverted):
# each round shows image_base=8 publish_stalled=2 fsync=errno=5; about 42 s per run (the unmount waits on the held unit)
# evidence loop: scripts/evidence/k38/swept_hint_arm.sh ARM TREE N   (N runs; summary.txt has rc and seconds per run)
EFS_SAN_ONLY=k38_swept_hint_live make tsan-check
```

`K38_SWEPT_ROUNDS` sets the rounds per run (default 2), `K38_SWEPT_KEEP=1` keeps the fixture, `EFS_DCACHE_TRACE=1` gives the client trace quoted above. A screen does not pass `LD_LIBRARY_PATH` (setgid): export it inside the screen's command or build with an rpath.

## Limits and remaining gates

- Loopback cluster on one host, RDMA on loopback; not orcd-mini, not a multi-host cluster. The orcd-mini `ior hard` and RDMA shared-chunk reruns are owed by the user and keep K38 OPEN.
- The test forces one path to the trigger (a partial write in the same unit as an `authority` loser). `d4_plateau` reached it through its own load; this test does not show that no other path exists.
- `cbase_slot` keeps one hint per inode (the shift drops the chunk index). That is why a sibling's commit hides the stall and why `d4_plateau` rarely trips it; it also means K88's hint helps only the last noted chunk of an inode. Recorded here, not changed: it is not a correctness defect (a lost hint falls back to base 0, the pre-K88 behaviour).
- `docs/status/README.md`: no change; its Current verdicts did not change (K38 stays OPEN).

Raw output is outside Git: `~/efs-artifacts/taskDG-20261105/` (shared `erbmi1` home on node9900 and fstor007), `SHA256SUMS` sha256 `ec036b1a37be1fd8cd64c10d1c6da85d5b782ef5d7ffb2e98dc61ecf33ee1073`. It holds one tarball per arm and timing set (every run log, `summary.txt`, `hashes.txt`; the pre-fix arm also has its first run's daemon and mount logs), the build logs, the TSan output, the loop script as run (`arm.sh`, the committed script with `TMPDIR=/tmp/efs-taskDG-tmp`) and `tally.py` with its per-host output.
