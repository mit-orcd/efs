# ORCD capacity and remaining test failures — October 11, 2026

The ORCD deployment had no configured storage quotas. STATUS exposed only
quota and fragment usage; FUSE and management consequently reported unknown
capacity as zero. The portal also parsed only MiB/GiB, excluding this cluster's
TiB-scale capacity. Files-engine PUT accounting skipped unlimited stores.

The fix extends STATUS after its legacy 24-byte prefix with valid filesystem
capacity and available bytes. Each daemon inventories its storage filesystems
once per distinct device, including symlink/shared-root aliases. Optional quotas
bound both capacity and remaining room. FUSE and management apply the existing
distinct-node 2+1 model to those same values. Available bytes include external
filesystem consumption and metadata overhead. No mounted-export probe is
required by the portal. Older servers retain the configured-quota fallback.
A coordinated matching server/client deployment is required; old management
binaries reject extended STATUS replies. Files-engine fragment accounting now
also covers stores without quotas; startup inventory recovers existing data.

Work is isolated on `fix-orcd-capacity-15975da0`, based on devel 966ad533,
because the integration checkout contains another in-progress documentation
merge. Installed ORCD stores and `/tmp/efs-mount/001/dat01` must be preserved.

Validation: ORCD scratch evidence is under
`~/orcd/scratch/efs/orcd-test/capacity-fix-20261011/`.
The Linux build, lookup fixture, socket deadline fixture and filesystem capacity
unit check pass. Local generated-document checks pass all five checks.
The old virtual-interface harness is migrated to the current `.efs/.query`
protocol, preserving permission, snapshot, raw-name, slot and recovery oracles.
The inaccessible command-source permission test expects Python's execution
failure (exit 2) rather than command dispatch failure (exit 1).

The first private native-RDMA guarded run passed the original cold ticket
stage, but a later administrative fence returned BUSY. FINISH performs another
all-storage-member certificate check; unlike the initial storage barrier it
previously had no bounded BUSY retry. The fix pins FINISH to the observed
UUID/epoch and retries only BUSY, never ambiguous transport errors. A production
CLI regression verifies one BEGIN, unchanged epoch, bounded retry and both
cleanup stages. Phase diagnostics identify any remaining incomplete barrier.
The full native-RDMA guarded gate now passes, including the cold ticket stage,
actual session revocation, physical deletion, cold inventory and late PUT rejection.
The unlimited-quota direct/RDMA gate also passes those baseline GC oracles and
nonzero statfs checks. The peer transport proof now follows an actual read,
because mount registration alone does not establish a verbs connection.

The full `make test` target passes on ORCD when test temporary files use local
storage. NFS temporary executable cleanup was unreliable; bulk logs and evidence
remain in scratch. The deployment archive now retains tracked result references
required by documentation checks while excluding the logs directory, and uses
compression. The five local generated-document checks pass.

All ten migrated virtual gates pass. Command/source permission, command options,
pages, loaded operation, noexec, legacy names and publisher failover also pass.
The failover fixture now keeps its original overall recovery budget while
allowing EFS's 30-second I/O budget and recording unsuccessful attempts. The final
policy RPC previously used hardcoded UID 1000, which correctly fails against
ORCD owner-only directories; it now tests the real mount owner and retains an
explicit outsider denial oracle. The full corrected query/publication suite passes, including both publishers
and the outsider-denial check.

Capacity commit: `15975da0`. Pinned FINISH retry: `9b232ffd`. Fixture corrections:
`b6ebf194`. The live rollout completed on all four storage hosts and seven clients.
Daemons/FUSE are `85944ad33f2c`; a compatible management-only reporting update
is `8117b4f9` (recorded separately in `.orcd-test-mgmt-revision`). All processes
use native `mlx5_2` RDMA; daemons use direct I/O and `.efs` remains enabled.
The existing 100 GiB file retained size, owner/mode and all four sampled blocks.
No complete-file hash or hardware power-loss claim is made.

Live `df` after rollout: 122901870542848 total bytes, 293616047616 used,
122608254495232 available (human output: 112T / 274G / 112T). Values vary with
workload; used space includes filesystem overhead and external consumption.
Management now exposes exact usable bytes, so the portal does not reconstruct
large totals from rounded TiB. The portal API has positive, internally
consistent capacity/free/used values with source `server_filesystems`.
Source-version comparison retains commit/branch/dirty identity while excluding
build host/time, correcting the false mismatch from separately compiled hosts.

Live full POSIX on seven clients: 1526 PASS, 0 FAIL, 7 unsupported mmap skips.
Distributed two-client verification: four physical pairs, 264/264 PASS. Full `make test`,
private guarded native-RDMA/GC, unlimited-quota GC, ten migrated virtual gates,
query failover and cold policy recovery all pass. Deployment snapshots retain
source, documentation and cited evidence without shipping historical run logs.
 Private same-host fixture results do not prove distributed hardware
power-loss behavior. Installed ORCD stores are never reformatted.
