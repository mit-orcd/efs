# ORCD-test passive network observation — 2026-10-13

**Observation completed; root cause OPEN.** No redeployment, restart, mount,
new I/O workload, profiler activation, process signal, application tracer,
network tuning or remote output file. Existing heavy I/O continued throughout.
This does not verify file contents or close correctness/acceptance failures.

## Method and identity

Four servers fcstor003–006 and seven clients fcstor007–013;120s observation
per host,13 samples at nominal10s intervals. All11 process identities stayed
unchanged; all versions were `9b2fac55fc69` on `devel`. Servers built18:39:13Z
or18:39:29Z, clients18:39:29Z–18:40:02Z on2026-10-13. Actual `/proc` environment
selects strict `rdma` and `mlx5_2`; server argv selects files/direct-I/O. Existing
server/client commands enabled, packing/expiry OFF. Nothing was toggled to
match Intel's disabled-command profile. Containers are not selected by servers.

Active fabric `mlx5_2/1`: native InfiniBand,200Gb/sec (4X HDR), ACTIVE/LINK_UP.
All four HCA devices were read; interpretation below selects the configured
EFS device. TCP/sysfs interface counters are separately host-wide. Hardware
and protocol counters may include other users' traffic on these shared nodes.

Read-only sampler source `737354e2` streamed over SSH stdin, with local output
only. It reads sysfs, procfs, process argv/environment and version, plus fresh
bounded stderr log ranges. No remote script/output is installed. All143 sample
collections succeeded, with no read errors or negative/reset counter deltas.
All log ranges were available and untruncated (32–206KiB/server,24–59KiB/client).
A subsequent30s two-host TCP socket/softnet observation was also read-only.

First pass used11 concurrent SSH observers. The committed driver now defaults
to4 (maximum8) to bound session pressure around existing monitor connections;
remote sampling logic is unchanged. Future cohorts may therefore have different
wall-clock windows. Reproduction from this repository, with SSH aliases and
nonroot erbmi1 access to the same nodes:

```sh
python3 scripts/orcd_network_observe.py --seconds 120 --interval 10 --jobs 4 \
  --output /outside/git/orcd-network
python3 scripts/orcd_tcp_observe.py --output /outside/git/orcd-network
```

Requires node Python3 and ss, readable RDMA/sysfs counters and existing logs;
no new root packages or permissions were needed. SSH through orcd-login is
used. The login host's older Python3.6 rejects `subprocess(text=True)` in an
initial ad-hoc socket query; that query was corrected to universal_newlines.
The actual observers run Python on compute nodes and succeeded normally.

## Two-minute deltas

Every configured mlx5_2 port had **zero** increases in port_rcv_errors,
port_xmit_discards, symbol/link errors, link-down events, local_ack_timeout_err,
rnr_nak_retry_err, req_transport_retries_exceeded, req_rnr_retries_exceeded,
out_of_buffer, packet/out-of-sequence, duplicate-request and CQ error/flush
counters. Standard interface drop/error/missed counters also stayed unchanged
on all11 hosts. This is no observed error/retry indication in this window,
not proof of a loss-free fabric at all times.

| Host | IB TX packets | IB RX packets | Host TCP retransmissions | Host TCPTimeouts | Logged successful RPCs with attempts>1 and elapsed>=1s |
| --- | ---: | ---: | ---: | ---: | ---: |
|003 |5,874,212 |5,304,123 |0 |0 |0 |
|004 |8,145,420 |6,075,525 |663 |13 |0 |
|005 |6,440,540 |5,750,473 |64 |1 |0 |
|006 |6,323,748 |5,735,492 |0 |0 |0 |
|007 |1,678,773 |2,441,301 |0 |0 |5 |
|008 |1,219,078 |1,772,397 |0 |0 |4 |
|009 |1,070,718 |1,555,694 |0 |0 |2 |
|010 |917,253 |1,337,399 |0 |0 |5 |
|011 |1,082,653 |1,572,779 |0 |0 |7 |
|012 |1,073,557 |1,559,826 |0 |0 |4 |
|013 |2,579,578 |3,352,261 |0 |0 |97 |

