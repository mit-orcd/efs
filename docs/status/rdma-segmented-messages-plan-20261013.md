# Agreed RDMA large-message design — 2026-10-13

**Design agreed; implementation and qualification NOTRUN.** This records the
human decision following the [ORCD TCP/RDMA investigation](orcd-rdma-tcp-cause-20261013.md).
No product code, running cluster, network setting or deployment was changed for
this plan. It is a transport correctness proposal, not performance acceptance.

## Current behavior and intended contract

The reviewed devel source is `5cf9fb40581de531142b7a6f74fb531a568fdcb7`.
The deployed ORCD source matched during the preceding investigation was
`9b2fac55fc69`; see that report for source hashes and environment evidence.
Currently `EFS_TRANSPORT=rdma` requires successful upgrade but permits oversized
application frames over TCP. The negotiated RDMA frame limit is at most
84 KiB (86,016 bytes), including wire headers. Metadata batches as well as file
data can exceed it. Server replies normally follow the request's channel,
unless their size requires TCP.

The agreed new contract for `EFS_TRANSPORT=rdma` is:

- TCP is used only for connection bootstrap/setup, not application metadata,
  file data or replication messages after successful setup.
- Large requests and replies are segmented over RDMA within supported logical
  message limits. Oversize alone never selects TCP.
- RDMA transfer failures fail the operation and retire the connection. A
  partially transferred message is never continued or silently replayed over TCP.
- Explicitly configured TCP remains a separate supported transport. Other
  transport-mode policies must be stated and tested before implementation is
  accepted; they must not weaken the strict RDMA contract.

## Upgrade decision

Rolling upgrades and mixed-version operation are **not required**. The cluster
can be stopped, the same qualified new binaries installed on every client and
server, and then restarted. Check a new protocol version/capability at setup
and reject incompatible peers with a clear diagnostic. Do not implement legacy
oversized-message TCP fallback to support older peers.

This is authorization for the design decision, not a deployment performed by
this documentation change. Running clusters remain unchanged until deployment
is separately requested.

## Proposed implementation

Keep the existing 84 KiB registered transport buffers. Add a negotiated
segmentation envelope containing message identity, total logical length,
segment offset/length and final-segment indication. Both directions need the
same framing support. Validate lengths, integer arithmetic, offsets, complete
coverage and message identity before exposing an assembled message to a handler.

Use bounded receive credits and prompt receive-buffer reposting so a large
message cannot exhaust the posted receives or deadlock the sender/receiver.
Credit/control handling must remain able to progress while a sender is blocked.
Enforce per-connection and global reassembly bounds; reject/refuse unsupported
sizes safely rather than allocating up to the general wire-format maximum.
Where practical, receive into the caller's destination to avoid a second full
payload allocation. Preserve original operation deadlines across all segments;
do not give each segment a new full deadline.

Completion, timeout, cancellation, allocation failure and QP teardown must
release partial assembly state and credits safely. Registered send-buffer and
zero-copy source lifetimes must extend through their completions. Reconnection
must not expose segments from an earlier connection generation.

Changes span `src/common/protocol.c`, `src/common/rdma.c`, the RDMA interfaces,
and `src/server/handler.c`, which currently consumes one receive-pool frame as
one complete request. Audit all specialized receive-into/reply paths and direct
registered reply-buffer paths, not only the generic send/receive functions.
Reconcile the separate 16 MiB RDMA receive checks with the new bounded logical
message policy; the general `EFS_MSG_MAX_LEN` is 3 GiB and is not a safe default
reassembly allocation limit.

Increasing the buffer size alone is not the recommended fix: it only moves the
cutoff and scales per-connection allocation. Defaults currently reserve four
receive and 32 send slots. One-sided RDMA READ/WRITE is a possible later design,
not required for this initial correctness change.

Add observable message/byte counters distinguishing RDMA application traffic,
TCP setup and any explicitly permitted non-RDMA traffic. These must establish
the contract without treating unrelated host TCP traffic as EFS traffic.

## Qualification plan (NOTRUN)

Use the same candidate source/build identity for RDMA and explicit TCP controls.
Record binary hashes, device/GID/port, topology, feature flags, storage mode,
quotas, concurrency, deadlines and cleanup. Keep raw artifacts outside Git.
No deadline widening, connection-window tuning or performance acceptance is
part of this work.

| Stage | Cluster | Required checks |
| --- | --- | --- |
| Native/protocol | Isolated build/test environment | Boundary lengths, malformed envelopes, overflow/offset checks, partial delivery, allocation failures, deadline/cancellation cleanup, connection generations and explicit version mismatch. ASan/UBSan and negative controls. |
| Software RDMA | AMD Soft-RoCE | Private stores, ports, processes and mounts; non-skipped RDMA device preflight. Small request/large reply and large request/small reply, concurrent bidirectional transfers, minimum receive credits, slow receivers, interrupted transfers and repeated reconnect/teardown. |
| Hardware RDMA | ORCD-test native IB | Isolated candidate after authorization, actual cross-host traffic. Large GET_CHUNK replies, metadata batches and server/server replication, including concurrent load; exact contents, EOF, persistence and transport counters. |
| Filesystem acceptance | Both | Same-source TCP baseline, full POSIX, peer and concurrent suites; ORCD additionally runs all required `.efs` gates. Packing, containers and expiry disabled; direct I/O. AMD ordinary-file gates use `.efs` disabled, with feature-specific tests explicitly identified. |

Test complete frames immediately below, at and above the negotiated 84 KiB
limit, accounting for both envelope and application headers. Exercise 128 KiB,
1 MiB, 16 MiB and 64 MiB logical payloads where the operation supports them;
also test the implemented logical maximum and safe refusal just beyond it.
Existing `tests/test_rdma_xprt.c` deliberately expects oversized TCP transport:
replace that expectation for the new protocol and add small-request/large-reply
coverage. `tests/live/rdma_posix_gates.sh` is a starting point for isolated AMD
gates, not cross-host ORCD proof. Extend committed harnesses with segment/fault
cases and exact reproduction commands before reporting acceptance. Existing
RDMA benchmarks reject oversize and cannot establish the new behavior unchanged.

Pass requires exact byte/EOF/persistence correctness, no partial message exposure,
bounded assembly/registered resources, safe failure and recovery, and **zero
EFS application-data TCP bytes after setup in strict RDMA mode**. Use connection-
attributed traffic plus application transport counters; bootstrap TCP is expected.
AMD proves software transport behavior; ORCD proves native IB cross-host behavior.
Neither a compilation result nor a skipped RDMA test closes these gates.

For eventual deployment: drain and stop clients cleanly, stop daemons, install
identical qualified protocol binaries everywhere, then start daemons and clients.
Preserve stores and retained acceptance fixtures. Do not reset data for an upgrade.
