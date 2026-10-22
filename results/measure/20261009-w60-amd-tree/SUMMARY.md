# SUMMARY — results/measure/20261009-w60-amd-tree

W60 AMD mixed-tree acceptance: read-pressure gate cold-remounts a nested tree
(2000 three-byte files + four 128 MiB files) and verifies every byte.

## Purpose and linked item

- **W60** mixed-tree read acceptance on AMD.

## Verdict

**PASS** (per README.md): the expanded read-pressure gate walks and verifies
every byte of the nested tree after cold remount (details in README.md).

## Source commit and build identity

Per README.md.

## Environment

Private four-daemon AMD direct-I/O/TCP fixture `gc-direct-8k8lelm8`, ports
28840–28843; installed stores preserved; only the two approved mounts borrowed
and drained; no NUC workload touched.

## Method

Cold-remount + full tree walk + byte verification per README.md.

## Results

All bytes verified after cold remount.

## Limitations and remaining gates

Fixture scope.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
