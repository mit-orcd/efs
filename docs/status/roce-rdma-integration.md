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
Evidence: [retained results](../../results/measure/20261007-amd-rdma/).
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
