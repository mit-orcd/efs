# SUMMARY — results/measure/20261008-publication-locators

D29/W88 durable receipt locators on the agent-4 private AMD fixture.

## Purpose and linked item

- **D29/W88** (durable receipt locators). Fixture
  `/data1/efs/evidence/agent4-d29-20261008` on amd_efs.

## Verdict

**PASS** (per README.md): warning-free rebuild (`build-2.log`); full suite
`make EFS_FAULTS=1 EXTRA_DEFS=-Werror test` exit 0 (`full-tests-2.log`);
crash/recovery (`pending-locators.log`): four private daemons — lost admission
ACK, early retirement refusal, all-daemon crash, cold pending query, altered
locator refusal.

## Source commit and build identity

Private agent-4 tree, `EFS_FAULTS=1` (README.md).

## Environment

AMD host, user `efs`, direct I/O/TCP, commands disabled, pressure admission in
the private tests; installed services untouched.

## Method

Build + full fault-enabled unit suite + crash/recovery locator scenarios per
README.md.

## Results

All locator scenarios pass per README.md.

## Limitations and remaining gates

Private fixture; W88's production integration gate remains separate (the legacy
span path).

## Evidence index

- `README.md` — narrative + links (run `shasum -a 256 README.md | cut -c1-16`
  here)
- `build-2.log`, `full-tests-2.log`, `pending-locators.log`

**Raw evidence note (2026-10-23):** the bulk data in `agent4-d29-20261008` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
