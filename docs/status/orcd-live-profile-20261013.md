# ORCD live daemon performance profile — 2026-10-13

Collection PASS on fcstor003–006. This is a live diagnostic, not qualification
or a closure of sustained close EIO. Sourcee8bab21014bad21e5ea2198141e4df3a45d3dc3d
is installed; native RDMA/mlx5_2, files/direct-I/O, `.efs` client/server enabled,
packing/containers/expiry disabled, nonroot siteUID246111. Four24-core EPYC
7402P hosts, six NVMe stores each. Existing namespace about17.9million rows,
including the retained400GiB fixture and successive failed benchmark fixtures.
Seven16-worker tiny-file writers and seven16-worker MD5 crawlers were confirmed
active. No deployment, daemon pause/restart, store deletion or flag change.

CPU collection:45s at2026-10-13 22:49 EDT (artifactUTC20261014-024903),
`perf record -e cycles:u -F99 --call-graph fp -p PID -- sleep45`,
per-thread pidstat1s/45 and proc state/wchan1s/45. All four recordings report
zero lost samples (22367/31356/8060/28967 samples respectively). Follow-up
10s proc syscall/mutex observations and5s iostat samples are later windows;
they are not simultaneous with every CPU/I/O figure below.

| Host | CPU cores used,45s average | RSS KiB,average | Actual write_bytes delta | Cancelled dirty bytes delta |
| --- | ---: | ---: | ---: | ---: |
| fcstor003 | 1.3704 | 930110 | 4196478976 | 151552 |
| fcstor004 | 1.2797 | 1535083 | 17766776832 | 3990892544 |
| fcstor005 | 0.9507 | 1645930 | 18179751936 | 4301910016 |
| fcstor006 | 1.1515 | 1006267 | 3813490688 | 147456 |

All perf percentages below are fractions of sampled `cycles:u` events, not
fractions of total daemon CPU or wall time; some frames are unresolved.
CPU has headroom. System CPU is54–88% of one core; read/write syscall counters
increase by roughly2.1–3.8million per daemon over the window. These counters
include all daemon operations and do not alone assign bytes to one subsystem.
RSS is modest against about251GiB RAM/host.

## Ranked issues

1. **Global Raft-host mutex convoy, especially004.** About52.3threads/sample
   wait on one mutex across10proc observations (53.0all candidate mutex waits).
   Debug types and direct pointer reads identify it as h->mu, address
   0x7f4b3f932378, not an idle worker condition variable. OwnerTID2866431 is
   host_pump (perf confirms drain_inbox/on_ae_req/apply_committed/host_apply).
   It was observed executing fdatasync(syscall75) on fd1911, resolved to
   `/data1/01/efs-orcd-test-erbmi1/mdraft/log/raft.log`, and also waiting on
   KV mutex0x10104e8. The latter address is independently resolved as l->mu.
   Thus disk/KV delay inside one global host critical section can queue
   foreground metadata requests despite spare cores. Mutex owners/wait counts
   are racy samples, not a complete per-request latency distribution. Priority:
   examine durable pump/log synchronization and KV work under h->mu; preserve
   apply ordering and acknowledgement durability when correcting progress.

2. **Repeated unsuccessful `.efs` refresh work and shared metadata device.**
   Both004/005 have active query-build files and sampled host_query_thread /
   efs_query_disk_build_checked stacks.005 inclusive query-builder CPU11.9%,
   0043.7%. Logs repeatedly report EFS_ERR_QUOTA(-11), about17.9million rows,
   index-stage rebuilds and no completed persist stage. Later005 examples grow
   from9.0s/878rebuiltshards to16.3s/1576shards; attempts recur after about30s
   plus build duration. This is real work ending in refusal, not successful
   refresh.004/005 cancelled-dirty-write counters grow4.0/4.3GB in the CPU
   window; aggregate actual writes are17.8/18.2GB. It is not valid to attribute
   all those writes to queries: fragments, WAL and compaction also contribute.
   Metadata/query cache/raft.log share `/data1/01` on nvme0n1; other stores
   map to nvme1n1–nvme5n1. Follow-up nvme0n1 utilization peaks90.7%/89.0% on
   004/005, write await up to5.38/4.23ms. Other data drives have much lower
   sampled utilization. NVMe%util is not a proof of maximum bandwidth, but
   shared-device contention plus failed refresh churn is a clear priority.
   Identify which unchanged16/32GiB accounting constraint refuses the build;
   do not claim a specific cap cause from rc alone or raise caps as a fix.
   Review early admission/backoff and avoid rebuilding a predictably refused
   generation while retaining refresh correctness.

