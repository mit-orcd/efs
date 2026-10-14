# AMD RDMA namespace admission — 2026-10-14

W106. Branch `devel-rdma`. Files/direct/strict segmented RDMA, Soft-RoCE
rxe0/GID1 on AMD-395; four same-host nodes. `.efs` client/server, packing,
containers, expiry and repair experiment OFF; metadata balancing1.
Status: fix pending AMD qualification; no push or TCP comparison.

## Reproduced merged baseline

Source `ac22b8d704be56011c58afe8180a4b9d733060bf`, native build
2026-10-14T04:01:51Z. Full native `make test` PASS after supplying omitted exact
tracked documentation link targets; the first incomplete-export run FAIL is
retained. Normal installed binary SHA256:

- efsd `25e101fe7d0b6a8d5a0620ee47ed647cd0e097feb082b637fb46fdc5a5c22881`
- efs-fuse `7c16f1ea0e3fcb676458f6108a0ca1aa83335f87fe3ef031894a8220e6e19a77`
- efs-mgmt `adb301d279fd637faa332f5b7ba3711f7ee38930a11f190ad6aceb545d7125d5`
- private fault-client `dd49c74cd017ada341281f1a8f368b7bf4198de1abd88cf04a74f6bba16a8b66`

Three fresh private 4G/node fixtures, seven independent clients/jobs16 each,
216 core cases/client, unchanged stock15s/decorated75s/outer180s:

| Fixture | Core | Elapsed | Other findings |
| --- | --- | --- | --- |
| rdma-qualification-lcw1ka |1512/1512 PASS|32.962s|Paused follower2 read FAIL: verified24MiB; timeout180/k5, exit137 |
| rdma-qualification-WGqm1g |1512/1512 PASS|38.204s|No degraded repeat |
| rdma-qualification-jfYuek |1509/1512 FAIL|44.630s|client3 unlink EBUSY, root_rename_from_subdir EBUSY, dir_deep_nesting15s timeout |

All three peer67/67, cold prepare27/27 and verify27/27, W100 two-case
refusal/recovery oracles PASS. Initial fixture overall exit1 due degraded read;
third overall exit1 due core failures. Controller completion exit0 means its
sequence finished, not that all gates passed. Raw evidence outside Git:
`/data1/efs/rdma-fixes-ac22b8d70` and the three fixture roots above.

The old fc unlink/link errors coincide with client `opid-admission` refusal:
oldest350,next366,live1/window16; an older rename later succeeded after5.438s.
The merged failure similarly shows oldest294,next310,live1; root rename consumes
its existing8s scope while admission blocks younger operations. These logs
narrow the immediate unlink refusal to client admission, but do not by
 themselves prove every upstream delay or that an unobserved server operation
failed atomically. Keep receipt/namespace consistency gates intact.

A separate marked `ac22b8d70-diag` tree logged RPC identities/statuses and
rename phases without changing deadlines. Its full mixed core PASS1512/1512
in33.766s; a slow rename first targeted single-group node4 (473.769ms) and
namespace forwarding paths remain active. Diagnostic code is not installed or
accepted as frozen-source evidence. Artifact patch is at
`/data1/efs/rdma-diag-artifacts-ac22/instrument.py`; outputs at
`/data1/efs/rdma-qualification-79BDzq`. No quiet tracing/build overlap.

## Fix and focused checks

Unlink, link and rename now reuse the established early namespace leader
hint used by mkdir/rmdir. Redirect only to a known nonlocal parent-group leader
whose committed membership votes in both metadata groups, before fresh reads,
session admission, transaction preparation or receipt probes. The client keeps
its operation identity/payload. Unknown, self, removed and single-group leaders
retain authoritative fallback. Every served mutation still obtains its fresh
proofs and durable transaction verdict.

This addresses unnecessary follower coordination/forwarding that can retain
the oldest identity and amplify admission stalls. It does not increase the
16-receipt window, one-second no-caller admission fallback, rename8s,
RPC retry counts, read deadlines or suite budgets. No claim that this alone
closes the intermittent failures until new quiet cohorts pass.

Local macOS focused checks PASS:

```sh
python3 tests/test_namespace_leader_route.py
python3 tests/test_namespace_receipt_pending.py
python3 tests/test_metadata_retry_budget.py
```

The route regression includes all three file mutations and existing membership,
unknown/removed/self/single-group cases. Receipt test retains pending-receipt
refusal/materialization race and errors; retry test retains identity/deadline
semantics. Old-source handoff regression fails its actual handoff assertion;
merged source passes. No live sanitizer claim for these local checks.

## Reproduction and remaining gates

Export a frozen commit as in [merged qualification](rdma-merged-qualification-20261013.md).
Build native and separately fault-enabled client with that same source identity.
Deploy normal binaries after draining installed AMD clients/daemons. Retain
actual process flag audit and binary hashes. Run after all builds finish:

```sh
cd /data1/efs/src
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
# Set FAULT_SOURCE to a separate EFS_FAULTS=1 tree of this exact source.
for r in 1 2 3; do
 degraded=0; [ "$r" != 1 ] || degraded=1
 EFS_GATE_DEGRADED=$degraded EFS_GATE_EOF_SOURCE="$FAULT_SOURCE" \
   bash tests/live/rdma_four_node_qualification.sh > "$OUTPUT/q$r.log" 2>&1
 echo $? > "$OUTPUT/q$r.exit"
done
```

Private driver owns four27432..27435 files/direct stores at4G/node, distinct
home mounts and fixtures, restores paused followers, clears fault files and
stops/drains owned processes before deleting only synthetic stores. Quiet runs
have no trace/profiler/build. Full native, fresh quiet load, peer/cold/EOF and
degraded rerun on fixed source remain NOTRUN. Preserve original failed markers;
new passing evidence must name its exact source. W106 aggregate stays OPEN.
