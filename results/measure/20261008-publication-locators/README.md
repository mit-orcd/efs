# D29/W88 durable receipt locators — AMD, Oct 8, 2026

Agent 4 private fixture: `/data1/efs/agent4-d29-20261008` on `amd_efs`, Linux user
`efs`. Direct I/O/TCP; `EFS_FAULTS=1`, `EXTRA_DEFS=-Werror`, commands disabled,
pressure admission enabled in the private tests. Installed services are untouched.

- Build (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-publication-locators/build-2.log`): warning-free rebuild.
- Full suite (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-publication-locators/full-tests-2.log`): `make EFS_FAULTS=1 EXTRA_DEFS=-Werror test`, exit 0.
- Crash/recovery (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-publication-locators/pending-locators.log`): four private daemons; lost admission
  ACK, early retirement refusal, all-daemon crash, cold pending query, altered
  retry refusal, terminal rejection and replay floor, exit 0.

The first full-suite attempt failed architecture link checks because results
were excluded from the private source sync. Restoring the referenced results
resolved that fixture setup failure; the retained second run passes. No source
failure was hidden by this correction.

This validates applied receipt locators. It does not validate mapping committed
while apply is pending or enable production REPORT/session integration.
