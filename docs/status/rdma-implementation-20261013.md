# RDMA implementation foundation checkpoint

Subsequent transport integration and AMD qualification: [current ledger](rdma-soft-roce-integration-20261013.md).
The following describes the historicale6d1ecda foundation, not the current branch runtime.

Branch `devel-rdma`, created from integrated devel `fc341f00`.
Foundation source: `e6d1ecda64fd4a38ed814ff5432d786abed9d56a`.
Contract: [segmented transport plan](rdma-segmented-messages-plan-20261013.md).

**Foundation PASS; enabled transport implementation and qualification NOTRUN.**
The new shared module is in the common build, but existing setup/data paths do
not call it yet. This commit does not eliminate oversized TCP fallback and must
not be deployed or described as strict segmented RDMA. AMD's four-node working
files/TCP/direct deployment at96f40602 remains unchanged; commands, packing,
containers and expiry remain disabled. No push or native IB deployment.

## Implemented

`include/efs/rdma_message.h`, `src/common/rdma_message.c`:

- Explicit big-endian 72-byte setup codec, separate transport version from
  address version. Exact size/version/capability/reserved-field checks, IB LID
  and RoCE GID validation, QPN/PSN/MTU checks. Negotiate physical/logical/queue
  minima; preserve actual peer receive count rather than assuming symmetry.
- Explicit 32-byte DATA/CREDIT envelope. Ordered connection-local nonwrapping
  message IDs, checked extents and exact FINAL semantics. Validate original
  wire length before allocation; require first segment to include all five
  logical-header bytes. Unknown application opcodes remain handler policy.
- One partial assembly plus a bounded complete-message FIFO (maximum four).
  Caller passes immutable per-connection and shared process budgets; process
  reservation uses atomic CAS. Completed messages retain charges until release.
  Any malformed/refused stream poisons the owner and releases its partial and
  queued messages; other connection reservations remain intact. Repeated
  destruction releases nothing twice. A single progress owner is required.
- Credit state tracks sends and cumulative successful remote reposts. Reserve
  one receive for controls; reject fewer than two receives, replay, inflation,
  and counter wrap. Local SEND CQE supplies no credit.
- Candidate syntax cap128MiB includes a64MiB fragment plus headers. This is not
  the operational resource policy: the assembler requires explicit budgets.
  Complete snapshot/metadata/query message inventory and negotiated default
  budgets must be reviewed before enabling it. Oversize must fail safely.

## Evidence and reproduction

Mac native and AMD Ubuntu native: PASS. AMD compiled both module and tests with
ASan/UBSan, leak detection and halt-on-error: PASS. No RDMA device required for
these pure codec/ownership gates. Device preflight `rdma link show` separately
shows `rxe0/1 ACTIVE LINK_UP` on wlp194s0; it is NOT live-transfer evidence.

```sh
make test-rdma-message
# Linux, from an exact source export:
cc -std=c99 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  tests/test_rdma_message.c src/common/rdma_message.c -pthread \
  -o /tmp/efs-rdma-message-san
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 /tmp/efs-rdma-message-san
rm /tmp/efs-rdma-message-san
```

`tests/test_rdma_message.c` has independent golden setup/envelope bytes, short
and extra setup rejection, unsupported/invalid negotiation, unequal capacities,
physical-boundary payloads and128KiB/1MiB/16MiB/64MiB+5 logical frames, exact-byte
comparison, no partial exposure, overlap/gap/ID/total/header errors, queue/global/
local refusal, allocation failure, replay/inflation credit refusal and eight
owners performing1000 reservations each. All budgets return to zero.
Negative control: in an isolated copy remove only
`if (h.offset!=r->received) return fail(r);`, rebuild the same regression,
expect a nonzero assertion exit in the valid-envelope overlap/gap cases.
This negative control failed as expected on AMD, so the oracle detected it.
Raw binaries/output outside Git: `/data1/efs/rdma-foundation`.

## Remaining implementation, in order

1. Replace both old setup paths with the new contract at once; define global
   defaults after complete supported-message inventory. Test strict rejection
   and bootstrap-only auto fallback. Existing runtime negotiation is unchanged.
2. Single-owner verbs progress routine, DATA/control credit reserve, prompt
   repost-before-credit, send completion ownership, one original deadline,
   teardown/generation safety and simultaneous bidirectional send tests.
3. Segment header/part1/part2 without a second full send allocation. Connect
   complete-message views to all protocol receive/watch/receive-into variants
   and handler dispatch; audit registered reply fast paths and lifetime.
4. Strict channel selection, no application TCP after setup, Raft application
   pipeline independent of physical credits, complete-reply counting and
   traffic/resource counters. Do not change consensus/durability semantics.
5. Native adapter faults/sanitizers; isolated AMD Soft-RoCE transport tests,
   then same-source full files TCP/RDMA filesystem gates. Native cross-host IB
   remains separately authorized and required for hardware acceptance.

The pure credit model does not prove control reserve progress under verbs load;
codec tests do not prove connection pinning, deadlines, CQ handling, receive-into
canaries, source lifetimes, persistence, or zero application TCP. These gates
remain NOTRUN. No speedup claim.
