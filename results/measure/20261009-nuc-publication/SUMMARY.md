# SUMMARY — results/measure/20261009-nuc-publication

NUC publication and bounded cleanup evidence on a private agent-4 fixture
(faults enabled, commands disabled, pressure admission on).

## Purpose and linked item

- Publication + bounded sweep acceptance: baseline `863054ca` plus the bounded
  sweep change and deterministic manifest-test corruption fix.

## Verdict

**PASS** (per README.md): merged baseline build and complete passing suite
(`build-integrated.log`, `full-tests-integrated-2.log`); the earlier
`full-tests-integrated.log` retains the pre-fix failure for history.

## Source commit and build identity

Baseline `863054ca` + bounded sweep change; built with `EFS_FAULTS=1
EXTRA_DEFS=-Werror` (README.md).

## Environment

Private source/stores `/data1/efs/evidence/agent4-publication-20261008` on nuc_efs;
test policy `EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_ADMISSION_PRESSURE=1
EFS_TRANSPORT=tcp`.

## Method

Merged build + full suite per README.md links.

## Results

Full suite passes on the merged baseline.

## Limitations and remaining gates

Private fixture; policy flags as listed.

## Evidence index

- `README.md`, `build-integrated.log`, `full-tests-integrated-2.log`,
  `full-tests-integrated.log` (pre-fix failure, retained) (run
  `shasum -a 256 <file> | cut -c1-16` here)

**Raw evidence note (2026-10-23):** the bulk data in `agent4-publication-20261008` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
