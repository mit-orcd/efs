# H-recovery — Task H fixes F26–F29 ported onto devel (2026-10-29)

**Purpose.** Port the Task H recovery fixes F26 (K36/K47 re-probe), F27 (K48
READDIR), F28 (K43 pin retention) and F29 (K36 recovery scheduling) from
`agent/task-h-repair` onto devel `126ba81d4` as branch `agent/h-recovery`,
keeping only what a retained test shows failing on devel. Inventory source:
`docs/status/task-h-wave33-reconciliation-20261029.md` on the Task H branch.
**Verdict:** F26, F28, F29 PASS (focused). F27 **not landed as code**: devel
already fixes it. Full gate: see "Gate" below.

## Source and build

- Branch `agent/h-recovery`; tested code commit `2295e1a0b` (devel `126ba81d4` + 5 commits; the gate tree also carried a `tests/budgets.tsv` row for `test_txn_recovery_priority.py` that the follow-up docs commit removes).
  Build identity is `unknown` (unstamped `make all`, gcc, `-O3`).
- Origin commits: F26 `e2e9030cd` `ece63c2a6`; F27 `236a25cb5` `9ceececc9` (test only);
  F28 `dde6564ed` `52d6f5890`; F29 `20c0a2ebe`. Task H's own tip versions of
  `test_recovery_pin_owner.py` and `test_txn_recovery_priority.py` are used.
- Devel parts kept: base K47 fix, K57/K58 fragment verification, K22/K48 truncate
  fairness, late proof mask and bounded rescan, `35a6e41b0`.
- Dropped: Task H's `txn-prepare-refused` log line (diagnostic only), forensic
  verifiers, `restart_scan`, `k36_verify_retained.py`.

## Environment

Host `nuc` (8 CPU, Rocky 10, gcc, no daemons running during the focused runs).
The tests below are source-level: they compile the production function bodies
into a fixture and run no cluster, transport or I/O mode. Fixture size is a few
fragments or records each.

## Results per fix

| Fix | Test | Prior source | With change |
|-----|------|--------------|-------------|
| F26 K36/K47 span holder | `tests/test_read_recorded_nodes.py` (runs `tests/delta_reprobe_gate.py`) | devel `read.c`: SIGABRT | PASS |
| F26 K36/K47 packed holder | `tests/test_get_integrity.py` (stale-down, async-cleared down bit, unreachable holder, deadline cases) | devel `read.c`: assertion failure in `packed_cases` | PASS |
| F28 K43 pin retention | `tests/test_recovery_pin_owner.py`, `./tests/test_pub_stage` | scheduler without the pin-only skip: SIGABRT; devel `txn.c/txn.h`: `pin_only` absent, `test_pub_stage` does not build | PASS |
| F29 K36 scheduling | `tests/test_txn_recovery_priority.py` | devel `raft_host.c`: SIGABRT | PASS (hot shard recovered inside the 64-scan cap; hot and cold budgets both served) |
| F27 K48 READDIR | `tests/test_readdir_migration_race.py` | `meta_apply.c` of `3426111b4`: SIGABRT; current devel: PASS | PASS (unchanged devel) |

Prior-source runs: tests from the branch with `EFS_GET_INTEGRITY_READ_SOURCE`,
`EFS_DELTA_READ_SOURCE`, `EFS_RAFT_HOST_SOURCE`, `EFS_META_SOURCE` pointing at
the older file, or with the devel source files swapped into a copy of the tree.

## F27 and K48

Task H's F27 changed `efs_meta_apply_readdir` to scan every lane while
SPLITTING and to claim a hashed copy unconditionally, then fence both groups in
`server_raft_host_readdir`. Devel `35a6e41b0` fixes the same hole the opposite
way: a local name stays listed until its copied lane is in the bitmap, and the
host rescans under its late proof mask. The migration test passes on devel and
fails on the earlier `3426111b4` meta code, so it guards devel's rule. Applying
Task H's change on top gave no extra failing case, so the code was not ported.
It also would have conflicted with the devel rule.

**K48 stays OPEN.** The 95 != 96 listings in `test_w130_dir_fault.py` had no
missing-name or RPC trace; this guards one interleaving only. No claim that it
explains either recurrence.

## Reproduction

```
git archive agent/h-recovery | ssh HOST 'mkdir -p ~/efs-hrecovery/fix && tar -x -C ~/efs-hrecovery/fix'
ssh HOST 'cd ~/efs-hrecovery/fix && make -j8 all && for t in test_get_integrity test_read_recorded_nodes test_readdir_migration_race test_readdir_source_cursor test_recovery_pin_owner test_txn_recovery_priority; do python3 tests/$t.py; done; ./tests/test_pub_stage'
```

Prior source: re-run a test with the matching `EFS_*_SOURCE` variable set to the
file from devel (`git show 126ba81d4:PATH`). Cleanup: `rm -rf ~/efs-hrecovery`.

## Gate

`EFS_TEST_JOBS=8 scripts/make_test_summary.sh` on nuc at `2295e1a0b`:
**TOTAL 558, FAIL 5, NOTRUN 11**, 1367 s, 26 over budget (loaded 8-CPU host).
NOTRUN: cluster scripts absent (1), valgrind memcheck.h missing (1), no RDMA
device (9). FAIL: `test_make_test_recipe.py` (the stray budgets row, removed in
the next commit; passes now) and four timing-sensitive tests
(`test_operator_workflows`, `test_d4_progress`, `test_d4_plateau`,
`test_pub_unit_forward_abort`) that each pass when re-run alone on the same
tree. None touches the changed code. Not a clean gate; no re-run of the whole
suite after the budgets fix. Raw summary stays on nuc `~/efs-hrecovery/fix/summary.txt`.

## D45 and deferrals

- F29 edits `host_rec_mark`, `host_txn_recover_pass` and the PREPARE hint in
  `raft_host.c` (recovery scheduling only). It does not reshape status
  coordination, write.c publication ownership or the buffer pool, so D45 does
  not hold it. Alpha 1.1 should still review that scheduler.
- Deferred: F27 code (superseded by devel, above).
- Not done: no live-cluster K36 gate (distinct-host recovery gate stays owed),
  no sanitizer runs of the new tests, no power-loss scope.
