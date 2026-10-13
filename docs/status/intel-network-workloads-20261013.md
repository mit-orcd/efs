# Intel cross-host network workloads — 2026-10-13

W97 / D30 / package3 ordinary-files diagnostic. **Correctness PASS** on both
hosts. **TCP retransmissions and NIC receive misses OBSERVED; cause OPEN**.
This is bounded correctness/traffic evidence, not throughput optimization or
closure of previous seven-client, ORCD or physical-reclamation acceptance gates.
No server deployment, format, budget, transport, NIC or timeout tuning.

## Source, topology and profile

Harness on `devel`: `c73c251dbf07b7000b02744b81a644e0b3456ec2` (initial
harness `3e1da43a`; follow-up preserves payload offsets on short writes).
Actual running product source: `1a737143e5437e0f4315e86b7ef51e38a989b811`,
not the newer branch head. No claim that this run tests the recent query fix.
Intel1 binaries built2026-10-13T16:54:57Z; Intel2 built16:55:09Z.

Two Core Ultra7 256V hosts,8 CPUs/~15GiB RAM each; Intel1 `10.0.0.207`,
Intel2 `10.0.0.209`. Each runs two logical EFS daemons, ports17432/17433,
400G per node, `/data1/efs/n1|n3` and `/data2/efs/n2|n4`. Ordinary files,
TCP/direct-I/O; `.efs` client/server, packing, containers and expiry OFF.
Actual daemon/client `/proc` profiles and disabled command policy mask0 PASS
before/after. Both original mounts `/data1/efs/mnt` remain intact.

Links: `enp128s0`,5,000Mb/s/full duplex; Realtek r8169, kernel
`6.12.0-211.64.1.el10_2.x86_64`, firmware `rtl8126a-3_0.0.5 08/30/24`.
Fresh diagnostic clients on each host use the opposite host's17432 seed.
Each phase gets a new FUSE process and normal unmount, preventing reuse of the
previous writer's client cache. `EFS_RPC_PROF=1` only on these temporary clients;
all four feature flags remain0. Original clients/profilers are untouched.

| Host | Binary | SHA256 |
| --- | --- | --- |
| Intel1 | efsd | a5df1001d866f6e05ff6623c18644437e265a50675b2197617607231bc04f854 |
| Intel1 | efs-fuse | 3f7dcdea4543955021d3008d262d0f6a42c8494e9315cb73bec7ec8bbd57a59a |
| Intel2 | efsd | 78b362c911ada034d26a9c5a680f02234cedae122c1c613fb146c0b4175d2103 |
| Intel2 | efs-fuse | e2259433940da7461ada1379a058e45de4536299fe2f4fdb436ae9302cf3359b |

## Reproduction and workload

Prerequisites: SSH aliases `intel1_efs`/`intel2_efs`, nonroot efs account, current
four-node profile and mounted export, Linux Python3, ss, ethtool, existing
`/data1/efs/src/scripts/client.sh`, enough storage for2GiB plus metadata.
No extra root packages or firewall rules were required. Run from this repository:

```sh
python3 scripts/intel_network_workload.py run --run-id net-20261013-a \
  --output /Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-network-20261013-a
```

For another run, supply a new unique run-id and an output directory outside Git.
The driver refuses existing driver/fixture names and preserves failed fixtures.
Remote raw files live in `/data1/efs/network-check/<run-id>`; diagnostic mount
is its `mnt` subdirectory. Never recursively copy a retained mounted directory.

1. Baseline without our workload, nominal10s, with existing Raft/background traffic.
2. Both hosts concurrently create16 files×64MiB, four workers each. Deterministic
   owner/index-specific SHA256-seeded bytes,256KiB writes, checked short writes,
   fsync and close on every file:1GiB per host.
3. Fresh opposite-host clients read every byte and exact EOF of the peer's16 files,
   then unlink only verified fixtures. Both owners' payloads are checked.
4. Both hosts create10,000 files×4KiB, four workers,64 parent buckets each,
   fsync and close each file. Fresh peer readers verify all bytes and exact EOF,
   then rename and unlink each verified file and remove the empty owned directories.

