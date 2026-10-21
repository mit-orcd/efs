# ORCD communication follow-ups — W109/W110

**Assigned cluster:** `orcd-test` (native InfiniBand/RDMA, direct I/O).
**State:** open investigation; no code/configuration change or deployment.

| Item | Assignment | Remaining work |
| --- | --- | --- |
| W109 | orcd-test — metadata/query contention | Correlate apply queues, durability and query refresh with lookup/writeback latency; reproduce and repair the demonstrated cause. |
| W110 | orcd-test — EFS/IB/TCP communication diagnosis | Establish operation/transport correlation, separate retries from permanent loss and validate recovery under hardware RDMA and its TCP side-channel. |
| W95 (existing) | Separate ORCD packing package | Keep payload/physical amplification work distinct; TCP volume alone does not establish packing as its cause. |
| W106 (existing) | Broader hardware acceptance | Retain current native-RDMA correctness, persistence and fault gates; these profiles do not close them. |

## Evidence baseline

October 11 UTC capture `communication-20261011T045504Z`, deployed runtime
`52bc8edb`: four storage daemons and seven FUSE mounts; existing 16-worker
`tiny_files.py` workload on fcstor007 continued. Five 30-second C CPU captures
(four daemons plus that FUSE client), separate bounded syscall runs, and IB/TCP
counter snapshots on all eleven hosts. Counter windows are approximately
40.6 seconds before/after perf and 91.5 seconds before/after the initial traces.
The later four-second decoded traces are separate windows. All tracers detached;
final status showed four nodes up and heal idle. No restart or mode switch.

**Transport findings:** two retransmissions on EFS TCP server-to-client sockets
(fcstor004→007 and fcstor006→007); no observed socket resets/replacement or hard
network syscall errors. Host-wide retransmissions total 35 in the longer window;
only two are attributable to sampled EFS sockets. All 25 selected error/drop/
retry-related counters on mlx5_2 remain unchanged across all eleven nodes.
Historical ACK/CQ error totals predate the baseline and have no timestamp or
process attribution. Zero counter growth does not exclude transient receive
starvation: hardware RNR retry is unbounded, but software guards overdue sends
at five seconds and bounds reply waits. Nonblocking EAGAIN
and zero/5-ms poll returns are not failed RPCs. No per-op lost-message rate is
available from these captures.

**Software findings:** fcstor004 service logs contain 15 Raft apply waits of
21.668–50.117 ms (median 23.895 ms). These are application-progress waits,
not wire RTT. Query-build thread inclusive sampled overhead is 19.87% on
fcstor004 and 42.96% on fcstor005; client receive-wait self overhead is 8.42%,
BLAKE3 AVX2 21.84%, buffer-copy 9.14% and allocator 8.63%. CPU samples are
not request latency or message counts; caller chains overlap and some frames
are unresolved. All five profiles report zero lost perf samples.

**Channel findings:** stable server-to-server TCP sockets have zero byte growth
in the first interval, while fcstor007 receives approximately 1.33 GiB over
40.6 seconds through TCP side-channels. The compiled RDMA pool is 84 KiB;
larger frames intentionally use TCP. Message types were not decoded, so do not
assign these bytes to a particular opcode/file or call this a failed RDMA
upgrade. Low average port bandwidth does not exclude microbursts or congestion.
No pure-TCP cluster-mode comparison or switch congestion capture was performed.

[Deeper findings and repair hypotheses](orcd-communication-hypotheses-20261011.md)
record the incremental-query budget, compaction correlation, TCP DSACK/probe
evidence, GC batch semantics and deadline audit, with tests to distinguish causes.

## Closure gates

**W109:** reproduce the original deep-directory/virtual-query timeouts and
namespace EBUSY under representative load with unchanged deadlines/oracles.
Correlate request timing with replication/read authority, apply queues, storage
sync and query rebuilds before choosing a repair. Repeat the full native and
peer suites without FAIL/NOTRUN; verify complete/fresh query semantics and
cold preserved bytes. Related: W66 routing/leadership, W92 query indexing and
W23/D30 pressure/compaction. Do not close those broader items by association.

**W110:** retain timestamped request identities/outcomes and endpoint/device
mapping while observing a reproduced stall; distinguish EFS retry/refusal,
TCP retransmission/reset and IB completion/port failures. Preserve existing
counter baselines; separate host-wide traffic from EFS-owned sockets/QPs.
Use available diagnostics first; any additional instrumentation is follow-up
implementation, not performed by this documentation. Validate ambiguous
request recovery and cold acknowledged bytes using existing fault/persistence
gates. Compare private RDMA and TCP fixtures if needed, without switching the
live workload or claiming that RXE establishes native-IB acceptance.

Earlier ORCD repeated-load verdict remains **1485 pass, 27 fail, 7 skips,
28 NOTRUN**; the [rollout report](orcd-integration-20261011.md) owns that result.
This communication capture did not rerun or close those gates.

Small derived evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-communication/analysis.json`).
Bulk profiles, three perf report shapes, traces and raw snapshots remain under
`~/orcd/scratch/efs/orcd-test/communication-20261011T045504Z/HOST/`.
The local full report and copies are under
`/Users/mike/Documents/Codex/2026-10-05/how-x20/reports/communication-20261011T045504Z/`.

## Bounded query-refresh intervention

A subsequent 05:31:28–05:33:58 UTC diagnostic on the unchanged `52bc8edb`
runtime left the same fcstor007 benchmark running. Thirty-second enabled
windows measured 409.51 and 405.84 files/s; two disabled-query windows measured
438.79 and 442.86 files/s. Commands policy mask 3 was temporarily changed to 0
and restored automatically to mask 3, version 2, with a final authoritative
read. Mean rate during the pause was about 8.13% higher. This is one short
sequence with screen-progress quantization, not repeated matched acceptance
or request-latency attribution. It supports query contention as a contributor,
while leaving most observed overhead unexplained. W109 remains open.
[Counts, timestamps and restoration](../../results/measure/20261011-orcd-query-ab/README.md).
