# AMD RDMA namespace admission — 2026-10-14

Historical a7 source checkpoint. The later [paused-follower read fix and
affected qualification](rdma-paused-follower-read-20261014.md) on ade source
passes four full32MiB reads and repeats all quiet core/peer/cold/EOF gates.
The a7 degraded failure below remains historical failed evidence.

W106. Branch `devel-rdma`. Files/direct/strict segmented RDMA, Soft-RoCE
rxe0/GID1 on AMD-395; four same-host nodes. `.efs` client/server, expiry and repair experiment OFF; metadata balancing1.
Status: namespace load PASS on fixed source; degraded read FAIL; W106 aggregate
OPEN. No push or TCP comparison.

## Fixed-source results

Code commit **`a7e2b6b7d0b57336850c6850f9f669139c6765a3`** is deployed on
AMD. Normal native build timestamp 2026-10-14T04:21:00Z (Oct 14 00:21 EDT),
branch `devel-rdma`. Full clean native `make test` PASS, exit0. All six installed
process environments were audited: RDMA/rxe0/GID1, commands client/server0,
expiry0, balancing1; all four daemons use files/17G/direct. `.efs`
is absent on the installed mount. The durable policy's default mask3/version0
is retained, but server startup ceiling0 disables features/raw queries and
query tracking/workers; do not confuse stored policy with effective enablement.
No diagnostic flags or fault omission hooks are set on installed clients.

Installed binary SHA256:

- efsd `30f970ebb2322e92dd71eed9adc3ac9ab7d5cfdb366a64886c8980af0ef2dd75`
- efs-fuse `1813e7e8f7fefd37ea7277710be94de8a12ca318f605d42647004ec2eabe8e3d`
- efs-mgmt `3bb8fcbacd6fbf5077b8c016dad94fb78e9acc8e106c9723fa77c212fb6579b5`
- private fault-client `06273299c7afc0fa6d133410ec0ced9935bc10f4a5e929bf352d88cd93643298`

| Fresh fixture | Seven-client core | Elapsed | Overall fixture |
| --- | --- | --- | --- |
| rdma-qualification-y3NC0e |1512/1512 PASS|33.746s|FAIL due separate degraded read |
| rdma-qualification-Sk6lvX |1512/1512 PASS|34.259s|PASS |
| rdma-qualification-oOszUY |1512/1512 PASS|42.400s|PASS |

Combined core **4536/4536 PASS**. Exact named checks:
`dir_times_unlink`21/21, `dir_times_link`21/21,
`root_rename_from_subdir`21/21, deep42/42 and `names_crazy_dirs`21/21 PASS.
No operation-ID admission refusal logged across the21 quiet load clients.
Other core checks still exercise hard links, unlink with open descriptors,
rename contention, file data and errno semantics. Seven per-client TSV headers
are not cases; count only PASS/FAIL rows.

Peer201/201; cold prepare81/81 and verify81/81 PASS. W100 fault oracle:
six cases, twelve refused fsyncs and six exact EOF/payload retries PASS across
three independent-client fixtures. Small payload checked throughout; sparse
low/high anchors and EOF checked, not a full-hole hash. These do not close
broader lost-original-EOF/staged-row or hardware obligations.

**Separate degraded-read gate FAIL.** Paused shared follower2; first8MiB verified
in22.597s, next8MiB request did not return the asserted length (reader exit1).
Actual second-return byte count was not logged, so distinguish neither positive
short read from premature EOF nor a storage/authority error from these logs
alone. A positive short POSIX read needs accumulation, but do not relabel this
run PASS: the full32MiB payload/digest was not verified. Existing30s read/5s
pair/180s outer limits remain unchanged; follower restored and mount drained.
Next diagnose this gate with per-read requested/returned bytes and offsets,
retaining the total32MiB byte oracle and outer deadline. The pre-fix24MiB/180s
failure is also retained. Namespace passing evidence does not close W106.

All quiet fixtures ran after native/fault builds finished, without profiling or
tracing. All synthetic stores removed after clean shutdown; only installed
mnt/mnt2 and four live daemons remain. Raw outputs outside Git:
`/data1/efs/evidence/rdma-fixes-a7e2b6b7d`, fixture roots above, normal source tree
`/data1/efs/evidence/rdma-merged-a7e2b6b7d`, fault source tree
`/data1/efs/evidence/rdma-fault-a7e2b6b7d`. Controller exit0 records finished sequencing;
individual fixture exit codes are1,0,0. No raw captures/binaries/stores committed.

The fix is qualified for this unchanged AMD mixed namespace workload, with no
unlink/link refusal reproduced after the change. Repeated passes do not prove
all future load cannot refuse admission, or that this is RDMA-specific. The
one-second fallback and bounded durable receipt window are deliberately intact.

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
`/data1/efs/evidence/rdma-fixes-ac22b8d70` and the three fixture roots above.

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
`/data1/efs/evidence/rdma-diag-artifacts-ac22/instrument.py`; outputs at
`/data1/efs/evidence/rdma-qualification-79BDzq`. No quiet tracing/build overlap.

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
RPC retry counts, read deadlines or suite budgets. The fixed-source quiet
cohorts above qualify this workload; broader acceptance remains separate.

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
merged source passes. Running the updated route placement regression against
ac22 source rejects its missing early route; the membership predicate itself
passes on both. No live sanitizer claim for these local checks.

## Reproduction and remaining gates

Export a frozen commit as in [merged qualification](rdma-merged-qualification-20261013.md).
Build native and separately fault-enabled client with that same source identity.
Deploy normal binaries after draining installed AMD clients/daemons. Retain
actual process flag audit and binary hashes. Run after all builds finish:

```sh
cd /data1/efs/src
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
FAULT_SOURCE=/data1/efs/evidence/rdma-fault-a7e2b6b7d
OUTPUT=$(mktemp -d /data1/efs/rdma-rerun-XXXXXX)
# Both source trees must name the full frozen a7 commit in SOURCE_COMMIT.
for r in 1 2 3; do
 degraded=0; [ "$r" != 1 ] || degraded=1
 if EFS_GATE_DEGRADED=$degraded EFS_GATE_EOF_SOURCE="$FAULT_SOURCE" \
   bash tests/live/rdma_four_node_qualification.sh > "$OUTPUT/q$r.log" 2>&1; then
   code=0
 else
   code=$?
 fi
 echo "$code" > "$OUTPUT/q$r.exit"
done
```

Private driver owns four27432..27435 files/direct stores at4G/node, distinct
home mounts and fixtures, restores paused followers, clears fault files and
stops/drains owned processes before deleting only synthetic stores. Quiet runs
have no trace/profiler/build. Full native, fresh quiet load and peer/cold/EOF
PASS on a7 source; degraded read FAIL as above. Preserve original failed markers;
new passing evidence must name its exact source. W106 aggregate stays OPEN.

**Raw evidence note (2026-10-23):** the bulk data in `rdma-diag-artifacts-ac22`, `rdma-fault-a7e2b6b7d`, `rdma-fixes-a7e2b6b7d`, `rdma-fixes-ac22b8d70`, `rdma-merged-a7e2b6b7d` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
