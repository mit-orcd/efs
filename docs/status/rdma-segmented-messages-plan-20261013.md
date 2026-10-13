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

## Implementation instructions from code review

**Review baseline:** `devel` at `9d7cf3ce0d41fc0f87d4fa66b1a37a88ecdcb606`.
This extends the design recorded in `34bb1811`; it does not report implementation
or runtime qualification. The reviewed transport/handler/Raft files match that
commit. Concurrent uncommitted client admission edits were outside this review
and must be preserved. All implementation gates below remain **NOTRUN**.

### 1. Define and negotiate the transport contract first

Update `include/efs/protocol.h` setup structures and
`efs_rdma_client_upgrade()` / `efs_rdma_server_accept()` in `src/common/rdma.c`.
Current setup accepts historical 20-byte IB requests and uses `address_version`
for the RoCE GID extension; neither proves segmented-message support.

- Introduce a distinct transport protocol version/capability. Keep address/GID
  version separate. Require exact supported setup lengths and validate every
  negotiated field before publishing the upgraded connection.
- Advertise physical frame capacity, logical message maximum, actual posted
  receive count and bounded outstanding-message capacity. Do not assume both
  ends configured the same receive count as `efs_rdma_conn_nrecv()` currently
  documents. Negotiate compatible minima and reject impossible configurations.
- In strict mode reject legacy/unsegmented peers. A HELLO build-ID match is not
  a substitute for this contract. Complete setup/reply over TCP before enabling
  application RDMA sends; no application bytes may leak into bootstrap.
- State `auto` policy explicitly. Recommended: fallback is a connection-setup
  decision only; after a segmented RDMA upgrade, failure retires that connection.
  Do not preserve mixed application channels to make strict mode work.

**Gate:** native golden setup encodings; unsupported version/capability, short/
extra payloads, invalid receive counts and asymmetric peer limits. Tests must
cover IB and the GID extension without reusing address_version as a message
version. No old-peer compatibility shim is required.

### 2. Build one logical-message layer above the physical verbs frames

Keep `efs_rdma_recv_frame()` / `efs_rdma_recv_repost()` as physical-buffer
primitives. Add a shared bounded assembler/sender module under `src/common/`
with a narrow interface in the RDMA/protocol headers and Makefile integration.
Both `protocol.c` and `handler.c` must use it. Avoid separate metadata and
fragment implementations or storage/Raft-state-machine changes.

Encode a versioned DATA/CREDIT envelope explicitly in wire byte order; do not
send a native C struct. DATA carries message ID, total logical frame length,
segment offset, segment length and final indication. Segment the existing
`len + type + payload` logical frame, so application opcodes/serializers retain
one format. Use checked subtraction to compute usable bytes from negotiated
physical capacity after envelope overhead. A segment must always fit its MR.

For the initial implementation, serialize DATA messages in each direction on a
connection, allow CREDIT records between their segments, and use one active
assembly per direction plus a bounded completed-message queue. Preserve full
duplex. RC ordering permits strict sequential offset validation: accept only
`offset == bytes_received`, unchanged ID/total, nonzero progress, and FINAL
exactly at total. Reject gaps, overlaps, duplicates, unexpected new IDs and
invalid flags; do not silently truncate or expose a partial logical message.
Scope IDs to a connection incarnation; fail rather than wrap into a live ID.

Validate the first logical header against the envelope total before reserving
the remaining assembly. Define a negotiated logical cap and per-connection and
process-wide reservation budgets. Inventory fragment PUT/GET headers, metadata
batches, Raft entries/snapshots and query export messages before choosing caps.
A 64 MiB fragment requires MORE than 64 MiB of framed space. The 3 GiB
`EFS_MSG_MAX_LEN` is a syntax limit, not an allocation budget. Reserve budget
before malloc/copy and release it exactly once. On refusal after consuming part
of a frame, retire the connection; an application BUSY reply alone cannot
resynchronize an incomplete stream.

**Gate:** independently generated envelope fixtures; header/envelope boundary
mismatches, integer overflow, zero-length segment, non-final last byte,
out-of-order/duplicate/overlapping segments, changed ID/total, queue/global
budget refusal and allocation fault at each ownership transition. ASan/UBSan.