3. **High metadata point-lookup fanout.** Inclusive lsm_get CPU fractions are
   24.8%/21.5%/11.4%/19.7% across003–006. Do not add overlapping inclusive
   frames. leaf lookup, segment probes, block searches and comparisons dominate
   this path. Sample005 has262–266L0 plus460L1 segments, with about0.91GBL0.
   Source walks eligible segment ranges for point lookups; it releases l->mu
   around block I/O and revalidates the view, so it is incorrect to say every
   point read holds KV mutex throughout disk reads. Probe/cache/revalidation
   work still adds serialization, especially when Raft apply also owns h->mu.
   Priority: measure probes/key and cache misses before choosing index/compaction
   or point-read changes. Newer read-admission progress commits on devel-rdma
   are separate from this deployed source and are not proven to close this
   observed fanout or the long-load EIO.

4. **Fragment checksumming is a significant CPU cost, lower priority than
   blocking.** efs_hash inclusive8.1%/16.3%/18.4%/10.9%; leaf AVX2 BLAKE3
   5.8%/13.8%/14.2%/8.8%. Stack paths are server_handle_conn fragment work.
   No packing is enabled, so tiny logical files still exercise full fragment
   storage/verification. These are CPU proportions, not wall-time latency
   or an argument that integrity checks are optional.

RDMA receive wait is about0.9–1.6% leaf CPU in this sample, substantially below
metadata probes and checksum work. No FUSE daemon CPU profile was collected:
these observations do not prove FUSE overhead irrelevant. Most sleeping poll
threads are connection waits, not evidence that all of them are contended.
The worst long-load close EIO was not captured at the instant of failure;
this profile identifies plausible delay paths, not a complete EIO causal proof.

## Reproduce and retained evidence

Run the committed `scripts/profile-live-efsd.sh PID NEW_DIRECTORY 45` natively
on each host with the same user as efsd, perf/pidstat installed and perf event
permissions available. Output belongs outside Git. The script records exact binary
SHA, arguments, CPU stacks, thread activity, wait states and proc I/O deltas.
Do not run a build, fault injection or restart alongside a comparison.
Use fresh artifact directories and record active client workloads separately.

For mutex identity, non-attaching GDB on the binary gives &g_host and offsets
of host.mu, host.kv and kv_lsm.mu. Proc memory reads resolve only these pointers
and40-byte glibc x86_64 mutex headers; proc thread syscalls identify futex waits
with expected value2, and the owner fields identify a liveTID. Owners can change
between reads. No GDB/ptrace attachment or process stop was used. This helper
is Linux/glibc-specific; re-derive offsets, do not reuse sampled addresses.

Raw perf data and summaries are retained externally on each host at
`/home/erbmi1/orcd/scratch/efs/orcd-test/fcstorNNN/live-profile-20261014-024903`.
Local copied text/JSON summaries are under operations/devel-efs, outside Git.
Daemon logs are per-host `profile-efsd-e8bab21014ba.log`. A no-match grep on
003/006 made the follow-up wrapper exit1 after their lock/device capture;
all four core perf/pidstat/wait captures and lock/device files are complete.
This is a grep-no-match collection limitation, not a daemon failure.
