# W111 — NUC sustained durable-write slowdown

**Status: open investigation; cluster: NUC.** Split from W97 on Oct 11, 2026
at the user's request. No new NUC test, repair, deployment or drive-setting
change is claimed by this documentation update.

**AMD overload/admission is closed.** Its ten-minute direct/buffered unpaced
load, all-worker progress, acknowledged cold bytes, physical drain,
post-pressure cold writes, native/fault and 216/67 functional gates pass.
Exact tested artifacts are deployed on retained AMD stores and both mounts
pass the independent audit. [AMD closure](amd-overload-admission-20261011.md).
NUC testing is not required to retain that AMD acceptance.

W111 owns the NUC time-dependent durable-write decline and XFS/block-queue/
device/cache attribution. W97 retains independent replication-inbox feedback
and bounded catch-up acceptance; W94 retains production data-plane integration.
Neither open scope reopens the completed AMD gate. W99 physical-capacity policy
is independent of the logical-quota admission experiment.

## Retained evidence

| Measurement | Observation | Evidence |
|---|---|---|
| Matched three-node drives | NVMe 31.83→10.80 files/s, Raft sync 2.465→6.993 ms; SATA 562.73→299.40 files/s, 0.150→0.319 ms | [Drive comparison](w97-three-drive-20261010.md) |
| Continuous load / pause / resume | First-minute 152/159 falls to 53/48 files/s; pause restores a brief burst, not sustained rate | [Merged-code observation](w97-latest-nvme-20261010.md) |
| EFS-stopped growing-file and ring controls | Growing NVMe declines; fully written NVMe also declines 1373/1356→450/439 durable writes/s; written SATA remains near 7100/s | [Allocation, XFS profiles and written-ring controls](devel-nuc-closure-20261010.md) |

Existing filenames, build labels and dated W97 references are historical
evidence; they remain intact. The transferred backlog detail is now under
[W111](../backlog/work-items.md#w111). Failed experiments and successful cold
checks remain separate observations. None proves a NAND-cache cause, hardware
ceiling or repair of the EFS SATA decline.

## Next gate

Use comparable device/workload state and repeated growing, preallocated and
fully written controls. Correlate sync tails with XFS journal work, block
dispatch/completion, queue depth and scheduler waits; record device telemetry
where available. Compare EFS-stopped and loaded runs without removing
durability barriers. A repair needs repeated sustained/tail improvement,
bounded peer/queue progress and cold-byte/native/POSIX/peer/fault acceptance.
A demonstrated hardware limit needs a measured boundary and documented
mitigation; it must not be presented as a software fix.
