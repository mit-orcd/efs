# SUMMARY — results/measure/20261009-catalog-index

Retained partition inode keys (`800122c5e86d`): generation-wide open-addressed
inode hash-table construction removed; each immutable inode block retains a
compact sorted 64-bit key array.

## Purpose and linked item

- Catalog index redesign (follows `20261009-catalog-blocks`): lookup routes by
  inode shard and binary-searches the block's validated strictly-increasing key
  array, mapping the local ordinal through that catalog's node_begin range.

## Verdict

**PASS** (per README.md): shared unchanged blocks keep their key arrays;
changed blocks rebuild them with validated rows/stats; extra key bytes are
fully accounted; global hash allocation/probing is gone.

## Source commit and build identity

`800122c5e86d` (README.md).

## Environment

NUC devel-find-stats line (per the catalog series).

## Method

Unit + deployment evidence per README.md; key arrays validated strictly
increasing against encoded source keys.

## Results

Hash-table construction removed; key-array lookup in place, fully accounted.

## Limitations and remaining gates

Catalog accounting charges shared blocks in full (per catalog-blocks).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
