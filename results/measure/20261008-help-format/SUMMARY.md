# SUMMARY — results/measure/20261008-help-format

Benchmark CLI help-format gates: ownership, parsing and scratch protection.

## Purpose and linked item

- Benchmark CLI hardening (help/ownership/parsing). Build
  `c22754236b38-help-abf6838b` (build-id.txt); source delta in
  `source-working.patch`; binary hash `help-binary.sha256`.

## Verdict

**PASS**: help-cli-tests.log — "benchmark CLI: ownership, parsing and scratch
protection PASS"; final build + smoke + production deploy logs retained
(help-final-build.log, help-final-smoke.log, help-production-deploy.log);
production stats in help-production-stats.json.

## Source commit and build identity

`c22754236b38-help-abf6838b` (build-id.txt).

## Environment

Production deploy on the shared host per help-production-deploy.log; stats in
help-production-stats.json.

## Method

CLI test suite (help-cli-tests.log), final build, smoke, production deploy.

## Results

All help-format gates PASS.

## Limitations and remaining gates

CLI surface only; no durability gates in this run.

## Evidence index

- `help-cli-tests.log`, `help-final-build.log`, `help-final-smoke.log`,
  `help-production-deploy.log`, `help-production-stats.json`,
  `build-id.txt`, `help-binary.sha256`, `source-working.patch` (run
  `shasum -a 256 <file> | cut -c1-16` here)
