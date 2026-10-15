# Intel eight-writer default and AMD dd profile — 2026-10-15

Follow-up to [private overlap qualification](intel-private-overlap-20261015.md).
**PASS: native routing checks on four hosts, production rollout, preserved-file
checks on four mounts, concurrent pressure smoke, three untraced 1 GiB writes
and two diagnostic 4 GiB writes with final fsync/full SHA-256 verification.**
Full POSIX/fault/crash qualification NOTRUN; no additional optimization enabled.

## Change and deployed identity

`bb22e9559337a836856dce1720b739743f0ef2d7` changes production daemon startup
from CPU-derived sizing to `EFS_DEFAULT_WRITERS` (eight). `--writers N`, including
inline zero, still overrides it. Internal/benchmark CPU-based sizing is unchanged.
No change to durability barriers, metadata publication, formats or acknowledgment.
Build ID `bb22e9559337`, branch devel, build timestamp `2026-10-15T12:10:00Z`;
native GCC -O3 -g -fno-omit-frame-pointer -march=native -mtune=native.

Intel main nodes retain 400 GiB/node, TCP/direct, ordinary files, ports17432/17433,
/data1 and /data2 roots. `.efs` client/server, packing, containers and expiry OFF.
Four matching clients: Intel1/Intel2 /data1/efs/mnt; AMD and physical xorinox
/data1/efs/intel-mnt with independent /data1/efs/intel-client deployments.
All daemon startup logs confirm `writers=8 shared` without an explicit override.
Commands policy mask=0. Native writer-routing tests passed on all four hosts.

Main daemon PIDs Intel1 3908856/3908863; Intel2 4173451/4173458; AMD client1127604.
Daemon SHA-256 Intel1 `b04be72ec415923b6427b5973e33851b515244b6f22243082665016ac0094d00`,
Intel2 `a81f95f59a9d1fb377d4cd3e102fc670d5557ea779bef149e330f8b084b3e6e2`;
AMD client `a86679db40642e0cff4371f4d0a08a38e63a8427e3c8f6c021a9ebd2439f33b4`.
Releases under /data1/efs/releases/bb22e9559337 on Intel and
/data1/efs/intel-client/releases/bb22e9559337 on the external clients.

All four mounts verified 64 preserved tiny-file inode/size/hash samples before
and after rollout. Drain-aware client stops preceded daemon stops. Startup used
launch/wait/mounts phases with existing stores; no formatting. Private Intel
processes/stores/mounts and AMD-local services were untouched. No active human
dd/tiny-file/MD5 jobs were found at preflight; none needed stopping/restarting.

## Throughput and smoke

Existing `tests/measure/intel_pressure_smoke.py` passed two concurrent 64 MiB
writers with two fsyncs, shrink/regrow, full content and exact EOF checks.
AMD nonzero dd, bs=1M, conv=fsync,excl, three 1024 MiB runs:
**91.43, 70.03, 112.73 MiB/s; median91.43**. Full hashes passed, verification
outside timing. This is not a controlled comparison against historical77 MiB/s:
fixture size, catalog state and background conditions differ.

Separate 4096 MiB diagnostic runs measured101.00 and103.83 MiB/s, both hashPASS.
Do not compare traced throughput as a baseline. First capture overlapped creation
of the local input fixture; repeat reused it and waited for the actual dd process
before launching observers. The warm repeat is the attribution evidence below.
All owned mounted outputs removed after verification; local input fixtures kept.

## Warm profile findings

20s `perf record -e cycles:u -F99 --call-graph fp`, followed by12s
`strace -f -c -w`, for each main daemon and the independent AMD client. Existing
`scripts/profile_intel_live.sh` also sampled wait channels and CPU/disk/network
counters. CPU snapshots span12:20:54–12:21:14 UTC. No lost perf samples reported.

* AMD client3.67 CPU-seconds/20s:18.4% of one core. Intel daemons6.70/7.58/8.02/7.55
  CPU-seconds:33.5–40.1% of one core each. No aggregate CPU saturation demonstrated.
* AMD user-cycle samples: memcpy40.59%, BLAKE3 23.21%, `efs_dcache_yield_extra`8.31%,
  mutex unlock7.39%, committed accounting4.64%, mutex lock4.28%, XOR3.17%.
  These are fractions of sampled user cycles, not wall-time fractions.
