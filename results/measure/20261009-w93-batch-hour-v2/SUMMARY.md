# SUMMARY — results/measure/20261009-w93-batch-hour-v2

W93 persistence-efficiency evidence: four sequential 240-second exploratory
cells on the frozen production build `39be3b38`.

## Purpose and linked item

- **W93** persistence efficiency: baseline / no-op continuation / further cells
  (per README.md); `24865a2b` adds only broader scheduler fixture coverage after
  the freeze.

## Verdict

**PASS** (per README.md): cells completed with per-cell process identity and
flags recorded; exact executable SHA-256 values in `versions.json`.

## Source commit and build identity

Frozen production build `39be3b38`; `24865a2b` = broader scheduler fixture
coverage only; `versions.json` pins executable SHA-256.

## Environment

NUC only; preserved stores, direct I/O/TCP, `.efs` disabled, receiver flow
control enabled in all cells.

## Method

Four sequential 240 s exploratory cells per README.md.

## Results

Cells complete; comparisons in README.md.

## Limitations and remaining gates

Exploratory cells; see README.md scope.

## Evidence index

- `README.md`, `versions.json` (run `shasum -a 256 <file> | cut -c1-16` here);
  cell captures in this dir
