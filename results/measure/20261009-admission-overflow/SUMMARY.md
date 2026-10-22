# SUMMARY — results/measure/20261009-admission-overflow

Admission arithmetic regression: saturated body/entry multiplication, combined
charge addition, cross-group count/body/log aggregation.

## Purpose and linked item

- Admission-accounting overflow hardening (README.md).

## Verdict

**PASS**: AMD private source tree, Linux build and full regression exit 0;
final additional host-pressure boundary tests passed separately on AMD and
macOS. Existing recovery-reserve, follower-pacing, leader-eligibility and
persistence poison checks retained.

## Source commit and build identity

AMD private source tree (README.md); compressed logs retain build and
regression output.

## Environment

AMD private tree; boundary tests also on macOS. No installed-service change and
no pressure/D25 activation in this checkpoint.

## Method

Unit/regression suite for the arithmetic (saturated multiplication, charge
addition, cross-group aggregation) plus boundary tests.

## Results

All pass.

## Limitations and remaining gates

Checkpoint does not activate pressure/D25.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  compressed build/regression logs in this dir
