# K90: the unmount drain after a refused write (Task DF)

**Verdict: PASS, K90 FIXED, blocks alpha: no.** The writing mount's close in `test_b6_unavailable.py` full mode went from 30.0 s (harness kill, `rc=-9`) to 0.2 to 0.3 s (`rc=0`), three of three runs on node9900. With the member still full, the unmount ends in 6.6 s, exit 1, naming the given-up chunks. Gate on fstor007: `TOTAL 589 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (648s) OVERBUDGET 1 BUILD 13s`.

Purpose: Task DF, trace and fix K90 from [gate-profile-20261104.md](gate-profile-20261104.md). Tested source: `cf35d1e95` on devel `42bc2f046` (`efs-fuse` and `efsd` built from a `git archive` of it, so the build id reads `unknown`). Later commits on the branch change this page, the K rows, a test docstring and the graph only.

Environment: node9900 (192 cpus, libfuse `/data1/efs/deps/libfuse`), fstor007 (96 cpus, private libfuse shim); private loopback clusters of three members under `/tmp/efs-taskDF-tmp`, EFS_FAULTS `efsd` where the test uses it, transport as the test picks it, chunk 128 KiB, node 3's quota 16 MiB (B6) or 4 MiB (new test).

## What was held

Measured on a pre-fix tree with a diagnostic line in `report_units_shape` (not committed), B6 full, node 3 already restarted with 1 GiB at the unmount:

| File | Unit | ci 0 and 1 | ci 2 (the 37856 B tail of 300000 B) | Writers |
|---|---|---|---|---|
| W0, W1, W2 (ino 1048577, 1052673, 1056769) | W77 span 0-2 | landed objects, unreported, record carried | `dirty=1 data=1 object=0`: its PUT was refused and nothing retried it | 0 |

The units were dcache chunks, not write-back pool jobs: nothing was queued in `g_wb`, and `g_wb.err` plays no part. The `Q*` files were refused too but form no unit (one chunk), so they never held a REPORT.

## Cause

1. `write` buffered the 300000 B in the dcache and returned. The FSYNC and the FLUSH each got `ENOSPC` (`dcache-flush-fail ... rc=-11`), which reached the application. The chunks stayed dirty.
2. Node 3 got room back. A whole-cache flush (`efs_dcache_flush_all`, reclaim mode) PUT the two full chunks, and skipped the tail: a chunk with no base is skipped so two reclaim flushes cannot overwrite each other's sparse appends.
3. `efs_fuse_destroy` retried the REPORT. Each run held the unit back whole (`EFS_ERR_AGAIN`); `unit_wait_idle` returned at once; `efs_dcache_flush_all` returned OK in 1 ms, again skipping the tail; after 16 rounds `report_dirty_ino_complete` mapped `AGAIN` to `BUSY`.
4. `efs_fuse_destroy` reads `BUSY` as a shard rebuild and retries every 200 ms (the 215 ms cadence, 138 `report-sync-fail ... stage=report-retry rc=-13` lines in 30 s), until the 60 s bound. No `data flush incomplete` line fired: the skip counts as success.

So the name K90 carried was wrong in one respect: the hold did not need a full member. It needed a refused tail and nobody to retry it, and the client could not tell, because `BUSY` said only that a unit was held. With the member still full the old code exited early when a full chunk's PUT answered QUOTA, and dropped the tail without naming it.

## Fix

- `efs_fuse_destroy` (`src/client/efs_fuse/main.c`): every drain attempt first PUTs all dirty chunks through `efs_dcache_drain_pending`, which flushes new partial chunks (the path `efs_control_drain` already used), then REPORTs. Room back: the tail lands, the unit publishes, exit 0.
- A fresh `EFS_ERR_QUOTA` from the drain or the REPORT ends the loop. `efs_dcache_discard_dirty` names each dirty range and stops retrying it, a last REPORT publishes what is complete, `efs_dcache_pending_records` names the rest (`force-discard ino= ci= off= len= cause=unmount-enospc`), and the stop is unclean: exit 1, stop record `ENOSPC: refused chunks given up`.
- `efs_dcache_drain_pending` is now one pass over every dirty inode, not a page of 64: a caller that sees OK has nothing dirty left. QUOTA ends the pass and `efs_dcache_flush_all`, since `put_fragments` has already placed around every full member it could.
- A transient failure still retries within the 60 s bound, now with the PUT retried too. No timeout, sleep or retry delay was added or widened. The decision comes from a fresh answer, not a remembered refusal, so a member that freed room is used.

