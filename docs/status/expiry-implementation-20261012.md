# W90 expiry implementation — October 12, 2026

Implementation branch: `devel-expiry`, based on release
`3a41493591dcede820700db7d934da9dc10f07ee`. D33 governs the policy.
W90 is expiry; the duplicate storage churn benchmark is now W113. Historical
benchmark fixture/evidence path names remain unchanged.

## Current verdict

**Implementation in progress; full expiry acceptance not yet passed.** W90 now
has operator policy controls, guarded inherited CREATE deadlines, cross-group
owner renewal and conditional deletion, rename-safe bindings, hard-link
exclusion, and ordinary REAP cleanup of deadline metadata. `EFS_EXPIRY` defaults
off independently of `.efs`.

The active worker uses a disk-backed deadline index: at most 64 rows per page,
eight deletion attempts per pass, and a 50-ms elapsed check between operations.
A single transaction may exceed that interval while awaiting its existing RPC
and durability deadlines; this is not a hard 50-ms wall-time guarantee.
Kernel `adjtimex` synchronization state and wall/monotonic drift gate deletion.
Uncertainty latches until explicit operator reset. Replicas apply committed
conditional namespace decisions and never independently compare local clocks.

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

Integration is implemented; qualification and fault coverage remain in progress.
Control uses the existing trusted-client UID model, not authenticated transport.
Existing-file enablement now uses an explicit name/FileID request and an
absent-record CAS, guarded effective policy/ancestry, current owner/type/link
checks and the same durable index transaction as renewal. Qualification of
this additional control is pending.

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

## Live integration checkpoint (not final acceptance)

A dirty integration build on NUC passed fixed/future-only policy, authorization,
owner renewal and stale revision refusal, active hard-link refusal, rename/path
reuse, disabled-subtree inheritance, namespace removal, physical reclamation,
and continued read/write/fsync through an expired open descriptor. Core POSIX
passed 216/216 in 24.6 seconds. Peer and final cleanup verdicts are still pending.
Raw evidence: `/home/efs/expiry-integration-gates.log`; fixture
`/data1/efs/expiry-integration/gc-direct-u31m8j3x`. This build reports `unknown`
and is preliminary evidence only, not a committed-source acceptance identity.

The committed `--expiry` harness extends the lifecycle matrix with cold restart,
concurrent publication, offline storage/rejoin and 144-file deadline pagination.
`--expiry-clock-fault` additionally requires a fault-enabled daemon and verifies
latched clock uncertainty plus explicit operator recovery, without changing the
host clock. Exact committed-source rebuild and rerun remain required.

## Extended committed-build diagnostic

Commit `31e1cbcb`, fault-enabled native NUC build, passed concurrent publication,
cold restart and offline-storage debt/rejoin. The clock-fault gate failed to
observe all nodes as uncertain: followers did not sweep and health reads only
returned their last sample. The fix makes health inspection sample the kernel
freshly even on followers; deletion already checks fresh health independently.
A real operator `efs-mgmt expiry-clock NODE:PORT get|reset` is added.
Raw diagnostic `/home/efs/expiry-31e1cbcb-gates.log`; fixture
`/data1/efs/expiry-31e1cbcb/gc-direct-pchr9uwb`. Full acceptance remains pending.

Clearing allows ordinary hard links. Renewal now guards the recorded name
binding and refuses reactivation after that name is removed. A cleared record
whose name no longer matches does not block ordinary rename; focused unit and
live regressions cover this lifetime. Pre-commit birth image contention returns
BUSY only after confirmed abort, preserving the client's operation identity.

## First complete fault-build run

Commit `54b193b3` (`EFS_FAULTS=1`) PASS: inherited/default-off deadlines,
authorization, renewal/revision refusal, cleared-link lifetime, rename/reuse,
retention/clear refusal, open-FD reads/writes/fsync, concurrent publication,
cold restart, actual group-0 leader loss/election, offline storage debt/rejoin,
clock uncertainty latch/reset and 144-file bounded-page backlog. Core POSIX
**216/216, 0 fail/skip/notrun (24.6s)**; peer **67/67, 0 fail (12.7s)**.
Ordinary physical-GC gates, cold empty fragment inventory and zero quotas pass.
Four nodes, 1-GiB quota each, native NUC Linux/XFS/SATA, TCP/direct/FILES,
`.efs` and packing disabled; isolated stores and permitted mounts.
Daemons during final POSIX used roughly 11–19 MiB RSS; this observation is not
an independent global RSS ceiling. Worker page/attempt bounds are explicit.

Reproduce from this commit using the build/configuration below and add
`--expiry-clock-fault` with a fault-enabled build. Raw log
`/home/efs/expiry-54b193b3-gates.log`, SHA256
`f05db272e6d9d5d6bd7795a4fc3fe1d66ef53a08ed292da5e5fc5dbc9f23bded`.
Fixture `/data1/efs/expiry-54b193b3/gc-direct-haqk6fan` plus sibling node3/4
root `gc-direct-1lyow2ty`. The harness stopped all owned clients/nodes.
Binary SHA256: daemon `63d8a3e1809c94657965608c50f8759086bde2a77327ea231d2ef883dbed0fd2`;
FUSE `6d6524125e74be1b0d3711afdfa3fda52967c38033fc8dd1582ccc6b79e065bd`;
management `a945e51cd220c7bad1562447f656618a7d99aef9128c2b9a50ad2fb53e367cfb`.

Normal commit `a5ac58a7` also passed explicit stale-delete replay, replacement
preservation and expiry disable/re-enable catch-up. Its leader-loss harness
assumed the seed hosted the target group and failed on group 2 (seed only
hosts group 0); the harness now finds an actual leader among all four nodes.
This is not recorded as a complete passing run. Raw
`/home/efs/expiry-a5ac58a7-gates.log` retained. New explicit existing-file
control and the corrected full matrix must pass before final qualification.

## Final matrix fixture corrections

Normal `9e292f84` passed explicit enablement on existing files (owner, generation,
link-count, retention, effective-policy and symlink refusals), deletion replay,
replacement survival, publication, cold restart and global disable/re-enable.
Its offline gate stopped fixed n4, which was not a fragment holder for that
particular file; physical fragments could legitimately be reclaimed. The gate
now chooses an observed fragment holder and records its actual fragment paths.
Leader loss similarly chooses an observed leader from all four nodes. These
fixture failures are retained in `/home/efs/expiry-9e292f84-gates.log`; they are
not called full passes. The final script also forces inherited policy across
both metadata groups and hashes the companion lifecycle script in its manifest.

## Corrected normal matrix

Commit `d471b901`, native `EFS_FAULTS=0`, PASS: the full lifecycle matrix,
including deterministic cross-group policy inheritance/deletion, explicit
existing-file enablement/refusals, stale-delete replay, replacement survival,
global disable/re-enable catch-up, observed fragment-holder outage/rejoin and
observed metadata-leader loss. Core **216/216 (24.7s)**, peer **67/67 (12.3s)**,
physical-GC gates, final cold empty inventory and zero quotas all pass.
Raw `/home/efs/expiry-d471b901-normal-gates.log`; exact hashes will be captured
with the final candidate evidence. The fixture stops owned processes/mounts.

Operator inspection is being finalized: `expiry-id NODE:PORT INO` returns the
authoritative generation, UID, mode and link count for policy/deadline controls.
Policy/deadline reads reject a stale generation instead of presenting absence
as a disabled policy/deadline. The final source will rerun normal and clock-fault
matrices; W90 closure is reserved for those results.
