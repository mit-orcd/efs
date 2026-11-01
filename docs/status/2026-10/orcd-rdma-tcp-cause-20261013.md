# ORCD RDMA/TCP selection and 004→003 retransmissions — 2026-10-13

**TCP use explained by deployed source. Retransmission pattern reproduced;
exact loss/delay location OPEN.** Read-only investigation: no deployment,
restart, new I/O workload, application tracer, packet capture, remote output
file, feature/network/sysctl/qdisc change or tuning. Existing I/O continued.
This follows [the passive network sample](orcd-network-passive-20261013.md).

## Why TCP remains active with RDMA enabled

Running product source `9b2fac55fc69` on the unchanged ORCD servers. Source
files on004 were SHA256-matched to `git show 9b2fac55:<path>`:

| File | SHA256 |
| --- | --- |
| src/common/protocol.c |5ffdc61d699e346a51d31442e4ea31978a00f4a38f570cb8f38d75f7fd5a07f7 |
| src/server/raft_host.c |423fd36a41248a9302a169c2e5c2cbb6ec693e6e0539a1a8d372c14db07553ab |
| include/efs/rdma.h |127a3e98094078aba0a74493fc3f609f9868bd5d966abce909aae580d653d4a2 |

`peer_connect()` creates a TCP connection and then negotiates RDMA on it.
The TCP descriptor remains open. `conn_pick_send_chan()` explicitly chooses
TCP if there is no RDMA connection or the frame exceeds the negotiated RDMA
maximum; otherwise clients use RDMA and server replies follow the request's
arrival channel. The size test precedes the server-channel test, so a large
reply can use TCP even when its request arrived over RDMA.

The compiled source defines `EFS_RDMA_BUFSZ=84*1024` (**86,016 bytes**); each
connection negotiates the smaller peer/local capacity. This threshold covers
entire wire frames, not just payload. A Raft sender comment still says72KiB,
but the actual header constant is84KiB; the comment is not the current limit.
`host_sender_main()` explicitly counts TCP and RDMA replies separately for
mixed batches. GET_CHUNK also falls back from the registered reply buffer to
ordinary reply storage and the common channel selector when the reply does
not fit. With maximum fragments64MiB, TCP traffic can include actual data
replies, not only setup or Raft control traffic.

Strict `EFS_TRANSPORT=rdma` rejects RDMA upgrade/send failures; it does **not**
prohibit this designed oversized-frame TCP side channel. No buffer size,
segmentation, format or transport behavior was changed in this investigation.

Three retransmitting004→003 accepted sockets received exactly25 TCP bytes
throughout the120s sample, while sending large amounts over TCP. For native IB,
RDMA_SETUP sends a20-byte payload plus the5-byte wire header. This strongly
supports successful setup followed by RDMA requests and oversized TCP replies.
It does not decode each observed message or prove the exact negotiated capacity
of each live connection; no process-memory inspection or packet capture was used.

## Transport path and methodology

Servers003/004/005 remained files/direct-I/O, EFS_TRANSPORT=rdma,
EFS_RDMA_DEV=mlx5_2. Source/version remains9b2fac55;004 executable still reports
build2026-10-13T18:39:13Z. Existing optional-feature profile was not changed.
Kernel on003:5.14.0-687.39.1.el9_8.x86_64. TCP to003 uses `ibs1f0`, whose PCI
address matches mlx5_2 (`0000:88:00.0`). IPoIB is **datagram mode, MTU2044**,
MSS1992 in the affected TCP sockets. TSO/GSO/GRO are enabled; LRO is off.
These are observations, not changes or tuning recommendations.

