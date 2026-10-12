# W90 expiry implementation — October 12, 2026

Implementation branch: `devel-expiry`, based on release
`3a41493591dcede820700db7d934da9dc10f07ee`. D33 governs the policy.
W90 is expiry; the duplicate storage churn benchmark is now W113. Historical
benchmark fixture/evidence path names remain unchanged.

## Current verdict

**PASS — W90 closed for the accepted D33 initial scope.** Final committed source
`0e975936ce3c4496883fa9ee192aa20c34439853` passed both normal and fault matrices
on NUC. Historical runs below describe intermediate failures, not the final verdict. W90 now
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

## Required live configuration and acceptance scope

Use NUC with TCP, direct I/O, FILES storage, `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_PACKED_WRITES=0`; no containers or packing.
Verify actual daemon/client environments and mapped executable hashes.
Private test stores belong under `/data1/efs`; leave unrelated services alone.

Integration and both full matrices pass. Control uses the existing trusted-client
UID model, not authenticated transport. Existing-file enablement uses an explicit
name/FileID request and absent-record CAS, guarded policy/ancestry and current
owner/type/link checks. Symlink and recursive directory expiry remain deferred
by D33; W91 presentation is separate and `.efs` was disabled for these gates.
No universal TTL is selected. Explicit operator policy enables expiry.

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

## Inspection redirect fix

`539290f6` normal fixture found a real control-path error: `expiry-id` sent to
n1 could not follow group 2 because the seed hosts only group 0 and returned
NOT_PRIMARY without a target. Policy/deadline inspection had the same path.
Non-hosted expiry control now supplies a peer that hosts the target group;
the CLI follows explicit non-applied redirects using advertised addresses.
The failing FileID gate/raw log `/home/efs/expiry-539290f6-normal-gates.log`
is retained; final qualification is being rerun after the redirect fix.

The release reference advanced concurrently to `ac8cf2d7` during this work.
This branch deliberately retains the original verified release snapshot
`3a41493591dcede820700db7d934da9dc10f07ee`; no unrelated concurrent release
cleanup was merged or reverted. Reproduce by the recorded source IDs, not a
moving release reference.

## Coordinator-loss liveness fix

Normal product `0c68c032` completed core **216/216 (24.9s)**, peer
**67/67 (12.2s)** and physical cleanup. The final fault run then exposed a
real liveness failure after killing n2, the group-2 leader/first dual-group
coordinator: a single-group replacement leader repeatedly chose that dead
coordinator instead of the surviving n3. Namespace/data stayed safe but expiry
stalled. Raw `/home/efs/expiry-0c68c032-fault-gates.log`, fixture
`/data1/efs/expiry-0c68c032-fault/gc-direct-z9ikmd8x`, retained.

The worker now remembers a failed coordinator and selects another eligible
peer on its next bounded pass. Transport ambiguity is retryable only because
the request retains exactly the same FileID/deadline/revision condition;
an already committed deletion becomes stale, never a deletion of a replacement.
An explicit n2 coordinator-outage gate complements observed-leader loss.
Timeout diagnostics preserve the original failed assertion when a node is
intentionally offline instead of masking it with a gc-status connection error.
Both full matrices passed after this fix; final qualification is recorded below.

## Final qualification — PASS

Exact source: `0e975936ce3c4496883fa9ee192aa20c34439853`, branch
`devel-expiry`. Native Linux NUC builds, `-O3 -march=native`, four jobs;
normal `EFS_FAULTS=0` and fault `EFS_FAULTS=1`. Both unit executables
`tests/test_expiry` and `tests/test_meta_apply` pass on both builds.
Four private 1-GiB-quota nodes span two metadata groups on one NUC, XFS,
TCP/direct I/O, FILES storage, two FUSE mounts, `.efs` disabled, no containers,
no small-file packing. The live backlog is 144 files; this is correctness and
bounded-page validation, not a billion-file capacity or hardware-ceiling claim.

| Matrix | Core POSIX | Two-client POSIX | Expiry/recovery/physical GC |
| --- | --- | --- | --- |
| Normal | 216/216, 24.2s | 67/67, 12.0s | PASS |
| Clock fault | 216/216, 24.6s | 67/67, 12.1s | PASS |

Both cover cross-group inheritance, explicit existing-file controls/refusals,
renewal and stale deletion replay, rename/replacement, open descriptors,
publication, cold restart, global disable/re-enable catch-up, actual offline
fragment holder/rejoin, first coordinator outage and observed leader election.
Final cold inventories are empty, deletion queues drained and quotas zero.
Clock jumps/drift are unit-tested; the live fault marker simulates unhealthy
clock state without altering the host clock. Deletion pauses, remains latched
when the marker disappears, and resumes only after operator reset.
All owned processes and mounts stopped; existing user data was preserved.