Every worker phase has the unchanged900s outer limit (TERM,10s kill grace);
normal client start/stop has90s. Each worker waits3s and performs an uncached
missing-name lookup to publish the RPC profiler tail. No application-level
retry is added by the test to hide a failed operation. Durations below include
this3s tail; they are evidence, not performance acceptance thresholds.

| Phase per host | Intel1 result / seconds | Intel2 result / seconds |
| --- | --- | --- |
| Large writes |16/16 PASS /23.197 |16/16 PASS /21.124 |
| Peer large reads and unlink |16/16 PASS /13.222 |16/16 PASS /8.968 |
| Small create/fsync/close |10,000/10,000 PASS /132.957 |10,000/10,000 PASS /99.508 |
| Peer small read/rename/unlink |10,000/10,000 PASS /249.292 |10,000/10,000 PASS /351.864 |

Total verified:32 large files +20,000 small files,2,229,403,648 logical bytes,
zero worker errors. All diagnostic clients unmounted normally. Both hosts
independently checked all four owned fixture roots absent through the original
mount; observer processes exited and diagnostic mounts are no longer mounted.
This checks namespace cleanup, not immediate physical reclamation.

## Network counters and attribution

Observers sampled `/proc/net/snmp`, `/proc/net/netstat`, NIC sysfs counters and
peer-filtered `ss -tinHe dst <other-ip>` roughly once per second. Only sockets
with local or remote EFS port17432/17433 contribute to EFS socket totals.
536/535 samples, zero collection errors; windows approximately550.6/549.1s.
Driver hardware statistics came from `ethtool -S` at both ends.

| Whole observation window | Intel1 | Intel2 |
| --- | ---: | ---: |
| Host-wide TCP RetransSegs increment |35 |51 |
| Sampled EFS peer socket retransmissions, lower bound |30 |29 |
| Sampled EFS peer retransmitted bytes, lower bound |19,329 |23,970 |
| Host TCPTimeouts / abort-on-timeout / OutRsts increments |0 /0 /0 |0 /0 /0 |
| Host TCPFastRetrans increment |6 |11 |
| NIC driver rx_missed increment |10 |6 |
| NIC RX/TX errors and standard RX/TX drops increments |all0 |all0 |
| Driver alignment errors, TX aborts/underruns and collisions |all0 |all0 |
| Driver received packets increment |10,387,722 |7,490,213 |
| Sampled EFS peer bytes sent, lower bound |4,838,610,199 |9,264,452,772 |
| Sampled EFS peer bytes received, lower bound |9,264,379,483 |4,838,615,035 |

Large-file traffic contains the observed EFS retransmissions:

| Active phase | Host retransmissions Intel1/Intel2 | Sampled EFS retransmissions Intel1/Intel2 |
| --- | --- | --- |
| Large writes |21 /24 |21 /24 |
| Large reads and cleanup |13 /24 |9 /5 |
| Small writes |0 /1 |0 /0 |
| Small reads/rename/unlink |1 /2 |0 /0 |

Baseline retransmissions are0 on both hosts. Observed EFS baseline byte rates
were only about5KiB/s per direction; GB-scale peer socket deltas during
work prove cross-host traffic, rather than a cache-only test. These totals also
include existing EFS background traffic and EC/protocol/metadata amplification;
user-payload bytes and wire bytes are not interchangeable.

