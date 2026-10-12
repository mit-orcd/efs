# W90 expiry implementation — October 12, 2026

Implementation branch: `devel-expiry`, based on release
`3a41493591dcede820700db7d934da9dc10f07ee`. D33 governs the policy.
W90 is expiry; the duplicate storage churn benchmark is now W113. Historical
benchmark fixture/evidence path names remain unchanged.

## Current verdict

**Implementation in progress; full expiry acceptance NOTRUN.** The first
foundation adds durable operator policy, FileID/deadline revision sidecars,
an ordered disk-backed deadline index and bounded 64-row pagination. Renewal
replaces the old index entry atomically. Clock drift/backwards movement latches
uncertainty until explicit reset; destructive activation remains absent.

A conditional single-group metadata apply combines namespace unlink, ordinary
ORPHAN/REAP state and deadline/index retirement in one batch. It verifies
FileID, deadline and revision under serialized apply. Cross-group expiry is
explicitly refused by this helper and needs its own guarded transaction.
Replica apply receives committed time and does not read its own wall clock.
These primitives are not yet connected to live policy control, inheritance,
renewal RPCs or a worker. They do not activate expiry or change ordinary creates.

## Foundation verification

NUC Linux/XFS; test binaries built natively with `EFS_FAULTS=0`, four build jobs.
This is an applied-state unit fixture, not a mounted TCP/direct-I/O cluster gate.
Source is the initial W90 implementation diff on the release base; a final
committed-source rebuild and binary identity remain required before live gates.

Reproduce on Linux from this branch:

```sh
make -j4 EFS_FAULTS=0 tests/test_expiry tests/test_meta_apply
./tests/test_expiry
./tests/test_meta_apply
```

`test_expiry` PASS: default off, operator refusal, policy revision CAS, fixed
materialized deadlines, retention/owner/multiply-linked/symlink refusal,
renewal removes stale deadline keys, FileID generation isolation, paged 140-file
scan bounded at 64 rows, cold LSM reopen, clearing and clock-jump refusal.
Conditional namespace gates PASS: premature/stale deletion refused,
open-leased expiry retains zero-link inode, path replacement survives old work,
ordinary metadata consistency, and an injected batch IO failure leaves both
namespace and deadline unchanged. `test_meta_apply`: OK.

Raw build/test logs are external on NUC:
`/home/efs/devel-expiry-build.log` and
`/home/efs/devel-expiry-meta-test.log`. No raw evidence is committed.
The local macOS build cannot build the existing Linux `fopencookie` logger;
the native NUC build is the supported verification environment.

## Required live configuration and remaining gates

Use NUC with TCP, direct I/O, FILES storage, `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_PACKED_WRITES=0`; no containers or packing.
Verify actual daemon/client environments and mapped executable hashes.
Private test stores belong under `/data1/efs`; leave unrelated services alone.

Remaining implementation: authenticated-in-the-existing-client-trust-model
operator/owner control, effective directory policy inheritance atomically
materialized at CREATE, hard-link refusal including races, cross-group
conditional deletion, bounded worker with trustworthy clock checks,
rename-safe name location and metadata cleanup at ordinary reclamation.
Do not authorize deletion from a stale query catalog or a path-only lookup.

Remaining acceptance: live inheritance/authorization, renewal versus deletion,
rename/path reuse, hard links, open-FD data access, clock uncertainty/jumps,
restart/leader changes, retries, concurrent publication and offline members;
separate namespace removal and physical reclamation; bounded worker resources;
full POSIX core and peer suites. W90 stays open until these pass with exact
source/build IDs and reproducible evidence.

## First committed-source live baseline

Commit `70ba0424` (foundation, before live integration) passed on NUC with four
1-GiB-quota nodes, TCP/direct I/O, FILES default, `.efs` and packing disabled.
Stores were private under `/data1/efs/expiry-foundation-70ba0424/`, with both
policy-approved mounts. Core POSIX: **216/216, 0 failures** (21.8 seconds).
Peer POSIX: **67/67, 0 failures** (11.6 seconds). Graceful shutdown/restart,
ordinary open-unlinked reclamation, late PUT fencing, zero quotas after cleanup
and empty physical fragment inventories passed. No expiry worker was active;
this is regression evidence, not full expiry acceptance.

Reproduce from that commit on NUC:

```sh
env EFS_PACKED_WRITES=0 EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_TRANSPORT=tcp \
  python3 tests/live/gc_reclamation.py --mode direct --transport tcp \
  --disable-efs-commands \
  --root /data1/efs/expiry-foundation-70ba0424 \
  --root /data1/efs/expiry-foundation-70ba0424 \
  --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --posix --posix-jobs 4 --port 24410
```

Prerequisites: native Linux/FUSE build, existing empty unmounted mount points,
existing root parent, free ports 24410–24413 and sufficient physical space.
Harness owns unique directories/processes, retains external evidence and stops
its clients/nodes. Do not run another fixture on these mounts/ports concurrently.
Normal build command used `EFS_GIT_ID=70ba0424 EFS_GIT_BRANCH=devel-expiry
EFS_GIT_DESCRIBE=70ba0424 make -j4 EFS_FAULTS=0 all tests/test_expiry tests/test_meta_apply`.

Actual mapped daemon SHA256:
`66817489de0205029524a4c69b2b222f6d99306ffa2ed76d0cb73b3de752d408`.
FUSE: `7a9d8463cf67601eb549a8053cb33634ed698158b4752bb1844a413f4c4883c2`.
Management: `b6a019c8cdf18979685d813c57f3aba1416723ec2ad5a41243e28faef3265c90`.
External log `/home/efs/expiry-foundation-70ba0424-gates.log`, SHA256
`eca21a48be290a8e790de1bb01866bd3875feae4a1919b1c76fc983cacb034fa`.
Raw per-case and runtime evidence remains in the two private harness roots.
All four process environments and executable hashes were inspected.

## Integration checkpoint (still NOT fully qualified)

The next staged changes add `EFS_EXPIRY=1` operator control, versioned
`efs-mgmt expiry-policy` and `efs-mgmt expiry` requests, leader-selected renewal
time, and inherited birth transactions with guarded ancestry/policy snapshots.
Default remains off. These controls use the existing trusted-client management
protocol; this is not authenticated transport or protection against a forged
client UID. Policy mutation requires UID 0; renewal checks current inode owner,
generation, links, retention and live namespace state at apply.

Birth transactions store a bounded name binding in the deadline record/index,
so future worker passes need no directory scan. Conditional cross-group delete
now stages exact revision/deadline CAS and normal inode/dentry/REAP changes;
it is not connected to a worker yet. LINK has an expiry-record CAS exclusion.
The staged CREATE path uses an absent-value dentry guard and retains durable
EEXIST receipts to preserve O_EXCL/retry behavior.

Applied-state tests now also pass explicit owner renewal, policy disablement,
ancestry inheritance, atomic birth failure, and transaction intent exclusion.
Live tests of this newer integration remain NOTRUN. Remaining work includes
cross-group renewal, rename binding updates, manual deletion/reclamation
cleanup, clock-health-controlled worker activation, and the full expiry matrix.
The `EFS_EXPIRY` capability currently enables control and guarded births, not
automatic deletion. Do not mistake the first baseline for an expiry gate pass.
