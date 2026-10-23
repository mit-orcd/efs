# W91/W92 — server command disablement and catch-up

`EFS_COMMANDS_SERVER=0` is a **server startup** setting. Supply it to every
efsd in the metadata namespace and restart gracefully. `1` enables the feature;
unset retains enabled behavior for compatibility. Other values reject startup.
This is namespace-wide operator configuration, not a new replicated per-export
policy or live efs-mgmt toggle. Persist it in the cluster/service environment.
Mount opt-in `EFS_COMMANDS=1` is still independently required.

## Disabled behavior

The server starts no catalog refresh thread, builds/retains no query catalog or
name indexes, and skips query-specific dirty counters and transaction-fence
updates. It returns a zero GET_FEATURES mask and rejects all supported query RPC versions
with NOT_FOUND before query reply/auth allocation, ReadIndex or source pinning.
FUSE uses the server mask as well as mount opt-in to hide synthetic `.efs`.
Real user `.efs` entries still take precedence. Capability polling is cached
(default two seconds); RPC refusal is immediate on a disabled daemon even if a
client has a stale mask. Configure all nodes alike; mixed rollout is not a
cluster-wide policy transition protocol.

Normal metadata durability, Raft recovery/replication, file I/O, transaction
recovery and GC continue. Disabling commands does not suppress required storage
engine work. This removes the expensive feature-specific background work and
catalog memory, not literally every instruction or byte: small fixed state and
an inexpensive disabled tracking branch remain. No measured zero-overhead claim
is made. Already-authorized open result/source descriptors keep their immutable
client-owned bytes until close; new queries cannot be obtained. This is not a
retroactive revocation of bytes already delivered to a client.

## Two weeks on, two weeks off, then on

1. While on, the server publishes indexed metadata snapshots and updates them
   in the background using N/T scheduling.
2. Restart with the setting off. The old process/catalog is released. Files,
   directories, permissions and payload continue to change through ordinary
   persisted metadata; no query-specific catch-up queue grows while off.
3. Restart with the setting on. The query worker starts with no old catalog and
   makes a full 4,096-partition build from a pinned current applied metadata
   image, including the memtable and immutable KV segments. It recomputes
   directory rollups and builds name indexes; it does not read file payloads or
   replay two weeks of user operations into an old query index.
4. Complete-group authority, post-capture barriers, transaction/import fences
   and pending-record refusal apply before publication. Queries receive BUSY
   while no initial catalog is ready, or the accurate build/resource error if a
   build fails. No stale previous-boot index or partial successful output is
   served. Subsequent refreshes return to N/T scheduling.

Catch-up cost depends on retained metadata size (including retained tombstones),
not wall-clock weeks. More files can need more RAM/build time; normal resource
caps/deadlines still apply and failures are explicit. The default catalog cap is
256 MiB; NUC's measured configuration remains 1,024 MiB. A very large namespace
can require capacity work; turning the feature on is not a promise that any
namespace fits. Pending cross-group mutations can delay a safe initial build.

## Validation

The private four-node TCP/buffered fixture runs on → off → on against the same
stores. It checks all supported RPC versions are rejected on all disabled nodes, synthetic
commands disappear despite mount opt-in, feature masks change, normal payload
create/write/rename/unlink persists, no off-phase publication/refresh occurs,
and re-enable rebuilds every partition with exact independent count/byte oracle
and correct new/renamed/deleted search paths. Unit tests also prove disabled
writes leave dirty/fence counters unchanged and re-enable works from a full
source without counter history. Existing command, POSIX and peer gates run first.

Clean candidate identity and raw evidence:
[policy results](../../results/measure/20261008-efs-policy).
The original 400 GiB, platform/gateway and persistent remote per-export policy
acceptance remain separate open work; this change implements startup disablement.

## Operator configuration

For the NUC layout, set the exported value in
`/data1/efs/devops/clusters/nuc.env` so every daemon inherits it:

```sh
export EFS_COMMANDS_SERVER=0  # off; use 1 to enable
```

Then use the normal graceful drain/restart workflow. An environment assignment
in an unrelated shell does not alter already-running daemons. Preserve this
setting when synchronizing devops configuration. Do not disable only the FUSE
mount and expect server background work to stop. Neither setting requires mkfs,
a data wipe, rebuilding file payload or changing transport/direct-I/O mode.

Candidate `1509e6e1-policy-a9f0a842` also tests a previously open result through
server-off restart: read/seek/fstat stay consistent while the existing mount
hides commands and direct query RPCs fail. Linux GETATTR without a file handle
uses genuinely open virtual-inode size pins before the disabled feature gate.
Lookup-only identities with no open descriptor do not bypass that gate.

Final clean-build gates passed on `1509e6e1-policy-a9f0a842`: catalog units,
14 command tests, POSIX **216/0/1 (25.0 s)**, peer **64/64**, raw v1/v2 refusal
and zero capability masks on all four disabled nodes, old-descriptor
read/seek/fstat plus existing/cold mount hiding, ordinary off-phase mutations,
no off-phase publications/refresh, and full-partition on-phase rebuild with
exact independent stats and renamed/new/deleted path oracle. The test simulates
the state transitions and intervening mutations, not a literal two-week soak.

NUC rollout completed with graceful mount/daemon drains and no store reset.
Installed build: `1509e6e1-policy-a9f0a842`; previous executables remain in
`/data1/efs/evidence/commands-rollback-policy-a9f0a842`. Commands are intentionally **on**
for all four daemons, verified by capability masks and their process environment.
`nuc.env` explicitly persists the default-on server setting with shell override;
its prior config is backed up as `nuc.env.before-server-command-policy-20261008`.
Cold production stats succeeded, and the existing tiny-files screen resumed
into `/data1/efs/mnt/tiny-policy-a9f0a842`. Runtime devops configuration must be
preserved/reconciled during a later driver-side devops sync.

The [command-options build](efs-command-options-20261008.md) extends these gates
to filtered-query wire v3; all three versions are refused while server commands
are disabled. Earlier named-build evidence above covered v1/v2.

**Raw evidence note (2026-10-23):** the bulk data in `commands-rollback-policy-a9f0a842` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