Interpretation: receive-side packet-loss indications and TCP retransmissions
occurred, but every tested payload and operation succeeded. Do not call the
link loss-free based on standard rx_dropped=0: the kernel documents missed
packets separately from standard RX drops
([Linux interface statistics](https://docs.kernel.org/networking/statistics.html)).
The upstream6.12 r8169 driver exposes hardware `rx_missed` tally through ethtool
and uses it for standard rx_missed_errors
([driver implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/realtek/r8169_main.c)).
The distribution build may contain patches; this supports counter interpretation,
not proof of a particular RX-ring, scheduling, cable or switch fault. The
hardware counter has no per-flow attribution. No switch counters, packet
capture or kernel retransmit event trace were taken, so an exact EFS wire-loss
rate and a cause-to-retransmission mapping are NOT established.

Host TCP counts include SSH/other traffic. Peer socket totals are lower bounds:
short-lived sockets and final increments before disappearance can be missed
between samples. Missing zero-valued ss retrans fields are treated as0, as
reported by this iproute2 tool. Phase boundaries have about one sampling interval
of ambiguity; EFS peer counts cannot explain every host-wide retransmission.
NIC/kernel counters also differ in offload accounting, so no packet-loss
percentage is calculated from incompatible denominators. RTT peaks across
sampled EFS sockets were30.111/30.153ms; they do not isolate wire latency.

## EFS retries and bounded tracing

| Observed diagnostic-client RPC evidence | Intel1 | Intel2 |
| --- | ---: | ---: |
| Logged inode-RPC transport retries (`why=send` / `recv`) |0 |0 |
| Logged no-hint retries |0 |0 |
| Large-writer RPC-PROF busy_n |7 |7 |
| Large-reader / small-writer / small-reader busy_n |0 /0 /0 |0 /0 /0 |
| Retry exhaustion / EBUSY / stop-refused / unmount data-loss lines |0 |0 |

Large-writer backoff time was550,470/600,486us. In this source `busy_n` counts
metadata RPC retry backoffs for BUSY/admission-BUSY/retryable STALE and
no-hint leader retries (none of the latter were logged here); it is not
packet loss or a transport-error counter. TCP recovery can happen without an
EFS request retry. These are observed client counters/logs, not a complete
census of unlogged data-path, leader-hint, lease or server-to-server retries.
Profile tails are periodically published, rather than an atomic global total.

During the small-write phase, each diagnostic client also received a separate
3s perf-stat sample then a3s strace summary. Reproduce while that phase is active,
using its daemon PID from `small-write-fuse.log` (not a production-client PID):

```sh
perf stat -e task-clock,context-switches,cpu-migrations,page-faults -p "$pid" -- sleep 3
timeout --signal=INT 3s strace -qq -f -c \
  -e trace=network,read,write,readv,writev,poll,ppoll,epoll_wait -p "$pid"
timeout --signal=INT 1s strace -qq -f -s 0 -e trace=recvfrom \
  -e status=failed -p "$pid"
```

Perf PASS: task-clock320.689/512.320ms,5,052/8,116 context switches,
72/130 page faults. Strace counted17,165/29,674 selected syscalls, including
active recvfrom, writev and sendmsg. Its recvfrom error counts3,067/5,395 are
not lost-packet counts. A separate1s failed-call sample on Intel1 captured
`MSG_DONTWAIT` returning EAGAIN; Intel2's corresponding sample contained no
failed calls during the end-of-write transition. There is no claim every
summary error was individually classified. Intentional tracer timeout rc124
means detached at the bounded duration, not a workload failure. Strace attaches
and changes scheduling; this run's durations are instrumented diagnostic
measurements, not clean throughput/latency benchmark evidence.

## Evidence and remaining decision

Raw output is outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-network-20261013-a`,
with remote copies `/data1/efs/network-check/net-20261013-a` on each host.
No payloads or raw logs are committed. Summary SHA256 Intel1:
`019a4fd3fcdacbe3757f351f03f824b68ad19aedf44a41370fc9e5df0e164e59`;
Intel2:`05e9f91664220d61294fc5a2eb541be11528ac47148b5550e5150bb25d9756b8`.
Telemetry SHA256 Intel1:
`8853a6622e22f480ea306621406f3358862703116f209bfdc34c5c461126c39f`;
Intel2:`581096f1462a2956f2fd1827e656cbe6a86449dbfacf4b52afdaf875547ca804`.

No tuning or additional root action is needed to reproduce the completed run.
If receive misses need a causal investigation, the next bounded correctness
check is phase-local rx_missed_errors plus time-correlated kernel retransmit
and switch-port counters, with an untraced control. That has NOTRUN status;
no ring sizes, offloads, coalescing, thread counts or protocol settings were changed.
