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
