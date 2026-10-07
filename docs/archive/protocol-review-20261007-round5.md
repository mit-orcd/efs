# Protocol/documentation review — Oct 7, round 5

[Queue](../status/README.md) · [Coverage](../status/spec-implementation.md)

Review started 15:29:08 UTC; checkpoint deadline 15:44:08 UTC. Source baseline
was HEAD `ddc677ae4bf351a7f73ead7384ee7cccfccc844e` plus existing working
changes. No production source edits, commits, SSH, installations or cluster
mutations were performed. Two isolated C review fixtures were compiled and
executed locally; both intentionally fail their safety assertions. This is
not a passing release suite or a live-cluster reproduction.

## Findings and evidence limits

| ID | Finding | Evidence and remaining gate |
| --- | --- | --- |
| [W82](../backlog/work-items.md#w82) | The host can reuse an old completed quorum index for a later read after isolation. | Shared-core fixture: old leader retains A and coverage after the new majority commits B. Host-path conclusion is source analysis; RPC/FUSE partition acceptance remains owed. |
| [W83](../backlog/work-items.md#w83) | Completed transaction decisions have no established safe retirement path. | Source tracing finds decision get/put, while drop/resolve scan participant records. Durable acknowledgement/retirement and restart gates remain owed; no disk-growth measurement. |
| [W84](../backlog/work-items.md#w84) | Namespace predicate collection has eight-record/participant limits. | Source analysis of ancestry and empty hashed-directory guards; deeper ancestry or more used lanes can hit BUSY without progress. Complete predicates and bounded admission need reconciliation; no live reproduction. |
| [W85](../backlog/work-items.md#w85) | Direct-mapped PUT hint eviction loses “already tried” history. | Sequential helper collision reproduces NEW on an ambiguous retry. Duplicate placement/quota drift are downstream source concerns requiring server/store tests. |

These are indexed findings awaiting triage, not approved redesigns or a new
execution order. They do not identify the cause of the reported 40 GiB GC
incident. Their implementation and acceptance work remain open.

## Retained isolated reproductions

The [stale-read patch](round5-stale-read-repro.patch) transforms the existing
`tests/test_raft.c` harness into a focused core fixture. Isolate node 0 in both
message directions after it commits A; elect a new majority leader and commit
B; tick the old leader 40 times. The old leader still reports completed
coverage. [Output](round5-stale-read-output.txt) records the failing assertion;
the run was repeated with the same result.

Apply the patch to a temporary copy of `tests/test_raft.c`, not the production
test file. Compile that copy with `src/raft/raft.c` and `src/raft/raft_mem.c`:

```sh
cc -std=gnu11 -Iinclude /tmp/stale-read-repro.c src/raft/raft.c src/raft/raft_mem.c -o /tmp/stale-read-repro
/tmp/stale-read-repro
```

Expected review result is exit 1, not PASS. The existing partition test starts
a fresh read round; `src/sim/sim_raft.c:read_begin_g` does too. They do not test
the production host's cached-coverage admission. Simulator/shared-core success
must not be described as production host/RPC acceptance.

The [PUT-hint fixture](round5-path-hint-repro.c) retains the inspected helper
and uses colliding inode keys 1 and 4097. After first sending A, sending B
evicts A's history; retrying A after a lost reply returns NEW instead of safe
probe 0. [Output](round5-path-hint-output.txt) records the result.

```sh
cc -std=gnu11 -Wall -Wextra -Werror -Iinclude docs/archive/round5-path-hint-repro.c -o /tmp/path-hint-repro
/tmp/path-hint-repro
```

Expected review result is exit 1. Server handling maps NEW to SKIP, bypasses
fragment-root lookup and can charge a first write; those consequences were
read in source, not reproduced against storage or a live cluster. Sequential
eviction already suffices to invalidate the retry-history claim; concurrency
is an additional gate.

SHA-256 fingerprints of inspected fixture dependencies (working-copy content):

| File | SHA-256 |
| --- | --- |
| `src/raft/raft.c` | `910f6a84e4bf244922806ba415b0eac37258b65ed4eaffb8befcd3e186bd7515` |
| `src/raft/raft_mem.c` | `b3c7606640810db77e8f8115b44ccf3429e2d7fdfae0152421bdd7ff3c3820d2` |
| `tests/test_raft.c` | `dddbcb5e8cd2cc5f3e89016283447a44022f0fb1201cc465fd93f65a5400bbb2` |
| `src/client/write.c` | `19a077236c317699fa083d6ee0716f2b6b7a971d103bfd5a6cc3afddd158c213` |

## Documentation and earlier contracts

The architecture, transaction, directory, session and verification guides now
link their implementation limits instead of implying blanket acceptance.
D7's accepted retry contract links W85; an accepted contract is not revoked by
an implementation defect. D2 open-row adoption, D4 snapshot bounds/fallback,
D5 sliced import, D17 present-count/local maximum and D18 cold staging-table
retirement received source spot checks. These checks are not new hardware,
failover or exhaustive contract acceptance.

Former comparison and naming paragraphs remain intact in
[comparison wording](comparison-wording-before-round5-20261007.md) and
[naming wording](naming-wording-before-round5-20261007.md). Current prose
separates product documentation from novelty, benchmark and legal conclusions.
Primary sources consulted on Oct 7:

- [Raft paper, §8](https://raft.github.io/raft.pdf): read-only authority needs
  a current-term entry and a majority leadership check; index coverage alone
  does not establish fresh authority for the next request.
- [DAOS 3.0 filesystem guide](https://docs.daos.io/master/user/filesystem/):
  version-specific filesystem support limits.
- [WEKA container architecture](https://docs.weka.io/weka-system-overview/weka-containers-architecture-overview.md):
  documented container roles; no unverified placement/POSIX comparison.
- [VAST whitepaper](https://www.vastdata.com/whitepaper) and
  [CephFS metadata cache](https://docs.ceph.com/en/latest/cephfs/mdcache/):
  distinguish architecture roles without inventing dedicated-node requirements.
- [GIGA+ project](https://www.pdl.cmu.edu/PDSI/gigaplus/index.html) and
  [IndexFS paper](https://www.pdl.cmu.edu/PDL-FTP/FS/IndexFS-SC14.pdf):
  attribution for partitioned namespace work, not proof of EFS novelty.
- [FoundationDB testing](https://apple.github.io/foundationdb/testing.html):
  simulation complements live/hardware testing; EFS's independent checker
  requirement is its own acceptance obligation.
- [XtreemFS](https://www.xtreemfs.org/) and
  [Amazon EFS guide](https://docs.aws.amazon.com/efs/latest/ug/): existing
  product names, not search dominance or legal naming clearance.

## Checkpoint estimate

Approximately **10% ±5 percentage points** of this documentation reconciliation
remains: deeper checks of earlier contract edge cases and retained acceptance
references. This is a judgement of document coverage, not a count of files,
percentage of code proven correct, or implementation completion. The active
queue retains every unresolved finding; historical contracts/evidence were
preserved rather than discarded. Documentation validation results are recorded
in the round-5 user-facing report.
