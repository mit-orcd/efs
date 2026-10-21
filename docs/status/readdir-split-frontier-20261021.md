# Readdir misses names at the directory-split frontier (2026-10-21)

**Verdict: PASS on a private node9901 cluster. The W125 C7 rerun on orcd-mini is NOTRUN.** Found while running W125 closure test C7
([W125 closure](w125-closure-20261021.md)). This is a namespace defect, not a
packing one.

## Symptom

- orcd-mini, devel `5c41111d106b`, 1,000,000 files in one directory
  (`scripts/w125_big_dir.sh`):
  - `ls -f` listed 999,990 names and a scandir pass listed 999,994;
  - `rm -rf` left 5 files behind (f0032778–80, f0032783–84). All 5 could
    be stat'ed and read.
- Focused reproducer, 500,000 files, three listings 60 s apart:
  - packing held back: 5, 8 and 0 names missing;
  - packing active: 8, 6 and 3.
- The missing names are short runs of consecutive names that move forward
  with time, for example f0004202–09 in round 1 and f0004733–40 in round 2.
  `ls` and `os.listdir` counts also disagree with each other.

## Cause

A directory with more than `EFS_DIR_SPREAD_MIN` (65,536) entries changes
layout LOCAL → SPLITTING → HASHED. While it is SPLITTING, a background
migrator moves names from the directory's own (local) shard range to their
hashed lanes. It moves them in name order. At ~530 names/min, a 500 k
directory stays SPLITTING for hours.

`efs_meta_apply_readdir` (`src/meta/meta_apply.c`) listed a SPLITTING
directory in this order:

1. hashed lanes 1–63;
2. then the local range, skipping names the hashed side already holds.

A listing of 500 k names is about 1,000 READDIR pages, and the migrator runs
between pages. Take a name that is still in the local range when its lane is
scanned. If it moves to that already-scanned lane before the local pass
reaches it, no pass lists it. That happens to the run of names at the
migration frontier during each listing, which is why the missing runs are
consecutive and advance between rounds. Packing is not involved.

## Fix

The fix is in `src/meta/meta_apply.c`; there is no wire or disk format
change. Source 0 is now always the directory's inode-shard range, which is
also the range of hashed lane 0:

| Layout | Source 0 | Sources 1–63 |
| --- | --- | --- |
| LOCAL | the whole directory | none |
| SPLITTING | the local pass, before any lane | hashed lanes |
| HASHED | lane 0 | hashed lanes |

The old separate local source (64) is gone.

Why this closes the gap:

- Names only move local → hashed. A name the local pass has not reached yet
  can therefore only land in a lane that is still ahead of the cursor.
- The same cookie stays valid if the layout flips during a listing.
  LOCAL → SPLITTING continues the same range. SPLITTING → HASHED continues
  in lane 0, where only lane-0 names remain.

The cost is the reverse window: a name the local pass listed can migrate and
be listed again by its lane. Readdir is a weak listing (spec §7.4), and the
FUSE client already drops repeated names (`readdir_collect_page`).

**Regression test:** `test_readdir_concurrent_split` in
`tests/test_meta_apply.c`. It creates 40 names, starts a split, migrates 8
names, then pages one entry at a time. It migrates 1 or 3 names after each
page, or finishes the split (→ HASHED) after page 1, 14, 27 or 40. Every
name must be listed at least once.

- Unfixed devel `287b2b3c4`: 6 failures.
- Fixed: PASS.

## Verification

**Build:** tested source is devel `287b2b3c4` plus this fix (the commit that
adds this page). On node9901 it was built as release tree
`/data1/efs/prof/bdfix/fix`: `6ffa9e785f57` plus `git diff 6ffa9e785f57
287b2b3c4` plus the fix, with `EFS_GIT_ID=bdfix287b2b3`.

**Unit tests (node9901, gcc, `make tests/<name>`):** all PASS.

- `test_meta_apply`, `test_sim`, `test_query_catalog`, `test_query_disk`,
  `test_txn`, `test_kv_lsm`;
- `tests/test_readdir_{collection,permission_authority,reply_boundary,source_cursor}.py`;
- `tests/test_query_retained_orphan.py`.

**Reproducer:** `scripts/bigdir_readdir_repro.sh REL OUT PORT [N] [ROUNDS]
[AGE_MS]`, run on node9901 in a named screen.

- It starts a private 3-daemon RDMA cluster (`scripts/w125_p0_cluster.sh`,
  `--direct-io`, quota 200 G, stores under `/tmp`).
- It creates N files in one directory with 32 threads. File sizes are
  1–4,000 B.
- Every 60 s it lists the directory with `ls -f` and with `os.listdir`.
- PASS requires every round to show `ls=N listdir=N missing=0`.
- The script stops the cluster and deletes its stores at the end. Only small
  result files are kept.

Commands (node9901, 2026-10-21, both runs at the same time):

```
fix/scripts/bigdir_readdir_repro.sh /data1/efs/prof/bdfix/fix /data1/efs/prof/bdfix/out-fix 17850 500000 6
fix/scripts/bigdir_readdir_repro.sh /data1/efs/prof/bdfix/fix /data1/efs/prof/bdfix/out-fix-pack 17860 500000 6 1000
```

| Run | Packing | Create | Rounds | ls / listdir per round | Missing | Result |
| --- | --- | --- | --- | --- | --- | --- |
| before (`5c41111d106b`, 3 rounds) | held back (1 h) | — | 3 | 499,998 / 499,997 at worst | 5, 8, 0 | FAIL |
| before (`5c41111d106b`, 3 rounds) | active | — | 3 | — | 8, 6, 3 | FAIL |
| fix, port 17850 | held back (1 h) | 500,000 in 1,179 s | 6 | 500,000 / 500,000 | 0 ×6 | **PASS** |
| fix, port 17860 | active (1 s) | 500,000 in 1,195 s | 6 | 500,000 / 500,000 | 0 ×6 | **PASS** |

The "before" rows are the original reproducer runs (`/data1/efs/prof/bdr-hold`,
`bdr-pack`, same workload, `bigdir_repro.sh` with 3 rounds). Results are in
`/data1/efs/prof/bdfix/out-fix{,-pack}/result.txt` on node9901; the clusters
and their stores were deleted by the script.

Limit: the script does not read the directory layout. That the listings ran
during SPLITTING is inferred: the split starts at 65,537 entries and migrates
at about 530 names/min, so 500 k names stay SPLITTING for hours, longer than
both runs.

## Limitations and follow-ups

- The migrator rescans the local range from its start for every name it
  moves (`efs_meta_dir_migrate_one` / `_peek`). It re-walks tombstones and
  lane-0 names each time, which is why a split runs at ~530 names/min and
  lasts hours. That is a separate performance item; this fix does not
  depend on it.
- **NOTRUN: W125 C7 rerun on orcd-mini with this fix.** Stopped at the
  user's request (2026-10-21). orcd-mini stays on `4058d81416a0`, which
  predates this fix (`f8b7a7818`), so this fix is not deployed there.
- **Design roadblock:** directories must always be spread, not co-located
  and then migrated. This fix closes the listing gap of the current
  LOCAL -> SPLITTING -> HASHED design but does not remove the migration
  window itself.
- Cleanup: the node9901 build trees are deleted. The small result files and
  patches stay in `/data1/efs/prof/bdfix`.
