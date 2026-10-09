# D29 committed mapping / apply pending — AMD Oct 8, 2026

Agent 4 private source and stores: `/data1/efs/agent4-d29-20261008`, Linux user
`efs`, direct I/O/TCP, `EFS_FAULTS=1`, `EXTRA_DEFS=-Werror`, commands disabled,
pressure admission enabled. Installed services and mounts remain untouched.

- [Build](build-proof-2.log): warning-free AMD build.
- [Full unit suite](full-tests-proof-2.log): exit 0.
- [Core regression](quorum-test.log): fresh correlated quorum proof while apply
  is parked; old replies/partition/stepdown cannot authorize proof; ordinary KV
  reads remain unavailable until apply, exit 0.
- [Ownership](ownership-proof.log): apply-pending retains dirty bytes, cache pin,
  immutable request and ordered ACK ownership, exit 0.
- [Fault release](apply-pending-r4.log): mapping group 0/index 12/term 2 commits
  while all replicas park mapping apply. Query returns the same proof; altered
  query and early retirement fail. Release yields rejection with the same
  index/term, exit 0.
- [Cold replay](apply-pending-cold.log): mapping group 0/index 11/term 2 commits
  before all four private daemons are killed. Cold replay preserves its exact
  final rejection locator; retry cannot reapply effects and retirement leaves
  a refusal floor, exit 0.

Initial probes remain remotely in `apply-pending.log`, `apply-pending-r2.log`
and `apply-pending-r3.log`. The first also exposed the raw fixture's missing
leader redirect support. The next probes exposed the core's existing coupling
between quorum completion and apply, and the need to start a new eligible round
once the previous round has quorum but is waiting on apply. They failed assertions
and are not counted as passes. The first full-unit attempt failed documentation
links because new retained results were excluded from source synchronization;
restoring those files resolved the fixture setup failure.

The live intent deliberately uses an invalid FileID and performs no fragment
PUTs: command commitment can lead to rejection, so this is not publication or
fsync success. Existing real-byte staged W88 tests are recorded separately.
Production legacy REPORT migration and pressure activation remain unfinished.
