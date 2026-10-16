# AMD paused-follower RDMA read — 2026-10-14

W106; `devel-rdma`. Fixed-source AMD affected gates PASS; hardware and
broader W106 acceptance remain OPEN. Files/direct,
strict segmented RDMA/rxe0/GID1 on AMD-395, four same-host nodes.
`.efs` client/server, expiry and repair experiment OFF;
metadata balancing1. No TCP comparison or push.

## Fixed-source qualification

Deployed code **`ade336dafd9a1046d7386f7109c61bb3eef4d86c`**, native build
2026-10-14T04:43:30Z (Oct 14 00:43 EDT), branch `devel-rdma`.
Clean full AMD native `make test` PASS, exit0. Normal and private fault client
come from this exact source; fault-enabled client used only for the owned EOF
oracle, no faults on installed clients. Binary SHA256:

- efsd `66f48899e00f83473b9e4b3c7ae783cbe81d3453d759fb9f8a7c959e15e0f556`
- efs-fuse `2c4cdb689ed4f130adc9a2b6e2898fd8c50153c030790df2996e24152f3b611b`
- efs-mgmt `05087b17665eea57b67b40be8fde6ee4126f980d36ec11bd67fd5a9b1a84fdcf`
- private fault-client `0611533fe5c572fda8561fe9cdbcbe5f1de98ec412a8ab9b5367297b225287f9`

All four paused reads selected node2 only after checking FOLLOWER in both
metadata groups. Each new independent strict-RDMA reader verifies four full
8MiB buffers, every256 deterministic128KiB block, EOF and32MiB digest:
`3cab1a5c8ff63b5d9d7375234c03a5fcae7beabdb5b600210586d07aa1deb586`.
All exit0; unchanged180s outer, read30s, pair5s and cleanup budgets.

| Paused follower run | Location | Sum of four read syscall durations | Result |
| --- | --- | --- | --- |
| 1 | installed17G/node cluster, paused-r1 |2.619s|32MiB PASS |
| 2 | installed17G/node cluster, paused-r2 |2.618s|32MiB PASS |
| 3 | installed17G/node cluster, paused-r3 |2.612s|32MiB PASS |
| 4 | fresh4G/node rdma-qualification-xsmCUU |2.622s|32MiB PASS |

First8MiB is2.55–2.56s; subsequent8MiB buffers are0.019–0.023s. Passing old a7
diagnostic spent about22.6s per8MiB. These are read-phase timings only, excluding
open/setup, caller verification and cleanup. This is functional software RXE
acceptance, not hardware bandwidth or GPU-device-memory DMA evidence. Historical
failed reads stay recorded; four passes qualify this exact scenario and scope.

Three fresh4G/node files/direct fixtures, seven independent clients/jobs16,
stock15s/decorated75s/outer180s unchanged:

| Fixture | Core | Duration | Overall |
| --- | --- | --- | --- |
| rdma-qualification-xsmCUU |1512/1512|34.972s|PASS, including paused read |
| rdma-qualification-GV9oPS |1512/1512|32.923s|PASS |
| rdma-qualification-fEdnsa |1512/1512|43.333s|PASS |

Combined4536/4536 core, deep42/42, crazy-dir21/21, unlink21/21 and link21/21
PASS. Peer201/201; cold prepare81/81 and verify81/81 PASS. EOF fault oracle
six cases/twelve refused fsyncs/six exact retries PASS (full small payload;
sparse anchors and exact EOF, not full-hole hash). No app-visible unlink/link
EBUSY recurrence. Third cohort logs three internal opid admission BUSYs on
client1 while an older rename completes in1.096s; wrapper retries recover and
all cases PASS. Retain this bounded-admission observation; do not claim all
internal contention is eliminated. No counts include TSV header rows.

Strict transport audit:3471 observed RDMA counter rows across standalone reader,
cohort client and main fixture logs have post-setup TCP bytes0/0 and violations0.
TCP setup/bootstrap is permitted; no payload fallback or TCP comparison occurred.
No trace/profiler/build overlapped quiet measurement. All six actual installed
process environments audited RDMA/rxe0/GID1 and commands client/server0,
expiry0, balancing1; all four daemon args files/17G/direct. `.efs`
is absent on installed mount. Only installed mnt/mnt2 and four nonpaused daemons
remain. All owned faults cleared, readers drained, followers restored; synthetic
fixture stores deleted only after clean shutdown. Installed namespace retained.

Targeted AMD ASan/UBSan production ordering-header regression PASS after quiet
fixtures, leak checking enabled, no sanitizer diagnostic:

```sh
cd /data1/efs/src
CC='gcc -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' \
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
python3 tests/test_read_peer_order.py
```

This instruments the new header/adapters, not entire live daemons/FUSE.
Local macOS ASan leak mode was unavailable; repeat with detect_leaks0 passed.
That unsupported option is not recorded as an implementation pass or failure.

Raw native/build/fixture/sanitizer results:
`/data1/efs/rdma-fixes-ade336daf`, the three fixture roots above; normal source
`/data1/efs/rdma-merged-ade336daf`, fault source `/data1/efs/rdma-fault-ade336daf`.
Normal installed `/data1/efs/src/SOURCE_COMMIT` names the full frozen source.
Controller/native/three standalone/three fixture/sanitizer exit markers all0.
No raw artifacts or binaries committed. No push.

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
hash; reconstruction still requires two good fragments.
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
readiness/ownership checks. Full native and fixed AMD live gates PASS as above.

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
namespace/read/peer/cold gates. The affected AMD paused-read and quiet regression gates PASS on ade source.
W106 aggregate stays OPEN for broader/hardware acceptance. Do not turn this
advisory routing fix or software RXE evidence into hardware acceptance.