### 3. Make credit progress independent of application replies

In `src/common/rdma.c`, review `harvest_recv()`, `pend_push()` / `pend_pop()`,
`send_buf_pick()`, `reap_sends()` and all receive waits together. The shared
poller currently queues a completion; it does not repost a buffer until the
consumer releases it. Thirty-two local send slots do not mean the peer has
thirty-two available receive slots. A local SEND CQE is not a remote repost.

Maintain peer DATA credits and a control reserve, based on negotiated receive
capacity. Return credit only AFTER copying/consuming a physical segment and
successfully reposting its buffer. Use checked cumulative/sequence accounting;
reject credit inflation/replay. Control records must not require the data
credit they replenish. Specify and test the minimum supported receive count;
reject an insufficient count at setup instead of relying on RNR retry.

A credit-blocked sender must service incoming DATA/CREDIT records and send CQEs
within the same operation deadline. It may queue complete inbound messages,
but must not recursively invoke application handlers. Provide a shared
connection progress routine with a single ownership/serialization rule for
pending receives and sends. Prove progress when BOTH peers are sending large
messages; merely waiting for an application reply between segment windows
would deadlock this case.

Do not run blocking credit waits, allocation/reassembly or application handlers
under `g_reg_lock`, the registry poller callback, Raft `h->mu` or outbox locks.
Preserve `reg_gen` checking in receive completion dispatch. If processing moves
outside the registry lock, pin the connection lifetime before releasing that
lock; teardown must not free state under the progress owner.

**Gate:** unequal receive counts, minimum supported credits, slow consumers,
all send slots busy, control reserve saturation, two simultaneous large sends,
credit corruption and QP failure while waiting. All buffers and reservations
must return to their baseline after repeated failure/reconnect cycles.

### 4. Route every API through complete-message semantics

| Code entry point | Required integration |
| --- | --- |
| `protocol.c: conn_pick_send_chan()` and `efs_conn_send_msg_parts()` | Strict upgraded connections select RDMA regardless of logical length. Send wire header/part1/part2 through segmented transport without constructing a second full contiguous payload. Remove size-based TCP selection for strict mode. |
| `protocol.c: conn_rdma_frame()` / `efs_conn_recv_msg()` | Replace one-pool-frame decoding with bounded complete-message acquisition. Preserve caller-owned malloc payload and free semantics; do not allocate twice for large messages. |
| `efs_conn_recv_msg_into()` | Assemble header and data into their separate caller destinations with checked extents. Preserve status-only errors and exact successful lengths; do not overwrite body buffers on an error reply. |
| `efs_conn_recv_u8_reply()` / `efs_conn_recv_put_reply()` | Consume one complete logical reply, never a CREDIT/envelope. Preserve PUT status/path and any cached small-reply ownership. |
| `efs_conn_reply_watch*()` / `efs_conn_put_reply_watch()` | Consume transport control/progress without declaring it an application reply. Preserve nonblocking/bounded harvest behavior; a partial large reply must not monopolize a client waiting on several replicas. |
| `handler.c: efs_handle_conn()` | Replace its direct `recv_frame` decoding and 16 MiB gate with the shared assembler and negotiated logical cap. Dispatch only complete, validated application requests. |

After strict upgrade, unexpected TCP application bytes are a protocol violation,
not `EFS_ERR_AGAIN` permission to read a fallback message. Retain TCP EOF/HUP
monitoring for connection lifetime; spurious readiness still needs the existing
nonblocking peek treatment. Explicit TCP continues through its existing path.
Audit low-level `efs_send_msg*` / `efs_recv_*` callers: establish which are
bootstrap or TCP-only and which must use an upgraded connection wrapper.

The handler's `rdma_frame` flag currently holds a receive buffer until after
its switch. Replace that with an explicit message view/release contract. Return
physical segment buffers promptly, but retain assembled payload ownership until
the synchronous handler finishes. Every early break/error path must release
its view. Audit asynchronous consumers before retaining any pointer.

**Gate:** small request/large reply, large request/small reply, both large,
status-only errors, exact EOF/bytes, receive-into canaries and several-replica
harvest with one stalled large reply. A raw segment/CREDIT must never appear as
an application reply or increment its completion count.

