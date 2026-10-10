# W97: matched three-node NVMe-only and SATA-only tests — Oct 10

Status: requested comparison complete; W97 remains open. Engine source is
`6b8f05acb484b52284a7ffdd6c1da7b0476f35f6`, build `6b8f05acb484-w97`, branch
`devel-persistence-pressure`. No production code or drive setting changes.

## Answer

**SATA latency also rises when all three EFS nodes use that drive.** The earlier
mixed-drive samples do not show that rise on SATA: first/final-minute server
Raft sync means are approximately 0.09–0.11 ms there. The new isolated tests
reproduce sustained decline on both drives, with far lower absolute SATA
latency and much higher SATA throughput for this durable tiny-file workload.

| Three-node storage | First-minute files/s | Final-minute files/s | First → final mean server Raft sync | Whole 20-minute files/s |
| --- | ---: | ---: | ---: | ---: |
| Samsung 970 EVO NVMe | 31.83 | 10.80 | 2.465 → 6.993 ms | 12.91 |
| MK0800GEYKE SATA SSD | 562.73 | 299.40 | 0.150 → 0.319 ms | 300.76 |

Server sync means pool the measured Raft sync histogram sums/counts across
all three nodes in each window. They include the server's observed barrier
service time; they are not bare hardware latency or medians. NVMe creation
falls 2.95× while server sync grows 2.84×. SATA creation falls 1.88× while server
sync grows 2.13×. SATA's last minute rebounds relative to preceding windows:
this decline is not strictly monotonic.

Five-minute creation rates:

- NVMe: **18.59 / 11.35 / 10.84 / 10.86 files/s**.
- SATA: **417.92 / 271.59 / 252.39 / 261.22 files/s**.

Linux NVMe flush means grow **1.375 → 3.870 ms**, mean read/write request time
**1.186 → 3.403 ms**, and IO busy accounting **91.39 → 93.57%**. SATA reports
zero separate flush requests, so no SATA flush mean can be inferred. Its mean
read/write request time grows **0.179 → 0.409 ms**, and IO busy accounting
**92.17 → 95.48%**. First/final host-write rates are **15.09 → 4.72 MiB/s** on
NVMe and **239.58 → 134.23 MiB/s** on SATA. Reads are very small in those windows.
These are whole-device counters, including other OS activity; busy percentages
are not proof of a hardware ceiling. Do not compare SATA request means with
NVMe flush means as though they measured the same operation.

Whole-run CREATE/open → write → CLOSE latency p50/p95/p99:
**680.57 / 884.32 / 981.01 ms** NVMe versus **22.03 / 49.92 / 69.56 ms** SATA.
These distributions combine early and late periods, not per-minute tails.

## Method and limits

One fresh private three-node cluster per drive, NVMe first then SATA, twenty
minutes each. Regular NUC mounts and four daemons drain safely before tests and
remain stopped during both runs. Every node's metadata, Raft/KV files and payload
storage use the selected volume. Explicit **EFS_MD_RAFT_N=3**; both groups are
asserted hosted on all three nodes with voters **0x7**. Both cases initially elect node 1 as leader of both groups. NVMe has
elections around sampled seconds 40–45 (group 0 reaches term 3; group 2 term 4),
after which sampled leadership settles on node 1, while SATA keeps
node 1/term 1 throughout the samples. These leadership changes and early long
operation tails are retained confounders; the comparison does not hold leader
history constant. The regular cluster later returns to four participants.

Eight workers, four per verified inode-placement group; 1–512-byte actual
SHA-derived payloads, exclusive CREATE, one write, CLOSE. Normal durability is
preserved; no per-file extra fsync is added. Preparation selects 2,048 directories
per group (512 per worker), 1,000 files per directory, bounded at 512,000 files
per worker. This larger cap avoids ending a faster SATA test early. Preparation
is excluded, followed by the same fixed 30-second settle interval. Preparation
takes longer on NVMe, so starting drive conditioning is not an empty-cache oracle.
Physical free-space >80 GiB on both volumes and available RAM >3 GiB are checked.
Private node quotas are 80G. Neither workload approaches its file cap.

Same verified installed binaries, FLOW=1, 10 ms marker interval, direct IO/TCP;
commands, overlap, no-op, group drain and early retirement off; batch observation
on during measurement. Actual fsync/fdatasync perf tracepoint counts retain
enable/disable acknowledgements. Five-second snapshots retain process/host IO,
RAM/CPU and group status. Observer logs live on SATA outside engine storage.
The NVMe volume also hosts root/home/swap; unrelated OS traffic is not eliminated.