Linux documents datagram IPoIB as using IB UD; native EFS RDMA connections
and TCP/IPoIB consequently have different transport/recovery mechanisms even
on the same physical port
([IPoIB documentation](https://docs.kernel.org/infiniband/ipoib.html)).

Two nonoverlapping cohorts,003/004/005 observed concurrently with three SSH
sessions:120s at nominal5s intervals, then60s with extended ethtool counters.
The first25-sample pass read all TCP/IP MIBs, softnet, configured IB/RDMA port
counters, interface counters, stable-inode EFS TCP socket statistics and qdisc
before/after. The second13-sample pass added per-ring/physical `ethtool -S`
counters at both ends. No negative cumulative-counter deltas; CurrEstab is a
gauge and decreased when connections closed. Counters are host/interface-wide
unless explicitly matched to socket inode IDs.

Reproduction requires SSH access, node Python3, ss, tc and ethtool:

```sh
python3 scripts/orcd_tcp_rdma_diagnose.py --seconds 120 --interval 5 \
  --output /outside/git/orcd-tcp-rdma
python3 scripts/orcd_tcp_rdma_diagnose.py --seconds 60 --interval 5 \
  --output /outside/git/orcd-tcp-rdma-extended
```

The first recorded pass predates the extended-counter addition; its core
sampling logic is identical. Python is streamed over SSH stdin and writes
only local output. Existing source reads, `ip route get`, ethtool read commands,
procfs and version queries were supplementary. No root action was taken.
The account has CapEff0 and tcpdump has no file capabilities; privileged packet
capture was not requested or attempted. No observer persists after completion.

## Reproduced TCP recovery pattern

| Window | Host | TCP retransmitted segments | TCPTimeouts | TCPLossProbes | TCPSlowStartRetrans |
| --- | --- | ---: | ---: | ---: | ---: |
|120s |003 |0 |0 |0 |0 |
|120s |004 |255 |5 |5 |245 |
|120s |005 |0 |0 |0 |0 |
|60s |003 |0 |0 |0 |0 |
|60s |004 |102 |2 |2 |98 |
|60s |005 |0 |0 |0 |0 |

Five individually observed stable-socket increments in120s, then two in60s:
each adds **51 segments /99,090 retransmitted bytes**. The five first-window
events all occur on004→003 port17432 replies: source004:17432, destination003
ports57438 (two events),57454 (two),46400 (one). Socket inode IDs24360882,
24360883,24396570; process2605437. First-window retransmitted bytes495,450;
second198,180. This is a repeatable loss-probe/timeout recovery pattern, not
fast retransmission: TCPFastRetrans stayed0. It is not evidence of RDMA RC
retry failure. Per-socket samples only bracket events within5s and can miss
short-lived connections; they do not capture individual TCP sequence/ACK timing.

TCP DSACK, spurious-RTO/host-queue retransmission indicators stayed unchanged.
DSACK was enabled on003. There is no positive evidence here that these were
merely spurious retransmissions caused by delayed ACKs; absence of such
indicators does not prove where a packet was lost. TCP acknowledgement/delivery
did not complete in time for these episodes, but actual packet loss versus
uncounted delay cannot be resolved from the counters alone.

On all three hosts, interface drops/errors, softnet drops/time-squeeze,
IP discards, TCP receive queue/backlog drops/pruning, TCP memory-pressure
indicators, physical discards and the sampled IB/RDMA error/retry/buffer counters
did not increase. Qdisc mq/fq_codel cumulative drops did not increase either.
Historical qdisc drops exist; they are not new drops in these windows. Two
ICMP input errors per host appeared in each window; their packets were not
captured and their relevance is unknown.005 recorded one TCPAbortOnData in
120s, not attributed to an EFS socket or to the004→003 retransmissions.

## Extended PHY evidence and limits

|60s physical counter |003 |004 |005 |
| --- | ---: | ---: | ---: |
| rx_corrected_bits_phy |65,328 |10,108 |75,866 |
| Sum of raw rx_err_lane_0..3_phy |65,328 |10,108 |75,866 |
| rx_pcs_symbol_err_phy, rx_crc_errors_phy, rx_symbol_err_phy |all0 |all0 |all0 |
| rx/tx_discards_phy, rx_out_of_buffer, rx_buff_alloc_err, rx_wqe_err |all0 |all0 |all0 |

Physical raw errors are occurring and FEC corrections increase. The mlx5
counter documentation distinguishes these pre-FEC/corrected bits from
uncorrected symbol errors
([mlx5 counter definitions](https://docs.kernel.org/networking/device_drivers/ethernet/mellanox/mlx5/counters.html)).
Do not describe the physical link as error-free merely because the generic
IB error counters stayed0. However, measured uncorrected errors and packet
reject/buffer counters stayed0;005 has the most corrected bits and no TCP
retransmissions in this cohort. Corrected bits alone do not demonstrate that
a cable/transceiver fault caused004→003 retries. rx_bits_phy is not exposed
here, so no normalized BER or threshold-based link-health verdict is calculated.

## Findings and remaining causal work

**Established:** RDMA mode intentionally carries oversized EFS frames/replies
on TCP; the currently deployed84KiB threshold is confirmed by matched source.
TCP/IPoIB retransmission episodes on004→003 are repeatable and timer/loss-probe
associated. They coexist with successful FEC correction and unchanged sampled
packet-drop/uncorrected-error counters.

**OPEN:** the precise packet/ACK loss or delay point, and which live EFS message
types occupy each oversized frame. No evidence justifies claiming RDMA is
broken, blaming fq_codel/ring exhaustion, replacing a cable or changing buffers,
offloads, IPoIB mode, deadlines or transport. No payload correctness was tested.
The next causal evidence would be synchronized non-promiscuous header/sequence
capture or an existing retransmit/drop trace plus relevant switch-port counters.
Those were NOTRUN; they would need a separate approved observation mechanism,
not a tuning experiment. No site-wide fabric scan was performed.

Raw evidence remains outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/orcd-tcp-rdma-cause-20261013`
and `.../orcd-tcp-rdma-extended-20261013`.
First summary SHA256:
`47b523658adac20a5361a46999419c58da64a87180863811d3aca071621698ec`;
second:`e897a873148826a57f571f1e834c97733d1ef6895048431c82459b1ae33c6114`.
Only reproduction source and this evidence summary are committed. Local Python
syntax and architecture documentation checks PASS; no product code changed.