### 5. Preserve send completion and registered-buffer lifetimes

`efs_rdma_send_frame()` currently returns after posting, and `send_buf_pick()`
reuses slots only after CQEs. Its W39 two-SGE path references a registered
client slab until reply completion, quiesce or connection destruction. Keep
that ownership invariant across ALL segments; posting the final segment is
not send completion. Consume one original operation budget throughout
segmentation, credit waits, receive assembly and quiescence. Do not restart
`EFS_RDMA_SEND_WAIT_US` or a full receive timeout for every segment/window.

In `handler.c` GET_CHUNK, preserve the small registered reply-buffer fast path
through `efs_rdma_send_buf()` / `efs_rdma_send_commit()`, accounting for envelope
space. Large replies use the shared segmented path from existing reply storage;
that fallback must remain RDMA. Ensure TLS reply storage is not overwritten for
the next request while a zero-copy send still references it. Initially copy
segments into pool slots if lifetime cannot be proven; optimize copying later.
Check return values from both reply-send routes and retire failed connections.
A CQE proves transport progress, not fragment disk durability or a Raft commit.

**Gate:** copied and W39 sends crossing header/part1/part2 boundaries, source
reuse attempts before completion, all registered slots busy, completion errors,
allocation failure, cancellation and teardown at first/middle/final segments.
Destroy/stop the QP and drain/cancel ownership before freeing MRs or assemblies.

### 6. Adapt replication pipelining without changing durability

Review `host_sender_depth()` and `host_sender_main()` in
`src/server/raft_host.c`. They currently limit WHOLE RPCs to receive buffers
minus one and count replies separately by TCP/RDMA channel. Those assumptions
cannot serve as segment credit accounting.

Separate the existing outstanding application-RPC limit from physical segment
credits. In strict mode every EFS_MSG_RAFT and transport reply uses segmented
RDMA; count one reply per complete RPC, not per segment or CREDIT. Preserve
FIFO and bounded queued complete messages while earlier replies wait. Keep the
explicit TCP path independent. Update mixed-channel comments/observations so
strict mode does not accidentally restore the removed side channel.

Do not reinterpret `EFS_MSG_RAFT_REPLY`, W97 flow feedback, SEND CQEs or credits
as durable consensus ACKs. Keep Raft's original index/term/applied/durable
watermarks and retries. A failed partially sent logical message breaks that
connection; higher-layer retries retain their existing identity/dedup rules.
Do not add transport replay that invents a new operation identity.

**Gate:** two groups sharing a peer, small control messages around large
AppendEntries, pipeline depth greater than one, concurrent reply traffic,
mid-message disconnect, leader change and cold durable replay. Test large
metadata batches and supported snapshot transfers as well as fragment traffic.

### 7. Add proof counters and close the gates in this order

Expose physical RDMA frames/bytes, complete application messages/bytes,
segmentation counts, current/peak assembly bytes, credit stalls and failed
assemblies. Separately count TCP bootstrap and TCP application traffic. Count
all paths, including direct registered replies and specialized receive APIs;
wire transport bytes and logical payload bytes need distinct definitions.

1. Native codec/assembly/ownership tests and sanitizer fault tests.
2. `tests/test_rdma_xprt.c`: replace oversized-TCP assertions with complete
   segmented RDMA assertions, add asymmetric request/reply sizes and protocol
   mismatch. A skipped device test is NOTRUN, not PASS.
3. AMD Soft-RoCE isolated transport/fault gates, then same-source TCP/RDMA
   full filesystem suites. Extend `tests/live/rdma_posix_gates.sh` with the
   small-request/large-reply and no-application-TCP proofs.
4. Authorized isolated ORCD-test native IB cross-host qualification, including
   replication and all required .efs gates from the matrix below.
5. Only after exact byte, EOF, persistence, resource/failure and traffic proofs
   pass, perform the separately authorized whole-cluster deployment.

Counters plus EFS-connection-attributed traffic must show zero application TCP
bytes AFTER successful setup. Existing benchmarks that reject oversize must be
extended before using them as evidence. Do not promise a speedup from this code
review: copy cost, storage barriers and contention need separate measurements.

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
