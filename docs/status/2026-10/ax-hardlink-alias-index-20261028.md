# AX — hard-link aliases: visit the aliases, not every loaded row (2026-10-29)

Branch `agent/devel-ax`, code `825667be3` plus the benchmark script commit, on
devel `74d34d588`. Gate: `make test-all` on xorinox, `EFS_TEST_JOBS=8`:
**TOTAL 553 FAIL 0 NOTRUN 10 STATUS INCOMPLETE (947 s) OVERBUDGET 1**. The ten
NOTRUN are host limits (no RDMA device for eight RDMA commands, no cluster
scripts for `test_cluster_stop.py`); the one OVERBUDGET is
`test_b_geometry.py grow-b` (131 s against 120 s). intel1 and intel2 were busy
with other tasks, so the gate ran on xorinox. Raw outputs and checksums:
xorinox `~/efs-artifacts/taskAX-20261029/`.

**Verdict: above the threshold, fixed.** A file with nlink > 1 paid a scan of
every loaded row on chmod, truncate, link and unlink, and a single-link file
paid nothing.

## Measurement

`scripts/ax_alias_bench.c` links `libefs.a`, loads ROWS unrelated files spread
over 1,000 directories, then times 100 operations each on a hard-linked file
(2 or 8 names) and on a single-link control. Mean ms per operation, xorinox,
single thread:

| loaded rows | aliases | chmod before → after | truncate | link | single-link chmod |
| --- | --- | --- | --- | --- | --- |
| 1 K | 2 | 0.0066 → 0.0002 | 0.0090 → 0.0002 | 0.0108 → 0.0003 | 0.0001 |
| 100 K | 2 | 0.866 → 0.0002 | 1.27 → 0.0002 | 1.32 → 0.0003 | 0.0002 |
| 100 K | 8 | 0.879 → 0.0006 | 1.28 → 0.0006 | 1.35 → 0.0007 | 0.0002 |
| 1 M | 2 | 12.94 → 0.0003 | 19.28 → 0.0003 | 19.50 → 0.0003 | 0.0002 |
| 1 M | 8 | 13.00 → 0.0006 | 19.32 → 0.0006 | 19.63 → 0.0006 | 0.0002 |

Before the change the cost was linear in loaded rows and independent of the
alias count; after it, linear in the alias count and independent of rows. At
1 M rows the old scan cost 13–20 ms per operation; a FUSE chmod is about 3 ms
end to end.

Unlink of an alias: 0.41 → 0.0042 ms at 100 K rows, 6.4 → 0.0043 ms at 1 M
rows (directories of 1,000 children). With all 1 M rows in one directory
unlink stays at 8–15 ms before and after: that residue is
`child_idx_del` scanning the parent's child vector, **Task AW's** scan, not an
alias cost.

An earlier attempt measured through a FUSE mount with a profiling build and
counted `ex->inode_count` per call. It was invalid: the mount's staged table
held 2 rows whatever number of files existed, so rows visited never grew. It
was dropped in favour of the library benchmark above, which loads the table
directly.

## The change

`struct efs_export` gains an alias map (`alias_keys/vals/mask`, `alias_inos`,
`alias_bytes`): ino → vector of row slots on that table, kept for hard-linked
inos only (`ino_idx` still holds one slot per ino). It is maintained at every
place rows of an existing ino are added, moved or removed:

- `efs_export_link` and `efs_export_link_dentry` reserve room first (may
  fail with NOMEM before any mutation), then push the new slot.
- `remove_inode_slot`, the only place rows are removed or moved, deletes the
  slot, repairs the vector for the swapped-in last row, and re-points
  `ino_idx` at a surviving alias without scanning. Its `expect_survivor`
  argument is gone: the vector knows.
- `export_reindex_inodes` rebuilds the vectors; `efs_export_free` frees them.
- The vectors are added to the stage-byte estimate.

Replaced scans: `sync_hardlink_attrs`, `stamp_ctime_loaded` (the table and each
shard table), `tab_set_nlink`, the nlink loops in link and unlink, the two
rollup alias walks, `efs_export_unlink`'s remove-all loop and the survivor scan
in `remove_inode_slot`. Single-link files still take the early return.

## Tests

`tests/test_hardlink_alias.c` (wired into `make test`): every name of a
hard-linked ino is looked up after each step and must show the ino's current
mode, size and nlink: four aliases, alias unlink, relocation of an alias by
removing an unrelated row, growth past capacity then a new link, reindex
(`efs_export_compact`), 200 link/unlink cycles, reduction to one row and a new
link, `efs_export_unlink` of the whole ino and `efs_export_forget_ino`;
unrelated rows are checked untouched. It also passes standalone under
ASan + UBSan with no leak report.

## Not covered, and the host result of asan-check

- Allocation failure in `alias_reserve` is not injected (no allocator fault
  hook reaches `metadata.c`); the code returns NOMEM before touching the
  table, as the existing capacity-growth failures do.
- The sharded-root cross-table case (alias rows on the parent's table)
  follows the same code as the single table; the existing hardlink POSIX
  gates in the full gate pass.
- `make asan-check` on xorinox: 49 run, **4 failed** (`d4_progress`,
  `k36_recovery`, `k36_segment_recovery`, `k37_segment_rebuild`: `join: …
  does not answer; retrying`). The identical four fail on untouched devel
  `74d34d588` on the same host the same day, so it is not this change; wave 36
  recorded 49/0 on xorinox, so the host or its load changed since. Left for
  the coordinator.
