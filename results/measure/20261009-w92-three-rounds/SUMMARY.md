# SUMMARY — results/measure/20261009-w92-three-rounds

W92: three additional xorinox profiling rounds on devel-find-stats (Oct 9 EDT /
Oct 10 UTC), each creating a separate retained tree with the original
tiny_files.py.

## Purpose and linked item

- **W92** profiling rounds on xorinox (files engine, direct fragment I/O, TCP,
  all three storage VMs; no wipe or push).

## Verdict

**PASS** (rounds completed): each live round creates a separate retained tree
with the original `~/git/cluster/testing/tiny_files.py` (6000 files, 4 workers,
1–512-byte payloads, max 100 files/folder, depth 3); directory shape and bytes
verified per README.md.

## Source commit and build identity

devel-find-stats (README.md).

## Environment

Xorinox, three storage VMs, files engine, direct fragment I/O, TCP.

## Method

Three live rounds with retained trees + profiling captures per README.md.

## Results

Rounds complete; trees retained and verified.

## Limitations and remaining gates

Per README.md.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  round captures in this dir