`port_xmit_wait` increased4,640 counter units on005 and0 elsewhere; no conversion
to elapsed time or loss is made. Packet/data increments establish substantial
existing fabric traffic. These port-wide counts do not identify particular
EFS request types, and traffic observations do not measure file correctness.

Host TCP OutRsts, InErrs and abort-on-timeout increments were0 everywhere.
TCPFastRetrans stayed0 even on004/005. Host TCP counters cannot independently
attribute retransmissions to an application or prove physical wire loss.

## TCP attribution follow-up

`ss -tinpe` found retransmitted established sockets owned by running `efsd`
processes on port17432. These are **EFS TCP traffic despite the RDMA profile**;
do not dismiss the host TCP counters as SSH noise or infer RDMA fallback from
them. Exact message types were not decoded.

A subsequent30s before/after comparison, matching stable socket inode IDs:

| Host | Stable EFS sockets | TCP bytes sent over those sockets | Retransmission increase | Software receive drops / time-squeeze increase |
| --- | ---: | ---: | ---: | --- |
|004 |419 |602,094,060 |102 |0 /0 |
|005 |464 |112,609,605 |0 |0 /0 |

The102 retransmissions are51 each on two004→003 connections:
`172.16.223.58:17432 → 172.16.223.57:57438` and`:57454`, owned by
`efsd` PID2605437, FDs723/725. Their socket inode IDs stayed stable. Route to
003 uses `ibs1f0`; its PCI device and mlx5_2 share `0000:88:00.0`. TCP/IPoIB
therefore uses the same physical InfiniBand device, but its TCP retransmission
counters are distinct from RDMA transport counters. The supplementary window
is not part of the earlier120s totals. Short-lived socket tails can be missed.

No packet capture or active probes were taken; no causal explanation for TCP
retransmissions is established. Delayed acknowledgement, software processing,
queueing or loss cannot be distinguished with these counters alone. Zero
hardware error counters and software drops are insufficient to clear TCP
retransmissions, and these observations do not establish a performance bottleneck.

## Existing EFS retry evidence

Fresh log ranges contain **124** successful slow RPCs with more than one
attempt, mostly on013 (97). They contain0 logged inode-RPC send/recv/no-hint
transport retries,0 retry-exhaustion/EBUSY/stop-refused/unmount-loss lines.
Samples on007 include REPORT_CHUNKS type67, attempts2–3, saw_busy1 and status0;
013 includes attempts2 with saw_busy1 and attempts2 with saw_busy0. Those
flags show that not all multi-attempt success can be assigned to the same
cause; no physical-packet-loss explanation is inferred.

EFS_RPC_PROF was not enabled on any observed process and remains unchanged;
there is no complete busy_n census. Slow-ok logs omit short retries, while
other internal/data/lease/server retries can be unlogged. Thus124 is an
observed event count, not the total number of retry attempts. No terminal
failure in these log patterns is not a PASS for every operation being run.

## Evidence and verdict

Raw local-only evidence:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/orcd-network-passive-20261013`.
Main summary SHA256:
`d3a5a1a003ab4bd5b17b33d2cc85fbe951a09d604cf3a5c16bc0b917e806e539`.
Perhost samples record timestamps, identities, raw counters, log offsets and
collection errors; supplementary socket snapshots are kept outside Git.
Reproduction uses the committed scripts, not a private capture. No observer
processes persist after completion and no remote cleanup was necessary.

Read-only observation **PASS**. Hardware IB/RDMA error increases **not observed**.
EFS-owned TCP retransmissions and successful application retries **observed**;
causal investigation remains **OPEN**. No Intel-style test payload was generated
or verified, and no workload, transport, deadline or deployment was changed.
