# W125 closure plan — small-file packing

**Status:** STOPPED (2026-10-21): C1 rerun 2, C9 and C10 NOTRUN at the user's request (design roadblock: directories must always be spread, not co-located then migrated). Scope: close the packing feature under
**normal operation** (no node, network or disk failures; failure tolerance
is the separate recovery work). Builds on the
[implementation plan](small-file-packing-plan.md) (P0–P7 done) and
[design](small-file-packing-default.md). Devel carries packing since merge
`1b2c3b638`; the pre-packing tree is tag
`archive/unpacked-small-files-20261019` (`8d1542f86`).

## Where it stands

Done and verified (evidence pages under `docs/status/w125-*`):

- packing after close, packed reads, modify-unpack-repack (small scale),
  backfill (private), crash points, reclaim;
- space-pressure busy budget, segment quota reserve (full-quota deadlock
  fix), scheduled compaction (idle + daily, tiny holes and cold first),
  `efs-mgmt pack-compact`;
- copy throughput ~5,000 files/s; foreground cost ~2% under a saturating
  writer with default settings;
- read-buffer headroom fix (`d7ddcc045`): reads under heavy write load no
  longer fail ENOMEM/EBUSY;
- two-client 1 h on orcd-mini: 0 mismatches, 0 errors; mixed-size
  create/read/delete/compact rounds; dd up to 4 GiB.

Closure tests ([evidence](../status/w125-closure-20261021.md)):

- C0, C2, C3, C4, C5 and C6 PASS; C8 recorded;
- C1 and C7 failed only on defects outside packing (0a, readdir);
- fixes made here: segment quota reserve, idle compaction batches
  (`e12e23394`), grown-file fold (`f958852fd`);
- also fixed: chunk loss after eviction (`4058d8141`, client, not packing);
- not run: C1 rerun 2, C9, C10.

Set aside (not W125): network-interruption bounds on branch
`client-interruption-bounds`; degraded publication (product gaps §1.2).

## Rules for the remaining work

- Every test runs on the nodes in a named `screen` (output under
  `/data1/efs/prof/<test>-<tag>`); cross-node runs are armed node-side.
  The Mac only starts and reads.
- Correctness bar: 0 mismatches **and** 0 errors returned to applications
  (except ENOENT races that the test itself creates).
- A defect found under normal operation is fixed on devel with a unit or
  regression test, then the test that found it is rerun.
- orcd-mini may be wiped and redeployed (user, 2026-10-20).

## Tests and gates

| # | Test | Where | Script (new unless noted) | Pass |
| --- | --- | --- | --- | --- |
| C0 | 2 h big mixed test with the read-buffer fix | orcd-mini, both clients | `orcd_mini_big.sh arm` (exists) | 0 mismatches, 0 errors, dd/md5/.efs clean |
| C1 | Modify under load: overwrite, append, truncate of packed files while readers verify and the packer repacks | orcd-mini, both clients, 1 h | `w125_concurrent.py --modifiers` | readers only ever see a committed version; all versions verify at the end |
| C2 | Namespace on packed files: rename within/across dirs, hard links, delete one link, chmod/utimes | node9901 private, then orcd-mini | `w125_packed_namespace.py` | data reachable by every name, reclaim only after the last link |
| C3 | Size and read edges: 0/1/16383/16384/16385 B, grow past 16 KiB after packing, sparse small files, partial and past-EOF reads, O_DIRECT, many readers of one segment | node9901 private | `w125_packed_edges.py` | byte-exact, sizes right, no errors |
| C4 | Space accounting: `efs-mgmt status`, quota and `df` against on-disk fragments+segments through pack, compact, delete | orcd-mini | `w125_space_accounting.sh` | within one segment per node of the physical count at each step |
| C5 | Unattended compaction: churn, then idle and read-only; daily window set near now | orcd-mini | `w125_compact_unattended.sh` | holes converge to 0 without `pack-compact`; no compaction while busy |
| C6 | Backfill on upgrade: write with `archive/unpacked-small-files-20261019`, upgrade to devel | orcd-mini (wipe) | `w125_backfill_upgrade.sh` | every old small file packed and verified |
| C7 | Large directory: 1M small files in one directory, packed | orcd-mini | `w125_big_dir.sh` | readdir/stat/`.efs find`/delete correct; timings recorded |
| C8 | Packed read cost: md5 crawl over the same files packed vs unpacked | orcd-mini | `w125_read_cost.sh` | numbers recorded (no gate; decide if follow-up needed) |
| C9 | 24 h soak: churn, md5 passes, compaction, client RSS | orcd-mini | `orcd_mini_big.sh arm 24` | steady space and RSS, holes bounded, 0 errors |
| C10 | `make test`, W125 gates, final A/B against the tag | node9901 private | chain (exists) | all PASS; A/B within targets |

## Implementation expected

- Fixes for any defect C0–C9 turns up (unknown now).
- Test-tool extensions: modifier role (C1), new scripts above.
- Documentation: architecture and data-protocol pages for the pressure
  budget, segment quota reserve, compaction schedule and
  `efs-mgmt pack-compact`, read-buffer headroom; operator notes for the
  `EFS_PACK_*` and `EFS_DCACHE_READ_BYTES` tunables.
- Evidence page per test group, `docs/status/README.md`, and a final W125
  closure page with the verdict.

## Order

1. C0 now (deployed `5c41111d1`, armed in screens).
2. While C0 runs: write C1–C4; C2/C3 run on node9901 private clusters in
   parallel with C0 (different ports).
3. After C0: C1 (1 h), C4, C5 on orcd-mini.
4. C6 (needs a wipe and two builds), C7, C8.
5. C9 overnight; C10 and documentation last.
