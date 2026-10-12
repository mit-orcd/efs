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
