# SUMMARY — results/measure/20261008-publication-apply-pending

D29 committed-mapping / apply-pending acceptance on the agent-4 private AMD
fixture (faults enabled).

## Purpose and linked item

- **D29** (committed mapping / apply pending). Fixture
  `/data1/efs/evidence/agent4-d29-20261008`.

## Verdict

**PASS** (per README.md): warning-free AMD build (`build-proof-2.log`); full
unit suite exit 0 (`full-tests-proof-2.log`); core regression (`quorum-test.log`):
fresh correlated quorum proof while apply is parked — old replies / partition /
stepdown cannot authorize proof; ordinary KV continues.

## Source commit and build identity

Private agent-4 tree; built with `EFS_FAULTS=1 EXTRA_DEFS=-Werror` (README.md).

## Environment

AMD host, Linux user `efs`, direct I/O/TCP, commands disabled, pressure
admission enabled; installed services and mounts untouched.

## Method

Private fixture build + full units + targeted quorum/ownership/cold-apply logs
(`ownership-proof.log`, `apply-pending-cold.log`, `apply-pending-r4.log`).

## Results

All listed proofs pass; see README.md's linked logs.

## Limitations and remaining gates

Fault-enabled private fixture, not the installed service.

## Evidence index

- `README.md` — narrative + links (run `shasum -a 256 README.md | cut -c1-16`
  here)
- `build-proof-2.log`, `full-tests-proof-2.log`, `quorum-test.log`,
  `ownership-proof.log`, `apply-pending-cold.log`, `apply-pending-r4.log`

**Raw evidence note (2026-10-23):** the bulk data in `agent4-d29-20261008` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