* Intel wait samples include `xlog_wait_on_iclog`, `xlog_wait` and flush-workqueue
  waits. Most total thread samples are idle/network/futex waits; counting these
  does not identify which wait lies on the active PUT critical path.
* Per-daemon fsync calls in12s:10940/10953/11029/10992, mean2.700/2.532/2.648/2.507ms.
  No fsync/fdatasync errors captured. These combine file/directory/other syncs;
  they are not isolated file-barrier timings. Strace perturbs execution and
  summed syscall wall time across threads is not a percent of elapsed time.
* AMD NIC TX3,627,108,959 bytes/20s (~173.0MiB/s), below its2.5Gb/s link ceiling.
  No new NIC errors/drops on the three hosts. TCP retransmissions:AMD7, Intel0/0,
  host-wide. This does not establish zero EFS retries or zero packet loss.
* Intel /data1 devices wrote1,245,223,936/1,353,312,768 bytes with16,932/17,503
  flush requests in20s. Busy time15.15/15.88s. Device counters include other work;
  NVMe busy time alone is not proof of throughput saturation. AMD fixture-disk
  writes were only69,120 bytes in the repeat, removing the first-run interference.

## Next options, ranked

1. Investigate bounded four-fragment durable cohorts. Write ahead within the
   cohort, retain every file barrier, share only safe directory persistence, and
   acknowledge after required barriers. Earlier independent local results show
   32–37% potential with p95 rising~9→23ms. Production already groups directory
   syncs, so that gain is not additive or established end-to-end. Need stage
   attribution, bounded waiting/flush of partial cohorts, error ownership and
   crash qualification before implementation/rollout. No barrier removal.
2. Test sixteen workers for sustained AMD throughput and small-file fairness.
   Private tests showed only~16% beyond eight; it may help without new code but
   is not qualified on this production workload.
3. Copy/hash reductions are lower priority for throughput while the client uses
   only18% of a core. Do not interpret high relative perf percentages as evidence
   that those operations currently impose the throughput ceiling.

Current evidence supports remaining durability/response-wait costs, but does
not isolate a new exclusive bottleneck or rule out metadata/RPC stalls. Precise
PUT admission/file-sync/directory-sync attribution would require another staged
capture; no new root probes were installed here.

## Reproduction and evidence

Build from the exact revision on each host:
`make -j4 EFS_GIT_ID=bb22e9559337 EFS_GIT_DESCRIBE=bb22e9559337
EFS_GIT_BRANCH=devel EFS_BUILD_TIME=2026-10-15T12:10:00Z efsd efs-fuse efs-mgmt
tests/test_writer_routing`, then `./tests/test_writer_routing`.
Production uses unchanged Intel devops launch scripts and independent external
client wrapper, with EFS_TRANSPORT=tcp, EFS_DIRECT_IO=1, EFS_COMMANDS=0,
EFS_COMMANDS_SERVER=0, EFS_PACKED_WRITES=0, EFS_EXPIRY=0.

On AMD: `python3 tests/measure/intel_dd_sync_batch.py /data1/efs/intel-mnt
/data1/efs/intel-client/default8-measure candidate --mib 1024 --repeats 3`.
For diagnostic use `--mib 4096 --repeats 1`; prepare/reuse the local input before
capture. On each observed host: `bash scripts/profile_intel_live.sh NEW_DIR
PID_COMMA_LIST 20`. Restrict PIDs to the current main Intel daemons / AMD Intel
client. No production/private PID matching by broad process name.

Raw controllers, captures excluding binary perf.data, build logs, hashes and
summary parser:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-default8-20261015/`.
Remote raw perf recordings: `/data1/efs/default8-profile-warm-20261015` on Intel;
`/data1/efs/intel-client/default8-profile-warm-20261015` on AMD. First capture
is retained alongside under names without `-warm`. SHA256SUMS indexes local
artifacts. Input1GiB SHA256
`926f401c3e2a3b33c2db9dce7a8be7f59053eece873505792ebac2b926683be7`, input4GiB
`03fbdc266d722ed20906b67839dcb70c5b01700836aedbfbcba0ea72f5bdd050`.
Observers and owned workloads finished; all main nodes remain deployed at eight.