This is a **drive/model/filesystem-path comparison**, not a general protocol
ranking. NVMe uses XFS/LVM on Samsung 970 EVO 1 TB firmware 2B2QEXE7;
SATA uses XFS on MK0800GEYKE, firmware HPG1. Linux reports NVMe write-back and
SATA write-through. No cache, APST, power, scheduler or firmware settings change.
One run each is useful discrimination, not a repeated randomized A/B acceptance.
Timed runs also grow different dataset sizes because SATA completes far more
files; this is not a matched-inode-count comparison. Three-node topology, fresh
trees and lack of mixed-read probes differ from the
previous retained-store four-node run; absolute rates cannot be attributed to
node count alone.

## Health telemetry and cache hypothesis

The user starts the read-only root NVMe collector after the NVMe decline begins.
146 samples span **18:09:37–18:45:54 UTC**. Composite temperature is **34.85–45.85°C**,
second sensor **32.85–61.85°C**. Critical warnings, media errors, warning/critical
temperature times and host thermal-management transition/time counters remain
zero; the historical error-log count stays 2670. No recorded thermal indication
explains the ongoing slow phase. This does not exclude every vendor-specific
throttle mechanism or establish temperatures during the unrecorded initial burst.

Raw temperature values are Kelvin; the analysis converts by subtracting 273.15.
[NVMe management utility and SMART field example](https://nvmexpress.org/open-source-nvme-management-utility-nvme-command-line-interface-nvme-cli/)
and [NVMe thermal-management features](https://nvmexpress.org/resource/technology-power-features/)
explain these reported fields. Identify reports warning/critical thresholds 358 K,
HCTM support, volatile cache support and APST support. Get Feature confirms
volatile write cache enabled, APST enabled and requested power state 0; these
are configuration observations, not a measured runtime power-state trace.

Noninteractive SATA SMART access is denied. An optional read-only root SATA
collector is provided but no samples arrive; SATA temperature/throttling cannot
be concluded. Both telemetry stop markers are set and the NVMe collector exits.

The latency/throughput relationship is reproducible on both drives, and server
barrier time tracks it. This strengthens persistence-service latency as a
major contributor. It **does not establish NAND cache exhaustion**, drive-only
causality or a hardware ceiling. Host bytes do not measure NAND write
amplification/cache occupancy. The next discriminating test remains matched
sustained durable writes outside EFS, separating preallocated overwrite from
growing XFS WAL/files and recording device health plus barrier latency. Preserve
flush/durability semantics; peak sequential bandwidth is not the relevant ceiling.

## Pressure, correctness and restoration

NVMe completes **15,506 files**, SATA **361,060**, both with zero loader errors.
Timely sampled counts exclude a small in-flight drain tail; final completed
counts include it. All replicas cover captured leader commit cuts. No measured
Raft sync-error increments occur. NVMe inbox refusals total **35** on n3;
SATA totals **15** on n2 and **109** on n3, versus 75,109 on n3 in the earlier
mixed-drive continuous observation. The decline therefore occurs without large
refusal counts; mixed-drive speed asymmetry/pressure may amplify it. Refusals
are queue/retry observations, not lost file operations.

Each private cluster is safely drained, restarted cold and checks first/last
four exact payloads per worker: **64 checks per drive**, 128 total through the
private mount. Private stores and test files are retained, never substituted
for regular NUC stores. Regular n1–n4 and both mounts restore on the verified
same build, FLOW=1/default 10 ms/TCP/direct IO, commands/optional tuning/diagnostics
off. Active executable hashes/default flags and both mounts pass final verification.
No private server, generator or profiler remains. Native/full POSIX/peer gates
of this unchanged engine are retained in the preceding report; this turn adds
the matched performance, catch-up and cold-byte gates rather than rerunning them.

Retain the initial setup failure: default four-participant bootstrap fails the
three-voter topology assertion before timed file creation. Its automatic restore
initially checks node 4 before its listener is ready; the later readiness retry
passes. Corrected runs explicitly set three participants, restore four, and wait
for all restored listeners before final assertions. No weakened topology check,
force-discard or store wipe is used. Final tests/restore/run exit records are zero.

Raw evidence, controllers, launch/analysis provenance, retained failure and final
restoration: `results/measure/20261010-w97-three-drive/`; remote evidence
`/data1/efs/w97-three-drive-v2-20261010`. Private stores are
`/home/efs/w97-three-nvme-v2-20261010` and `/data1/efs/w97-three-sata-v2-20261010`.
`analyze.py` regenerates minute/five-minute windows and pooled sync means;
`health-summary.py` regenerates temperature/counter coverage. W97 stays open.
