# ORCD integration and rollout — Oct 11, 2026

Merged 33 existing commits from AMD, Intel and NUC branches into devel through
`eb7d449d`. Runtime snapshot `52bc8edb` compiled on all eleven hosts and passed
complete Linux `make test`. Later commits changed only scripts/tests; installed
tooling is `368c819f`, including the corrected symlink-follow fixture. Runtime
C sources are unchanged between those points. Cluster portal commit `3132901`
adds TiB/PiB formatting and large quota-unit parsing; port 8088 was reloaded.

Cleanly drained seven clients and stopped four daemons before each runtime
switch. Prior source/binary trees were retained; storage directories and export
metadata were preserved. Final audit verifies four direct-I/O daemons, seven
mounts, native `mlx5_2` RDMA and `.efs` enabled. Cluster status reports four up,
matching runtime versions and idle heal. User file `/001/dat01` remains
107374182400 bytes; ownership, mode and four 64 KiB sample hashes match the
pre-deployment record. This is sampled integrity, not a full-file checksum.

Automatic quotas are approximately 29.31 TiB per node. Usable 2+1 logical
capacity is 78.15 TiB; management, `df` and portal values agree within sampling
and display rounding. Startup reserve policy is active; W99 runtime shared-domain
protection remains open.

| Gate | Result |
| --- | --- |
| All eleven builds | Pass |
| Complete Linux make test, runtime snapshot | Pass |
| Late client-process/leak-verdict/TLS/meta-apply gates | Pass |
| Direct-I/O TLS concurrent thread retirement | 400 retirements; no outstanding allocations |
| .efs cross-client stats/find smoke | Exact files, bytes and complete results pass |
| Native XFS and EFS symlink control | 3/3 on each |
| Installed POSIX harness self-tests | Both pass |
| Corrected four cross-client suites | 268 pass, 0 fail; all complete, rc 0 |
| Corrected repeated seven-client full suite | 1485 pass, 27 fail, 7 mmap skips, 28 NOTRUN; rc 1 on all seven |

The first full and peer runs failed only newly merged symlink-follow fixtures.
On native XFS, Linux CPython's plain `os.link(..., follow_symlinks=True)` did not
follow the source; supplying a directory descriptor dispatched to linkat and
followed correctly. `368c819f` requests that syscall explicitly, including the
expected ENOENT for a dangling target. Original failures remain recorded.

The corrected repeated full-suite load exposed **separate real failures**:
EBUSY during directory creation/removal/cleanup and 30-second deep-directory
and virtual-query timeouts. Four suites intentionally lack a complete marker
and record seven NOTRUN cases each after stuck workers; these are not passes.
The workers subsequently retired naturally; the final process audit found none.
Do not close the load gates based on the passing cross-client or earlier smoke
runs. Logs in the failure window show compaction activity and repeated
`apply lane-sweep rc=-13` on inodes 39615 and 43395. A causal connection to the
failures is not proven. Prioritize a focused metadata/GC/query contention
investigation under W66/W23/D30, retaining original deadlines and error oracles.

Small evidence: [original gates](../../results/measure/20261011-orcd-integration/original-gates.json),
[corrected gates](../../results/measure/20261011-orcd-integration/corrected-gates.json),
[runtime audit](../../results/measure/20261011-orcd-integration/runtime-audit.json),
[retired workers](../../results/measure/20261011-orcd-integration/retired-workers.json).

Bulk logs and TSVs stay under `~/orcd/scratch/efs/orcd-test/`:
`fcstor003/test-52bc8edb3d1a.log`, `merged-52bc8edb-20261011/` (original),
and `linkat-gates-20261011/` (native controls, probes and corrected runs).
Installed server/client logs are in each host's subdirectory there. W107/W108
uncommitted documentation was preserved and excluded from this integration.