Reproduce on Linux with FUSE installed and permission to mount both paths,
from the exact source above (use separate build directories for each variant):

```sh
make -j4 EFS_FAULTS=0 efsd efs-fuse efs-mgmt tests/test_expiry tests/test_meta_apply
./tests/test_expiry
./tests/test_meta_apply
env EFS_PACKED_WRITES=0 EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_TRANSPORT=tcp \
  python3 tests/live/gc_reclamation.py --expiry --mode direct --transport tcp \
  --disable-efs-commands --root /data1/efs/expiry-repro-normal \
  --root /data1/efs/expiry-repro-normal --mount /data1/efs/mnt \
  --peer-mount /data1/efs/mnt2 --posix --posix-jobs 4 --port 24410
```

For the fault variant build with `EFS_FAULTS=1`, use a fresh private root and
add `--expiry-clock-fault`. The harness checks mapped binaries and configuration,
retains manifests/logs in its generated private directories, and stops its owned
mounts/processes. Do not reuse paths or ports belonging to a running user cluster.

Raw evidence retained on NUC, outside Git:
normal `/home/efs/expiry-0e975936-normal-gates.log`, manifest
`/data1/efs/expiry-0e975936-normal/gc-direct-dqh7wp28/manifest.json`;
fault `/home/efs/expiry-0e975936-fault-gates.log`, manifest
`/data1/efs/expiry-0e975936-fault/gc-direct-sjltj44x/manifest.json`.
Build trees are `/home/efs/devel-expiry-0e975936-normal` and `-fault`.

SHA256 (normal / fault):

| Artifact | Normal | Fault |
| --- | --- | --- |
| efsd | `705fc3cfe3ff1c17980f1e7f50626adefae42efe66856ba5c5d743cebf38b8ad` | `5bc83fa3197be727f1ff8e94fc92b57c0365330f84ed5163cbc215ef42ad32d3` |
| efs-fuse | `dd9b00bf1771aede5efb681826504e0c777e06e6efae31d8a7efd8623aaa31ea` | `5ce485adfb91cc30b76e7532747740e1adbfbc5d393fbcc5da4e635424d1953f` |
| efs-mgmt | `4fa75a7425b29a1aebdcb9ce6b63b53c7b8842e87f18bece840c3e9e9dcaf6ed` | `07230f17074adfdef7de0bfe65677a8622f646f3adcaef4a83a567ec6817ac2a` |
| Gate log | `6587b6e2dd0ee6ab127a0055a6fd529353227a25ac66cebba27142af4dabf131` | `e83e0b10c1fa5842ea4bd695001e691cdb9cbf74ad696653efc819c9ea932f2c` |

## Inspection, worker status and sustained soak follow-up

In progress after initial D33 qualification: `expiry NODE:PORT show PATH
[--mount MOUNT]` provides readable local-time deadlines and relative time, saved
policy source for files, and nearest enabled/disabled future-file policy for
directories. Paths resolve authoritatively from the export root; `--mount`
strips a local mount prefix and checks its inode against the server. Endpoint
FileID/deadline changes fail inspection rather than silently showing a replacement.
Inspection across RPCs is sampled, not atomic; folder ancestry can change.

`expiry-status NODE:PORT` reports local enabled/clock/leader state, last confirmed
removal age, process-local confirmed removal count and observed logical namespace
bytes, refused/retry/error counters and bounded backlog sample age. `gc-status`
version 3 appends the same expiry observations; existing physical fragment/payload
counters still cover all deletion causes. No expiry-only physical-byte attribution
is claimed. Counters reset on restart and may omit ambiguous committed outcomes.
Do not treat their sums across replicas/restarts as a durable lifetime ledger.
Only the coordinator counts confirmed removals; forwarded workers do not double
count them. Backlog scans remain capped at 64; an old sample is not a current
exact total and a follower does not publish authoritative backlog samples.

A bounded two-mount soak is being added to `--suite expiry --soak-seconds`:
four concurrent workers create/write/fsync, renew, rename, and replace paths;
expired originals drain from namespace and physical storage, renewed and replacement
bytes must survive, survivor cleanup must reclaim storage. Cold restarts occur
between fully closed/drained batches. Space/inodes and daemon RSS are monitored.
Final verdict and exact build/evidence identities will be recorded after NUC gates.
