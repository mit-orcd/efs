# SUMMARY — results/measure/20261009-catalog-blocks

Compact immutable inode blocks (`a62cbd5f6ca2`) deployed on devel-find-stats,
NUC direct I/O/TCP, .efs enabled.

## Purpose and linked item

- Catalog storage redesign: inode row/stat values live in reference-counted
  immutable partition blocks; regular files no longer carry empty rollups.

## Verdict

**PASS** (per README.md): implemented and deployed on devel-find-stats; reuse
requires the identical retained inode partition and identical retained
partitions for all active lane dependencies; a changed dependency rebuilds the
affected block conservatively using authoritative getattr; shared blocks are
charged in full to each catalog's accounting — no cap bypass.

## Source commit and build identity

`a62cbd5f6ca2` on devel-find-stats (README.md).

## Environment

NUC, direct I/O/TCP, `.efs` enabled.

## Method

Deploy + catalog gates per README.md; raw partitions and edge folds retain
their existing dependency/lifetime rules.

## Results

Deployed and passing per README.md; each catalog owns its small global pointer
vector/index and fresh directory-only rollup/link/child-count records.

## Limitations and remaining gates

Shared-block accounting charges in full per catalog (documented design choice).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
