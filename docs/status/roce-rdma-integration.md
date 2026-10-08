# RoCE RDMA integration branch

Branch `devel-roce-rdma` starts at devel `cd0cc4c5` plus a separate snapshot
of the working changes (`eeb1f73a`). The original devel checkout is unchanged.
Keep that snapshot separate from transport commits when merging; shared fixes
should return to devel and later refresh this branch from devel. Do not copy
old metadata/storage implementations from the backup.

The archive's source copies contain the existing native-IB transport, not the
RoCE/GPU source referenced in its successful-run notes. RoCE GID addressing
was implemented in the existing transport. Native IB sends the legacy 20/24
byte setup/reply, while RoCE uses a version-1 GID extension. Strict rdma mode
requires successful upgrade. TCP remains the bootstrap and intentional
oversized-frame side channel; this is not an all-packets-on-RDMA protocol.
The optional HIP probe recreates the archived mapped-host-memory experiment,
with verbs registration and a verified GPU kernel; it does not claim GPU-direct
DMA. Archived DMA-BUF registration returned EOPNOTSUPP on RXE.

AMD initially has no verbs device. Build succeeds; RDMA and GPU-registration
probes skip and are NOT acceptance. Root must load ib_uverbs/rdma_rxe and
create rxe0 on wlp194s0. Persist the modules with modules-load.d and recreate
the device with a network-online systemd oneshot. efs has unlimited memlock.
After setup, inspect nonzero GIDs and choose EFS_RDMA_GID_INDEX explicitly if
needed. This is same-host Soft-RoCE correctness, not hardware offload or
cross-host performance acceptance.

Run tests/live/rdma_posix_gates.sh on AMD from /home/efs/efs-rdma/src.
It uses private stores and ports 27432–27435, two private mounts, direct I/O,
and retained evidence. EFS_TRANSPORT=tcp provides a same-code baseline;
EFS_TRANSPORT=rdma requires a non-skipped loopback test and transport-up logs
from all daemons and both FUSE clients. The installed TCP cluster is retained.
The known Python link-of-symlink test now explicitly requests no-follow.

Open: live RDMA/GPU probes, full RDMA POSIX and peer gates, native IB legacy
compatibility, mixed-version failure handling, connection fault/retirement
stress and measured resource bounds. Successful compilation is not closure.

TCP baseline on AMD: full POSIX 216/0/1 skip in 17.9 seconds; peer 64/64
in 7.3 seconds. Evidence: /home/efs/efs-rdma/gate-tcp-NLpu87.
Root setup script: tests/live/amd-enable-soft-roce.sh.

## Oct 7 AMD Soft-RoCE acceptance

Root setup completed: rxe0 ACTIVE on wlp194s0, GID index 1 is IPv4-mapped,
uverbs available and boot service enabled (reboot itself not tested).
RXE max_cqe is 32767; shared CQ allocation now respects queried limits and
reserves receive capacity per connection with teardown headroom. Failed CQ
creation releases its channel. A freed setup-reply pointer in optional
first-frame diagnostics was also replaced with a saved QP number.

Transport probe passes 67 RDMA frames and one intentional oversized TCP frame.
HIP registered mapped pinned host buffer + verified GPU transformation passes.
Full direct-I/O RDMA POSIX passes 216/0/1 skip in 18.7 seconds; POSIX2 passes
64/64 in 8.0 seconds. All four daemon and both FUSE logs confirm RDMA upgrade.
Same-host software RoCE is slower here than the prior TCP baseline (17.9/7.3
seconds); this is one functional run, not a transport performance conclusion.
Evidence: [retained results](../../results/measure/20261007-amd-rdma/posix.tsv).
Remote full logs: /home/efs/efs-rdma/gate-rdma-5jt0h6.

Cleanup observed client.sh returning an identification error after the first
mount/process had already disappeared; verified detachment, stopped the peer
cleanly, then stopped only the private daemons. Installed cluster unaffected.
Runner now rechecks actual mount state after a failed stop. Source hashes
identify this test tree: initial rsync build had unknown Git metadata, so it
must not be represented as an exact clean committed-binary acceptance.

Still open: native-IB/mixed-version gates, fault/retirement stress, sustained
CQ capacity and RSS bounds, reboot verification and cross-host hardware RoCE.
GPU-direct DMA remains unsupported/unvalidated.

## Oct 7 installed AMD cluster switched to RDMA

Reused the existing four nodes and both mounts under /home/efs/efs, deployed
the devel-roce-rdma source, and restarted with EFS_TRANSPORT=rdma,
EFS_RDMA_DEV=rxe0, EFS_RDMA_GID_INDEX=1 and direct I/O. All four daemons and
both FUSE processes hold uverbs0 open; the live portal reports RoCE / RDMA
and direct I/O. Native Ethernet/InfiniBand link-layer discovery distinguishes
RoCE / RDMA from IB / RDMA. The installed cluster is left running.

Full POSIX: 216 pass, 0 fail, 1 skip, 19.5 seconds.
Two-mount POSIX2: 64 pass, 0 fail, 7.9 seconds. No new failures.
Remote results: /home/efs/efs/logs/posix-20261007-230909.tsv and
/home/efs/efs/logs/posix2c-20261007-230929.tsv.
Cluster status after testing: all four active, cluster OK, both mounts live.
The installed build reports unknown Git metadata because deployment uses
a source copy; the deployed source is from this RDMA worktree.

Devops now propagates configurable transport/device/GID settings across SSH
and to both daemons and mounts; AMD defaults to RDMA/direct, NUC to TCP.
The portal was restarted on its existing AMD port 6060 and live API verified
all six process badges. RXE remains software RoCE, not hardware offload.

## AMD root migration to /data1/efs

Moved the installed cluster from /home/efs/efs to /data1/efs after cleanly
stopping both mounts and all four daemons. Updated devops and portal AMD
configuration and the user logrotate cron paths. Existing metadata and stores
were moved without reformatting. Both paths use the same ext4 NVMe filesystem.
Four nodes restart healthy with strict RXE RDMA and direct I/O.

Mounts currently blocked by Ubuntu AppArmor fusermount3: kernel audit reports
failed mntpnt match for /data1/efs/mnt. Root setup prepared at
/data1/efs/devops/allow-data1-fuse.sh adds narrowly scoped mount/umount rules
for mnt and mnt2 in /etc/apparmor.d/local/fusermount3, reloads the profile,
and starts the cluster as efs. Root setup executed successfully; both mounts now live. Subsequent full
POSIX gates passed: 216 pass / 0 fail / 1 skip in 19.1s, and two-mount
POSIX2 64/64 in 8.2s. Evidence in /data1/efs/logs/posix-20261007-231719.tsv
and posix2c-20261007-231738.tsv. All six processes run from /data1/efs/src;
live portal confirms RoCE / RDMA and direct I/O on all four nodes. Portal restarted on 6060 with new paths.