Where the error goes: the application got `ENOSPC` at its FSYNC or FLUSH, as before and asserted by the new test. At unmount no caller is left, so the record is the exit status, the stop record and the `force-discard` lines. The rule is in `src/client/efs_fuse/README.md`; `writeback.h` is unchanged because `g_wb` did not change.

## Results

| Check | Before | After (`cf35d1e95`) |
|---|---|---|
| `EFS_B6_MODES=full`, `close mnt-a`, node9900 | 30.0 s `rc=-9` (DA, and 138 retry lines in 30 s here) | 0.2, 0.2, 0.3 s `rc=0` |
| B6 full wall time, node9900 | 212.4 s | 186.1, 185.9, 186.0 s, PASS |
| B6 full row, fstor007 gate | 216.9 s (wave 56) | 185.5 s |
| `test_k90_unmount_enospc.py`, room back | `TimeoutExpired` at 30 s (pre-fix binary, the two new log asserts removed) | exit 0 in 0.2 s, file exact |
| same, member still full | exit 1, tail not named | exit 1 in 6.6 s, tail named |
| `test_k22_snapshot_import.py`, node9900, once | 30 s close in 2 of 2 runs (DA) | PASS, `close mnt-a` 0.2 s `rc=0`, 159.8 s |

- `make tsan-check` (default list), node9900: `VERDICT: PASS (22 run, 0 failed, 541s)`. The new test under TSan, in the same instrumented build through `EFS_SAN_LIST`: `PASS k90_unmount (27s)`.
- Gate on fstor007, `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`: `TOTAL 589 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (648s) OVERBUDGET 1 BUILD 13s`. NOTRUN: `test_cluster_stop` (cluster scripts absent), `test_rdma_memcheck` (no `memcheck.h`), `test_qualify_rdma_live`, the usual three. OVERBUDGET: `test_member_limit` 36.9 s of 20 s (13.3 s in wave 56); it runs daemons only, no client unmount, so this change does not touch it.
- New test: 19.9 s solo on node9900, 18.3 s in the gate; row `client/efs_fuse.main`, target `test`, 30 s ceiling.

## Reproduction

```bash
make -j32 test-bins tests/faults/efsd
python3 tests/test_k90_unmount_enospc.py
EFS_B6_MODES=full python3 tests/test_b6_unavailable.py
python3 tests/test_k22_snapshot_import.py
make tsan-check
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
```

The `close mnt-a` times come from a one-line timing print in `Mount.close` of the measured copy of `tests/k33_daemons.py` (not committed). For the before row, run the new test on `42bc2f046` without its two asserts on the new log lines. Clean up by unmounting anything left under `TMPDIR` and removing that directory.

## Limits

- k22's `EIO` close did not occur in the one run here (no `EIO` in its output), so the k22 case is covered by inference: same drain, same path. One run is not an odds measure against DA's 2 of 2.
- The refused file can now appear after room comes back and the unmount publishes it. The application was told `ENOSPC`; POSIX leaves such data unspecified, and a live mount's later flush could already publish it.
- An unmount with new partial chunks dirty and members unreachable used to exit at once and lose them silently; it now retries for the 60 s bound, then says `UNMOUNT DATA LOSS`. A writer-owned dirty chunk also answers BUSY for the bound instead of one attempt. Neither showed in the gate's timings.
- A writer-owned chunk given up on `ENOSPC` is named, not retired through its writer state; the state goes with the process. Not exercised by the new test.
- `efs_control_drain` keeps its design: it refuses with the mount retained at its own deadline, and QUOTA does not end it early.
- A full member and many dirty chunks with a base are decided by design at the first QUOTA (one PUT, about 6.5 s); only the tail case was measured. Space freed after that decision is not used.

## Artifacts

`~/efs-artifacts/taskDF-20261104/` on the ORCD hosts (NFS home): `repro-diag/` (pre-fix log with the held-unit lines, pre-fix test output), `verify-cf35d1e95/` (B6 and k22 output), `tsan-check.out`, `tsan-k90.out`, `gate-fstor007-cf35d1e95/` (summary, timings). `SHA256SUMS` sha256 `0a6afc37f3323c43b8d2f381e0595a46eb23192a121ae6910bd0a0dc7fa042ae`. Reproduction does not depend on them.
