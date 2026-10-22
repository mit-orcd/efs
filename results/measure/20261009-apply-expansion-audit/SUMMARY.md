# SUMMARY — results/measure/20261009-apply-expansion-audit

Fixed-capacity apply expansion audit on an AMD private fixture (pressure
enabled in the fixture only).

## Purpose and linked item

- Apply-path expansion audit with fixed capacity; fixture
  `/data1/efs/w66-gc-fixtures/gc-direct-ohu4urqa`.

## Verdict

**PASS** (per README.md): full Linux build and regression pass, no build
warnings; core/peer POSIX, physical GC effects, cold inventory/quota zero and
late-PUT fences pass; installed `978caaf7` service restored with pressure off.

## Source commit and build identity

Per README.md; installed service `978caaf7`.

## Environment

AMD private fixture, direct I/O/TCP, pressure enabled in fixture only;
installed service restored with pressure off.

## Method

Full build+regression, core/peer POSIX, GC-effect and cold-inventory/quota
checks, late-PUT fences.

## Results

All listed gates pass.

## Limitations and remaining gates

These functional gates do not certify universal memory bounds or pressure
activation; session cleanup and remaining command coverage remain open
(README.md).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
