# AMD paused-follower RDMA read — 2026-10-14

W106; `devel-rdma`. Fix pending exact-source AMD qualification. Files/direct,
strict segmented RDMA/rxe0/GID1 on AMD-395, four same-host nodes.
`.efs` client/server, packing, containers, expiry and repair experiment OFF;
metadata balancing1. No TCP comparison or push.

## Baseline and diagnosis

The [namespace qualification](rdma-namespace-admission-20261014.md) records
`a7e2b6b7d` full native/core/peer/cold/EOF PASS but paused follower2 read FAIL:
first8MiB verified, next8MiB read-length assertion failed (actual returned count
not logged). Earlier merged ac22 stalled after24MiB at180s, exit137. These remain
failed evidence, not relabelled by later passes.

On the installed a7 binary, an unchanged32MiB/180s diagnostic that logs each
requested/returned read completed all four8MiB buffers; deterministic payload
SHA256 `3cab1a5c8ff63b5d9d7375234c03a5fcae7beabdb5b600210586d07aa1deb586`.
Each buffer was about22.6s. Failure is intermittent, not universal corruption.
Raw evidence: `/data1/efs/rdma-paused-read-a7/diagnostic*`. The diagnostic adds
byte-count output but retains every original length/block/EOF assertion.

Retained failure logs show repeated segmented-RDMA connection setup failures
against the paused daemon, about2.5s per group of attempts. Fragment preferred
pair has an unchanged5s budget and admission reserves half for its other peer.
Connection cancellation at this nested deadline correctly avoids a global
peer-health verdict. However, later chunks still select the same unavailable
preferred fragment and repeat setup before using parity. A single parent read
can spend most of its30s budget probing that candidate. The logs establish this
repeated work, not a complete explanation of every previous short read.

## Fix

A bounded advisory read-peer order moves failed NET/BUSY preferred GET peers
behind the other captured fragment candidates. The hint expires at that
initiating read's already established deadline (no new timer, no extension).
Active hints are not refreshed by later chunks; a successful verified fragment
clears its peer's hint. Invalid IDs/expired callers add nothing. Atomic updates
allow concurrent chunk workers to share this scheduling observation.

This is neither global node-health state nor a new placement/quorum decision.
If the two preferred candidates fail, the deferred candidate is still attempted.
Every fragment still checks generation, length, captured digest and payload
hash; reconstruction still requires two good fragments. Packed reads unchanged.
Read30s, preferred pair5s, admission5s, cooldown30s and outer180/k5 unchanged.
The existing strict four8MiB reader assertions are retained; short read failures
now log the actual returned count and offset before raising.

Focused local checks PASS:

```sh
python3 tests/test_read_peer_order.py
python3 tests/test_get_pair_budget.py
python3 tests/test_get_pair_readiness.py
```

The production ordering header regression covers alternate data/parity order,
remaining fallback eligibility, exact expiry, no perpetual refresh, explicit
recovery, invalid IDs, expired callers and eight concurrent hint writers.
Existing pair tests retain original I/O budget/TLS restoration and CREDIT-only
readiness/ownership checks. Full native and fixed AMD live gates remain NOTRUN.

## Reproduction / remaining gates

Use the frozen candidate source/build manifest; export/build/deploy normal
binaries as in the [namespace ledger](rdma-namespace-admission-20261014.md).
Finish builds before measurements. On AMD with installed sources and PID files:

```sh
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
OUTPUT=$(mktemp -d /data1/efs/rdma-paused-rerun-XXXXXX)
for r in 1 2 3; do
 if python3 -u /data1/efs/src/tests/live/rdma_degraded_read.py \
    /data1/efs/src /data1/efs/mnt "$OUTPUT/r$r" > "$OUTPUT/r$r.log" 2>&1; then
   code=0
 else
   code=$?
 fi
 echo "$code" > "$OUTPUT/r$r.exit"
done
```

Prerequisites: four files/direct daemons, ordinary writable installed mount,
nonroot efs user with PID ownership, enough space for32MiB fixture. Script
selects node2 or3 only after proving FOLLOWER in both groups; verifies PID/node
identity, pauses it, opens a fresh independent RDMA mount, verifies all256
128KiB blocks/four8MiB buffers/EOF and prints the digest. Restores SIGCONT first,
drains reader, deletes only its exact owned namespace. No fixture store wipe.
For a fresh4G/node full load/peer/cold/EOF/degraded rerun use the committed
`tests/live/rdma_four_node_qualification.sh` at unchanged budgets, no tracing.

Require repeated exact-source degraded byte/digest PASS plus affected quiet
namespace/read/peer/cold gates. Until recorded, W106 remains OPEN. Do not turn
this advisory routing fix or software RXE evidence into hardware acceptance.
