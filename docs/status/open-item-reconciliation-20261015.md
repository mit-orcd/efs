# Open-item reconciliation — October 15, 2026

Review baseline: `devel` **65954cdca**, whose latest product commit is
`4f5fca5dd`. This is a documentation/source review, not a deployment or new
acceptance run. Runtime tests: **NOTRUN**. No cluster was accessed, feature
activated, acceptance limit changed or historical failure discarded.

The queue and specification review contain old implementation statements next
to newer acceptance evidence. Superseded wording is not an outstanding coding
task. Conversely, code present or a passing different platform does not close
an item's remaining gate. This review makes no new global closure decisions.

## Superseded claims and narrower remaining work

| Item | Disposition of older claim | Evidence and remaining action |
| --- | --- | --- |
| W71 | **Implementation claim obsolete:** production PUTs do have durability barriers. | `src/server/store.c:finish_fragment_put` explicitly finishes the file with a barrier and syncs parent paths; October 14 directory barrier batching preserves the boundary. [Original implementation](replay-readiness-20261008.md), [batching qualification](intel-sync-batching-20261014.md). Hardware power-loss qualification is separate and deferred in the normal-operation scope; do not implement a second barrier to fix the old description. |
| W72 / D31 / W86 | **Partially superseded:** mount sessions are not wholly absent, and tickets are more than the original metadata-only prototype. | [Dependency integration](dependency-package-20261009.md) and [NUC integration](devel-nuc-integration-20261011.md) record mount identity, namespace admission and REPORT/session gates. Complete production ticketed PUT/flush, storage rejection floors and authority-safe cleanup remain unclosed. Avoid repeating the original session-bootstrap task. |
| W73 | **Already accepted for supported software scope.** “Neither FUSE callback publishes O_SYNC/O_DSYNC” is obsolete. | Current `efs_fuse_sync_write_result` is called by both write paths. [AMD direct/buffered TCP/RXE acceptance](amd-rxe-gate-closure-20261009.md). Physical hardware durability remains W71; lack of RXE on NUC is not an outstanding W73 prerequisite. |
| W84 / D32 | **Already closed for the accepted 144-participant envelope.** Full-ancestry expansion is not an unpaid gate of that closure. | [Closure](w84-closure-20261009.md): exact144 PREPARE/COMMIT, bounded refusal and cold atomicity. Larger-operation protocol is deferred. |
| D28 | **Decision settled, not ASK.** | [Accepted contract](decisions.md#d28d30--accepted-oct-8-2026): unpublished data has no survival guarantee after client death; automatic salvage deferred. D27 retention and visible errors while alive still apply. |
| W74 / standalone repair | **Original “two repair GAPs / TCP only” task superseded. Broader recovery deferred, not closed.** | [Ordinary-fragment repair qualification](orcd-default-repair-20261012.md), source `52581e6ca`, is in baseline ancestry. Private TCP and native-RDMA harness results without `--repair-experiment` do not establish automatic live repair, packed repair or D36 profile protection. The old inactive-only checkpoint is historical, not a description of every current repair path. |
| W75 | **Missing-evidence acceptance claim superseded; identity-bound integrity remains open.** | [Work item](../backlog/work-items.md#w75) records fail-closed checksum handling and captured metadata-digest validation. Its original title must not be read as current behavior. Full identity-bound format/repair requirements are not closed by payload checksums. |
| W100 | **Closed by explicit human acceptance October 14.** | [Final Intel report](intel-report-newer-snapshot-20261014.md), `28df9c7eb`: preserve earlier timeout/EIO evidence, but do not keep executing the older “remaining action” as W100. New reproduced stalls need fresh identity/evidence. This does not establish original ORCD root cause. |
| W101 | **Creation phase complete; verification remains.** | [Work item](../backlog/work-items.md#w101): original two million-file jobs finished. Preserve their trees and prove required inventory/payload coverage; do not rerun creation merely because the item says in progress. |
| W102 | **“Wait for W100 repair” prerequisite obsolete; reconcile acceptance coverage.** | [Final Intel report](intel-report-newer-snapshot-20261014.md) includes both hosts' full recovery/GC fixtures and cross-host peer PASS. Match those exact fixtures/source/profile to W102's private harness and any still-required buffered scope before closing; later direct-only success cannot silently satisfy all older modes. |
| W103 | **Historical blanket query-failure description narrowed; not globally closed.** | [Work item](../backlog/work-items.md#w103) already records later passing six-query matrices. `.efs`-disabled Intel tests cannot close enabled command freshness/refresh gates. Keep persistence and `.efs` acceptance separate. |
| W108 | **Already closed.** Numeric-only `ls/find` is the historical trigger. | [Rebuilt seven-client ORCD acceptance](devel-efs-query-20261012.md) covers owner names, fallback, numeric mode and JSON. Do not reimplement NSS resolution. |
| W109 / W92 | **Several original mechanisms repaired; aggregate scale/load gate remains.** | [Coherent capture fix](devel-efs-query-20261012.md), [sort peak/cleanup](query-sort-peak-correctness-20261013.md), [AMD functional evidence](amd-package2-query-20261013.md), [later ORCD query work](orcd-query-name-ordinals-20261013.md). Old 1,024-change-cliff/row-size descriptions are not a complete description of current code. Full-size pressure, complete payload verification and held acceptance are not proved by those fixes. |
| W110 / W106 | **Oversized application-frame TCP fallback explanation is historical.** | Current `src/common/protocol.c:conn_pick_send_chan` selects RDMA for every upgraded connection regardless of frame size; `39d93093a` integrated segmented transport. [AMD qualification](devel-rdma-merge-qualification-20261014.md) is source-specific. TCP setup descriptors can remain; hardware traces and original loss/stall attribution still need their own proof. |
| W112 | **Already closed for canonical full-overwrite replay.** | [Closure](w112-full-overwrite-replay-20261012.md), `02b9918b`. The old imperative “close only after” is a historical gate, not a reopening. Partial ownership and unrelated publication failures are separate. |
| W118 | **Still open on the failing ORCD platform.** | [Intel fork/read controls](intel-direct-read-fork-20261014.md) do not reproduce it. Oracle repair and October 15 read-cache/FUSE changes are not proof of fixing the kernel/application-buffer race. |
| October 15 read optimization recommendations | **Implementation advice partly superseded; qualification remains source-specific.** | After [the measurement](intel-read-coordination-20261015.md), `8d06e1723`, `cedc9c46e`, `290ddaee3`, `629f14bf5`, `1b37a915f`, `4f5fca5dd` implement read-image reuse/coherent keys and FUSE negotiation/cache-miss changes. Do not recommend implementing these from scratch; qualify the resulting source. No new speedup claim here. |

## Still applicable, deferred, or not established by this review

| Items | Disposition |
| --- | --- |
| D25 / W43; D27; D29 / W88; D30 / W23 | **Keep open.** Staged primitives, admission and individual replay fixes do not prove public logical-truncate activation, bounded whole-call recovery, durable receipt retirement or universal follower memory/lag bounds. [Dependency ledger](dependency-package-20261009.md) preserves integration limits. W89's measurement-schema bug is already closed; it must not be counted as D30 completion. |
| D26 / W44; W61–W63 | **Separate implemented mechanisms from residual gates.** Watermark/cursor and GC fixes exist; old incident drain/reconciliation or retained-table latency findings need their named evidence, not automatic deletion because newer GC suites pass. |
| D35 / W81 | **Keep open.** The inspected `efs_meta_dir_note_entry` still starts splitting from the entry-count threshold. No demonstrated sustained-contention policy closure was found; explicit spread and faster metadata lookup are not that policy. |
| W83 | **Keep open.** Inspected decision-key callers in `src/meta/txn.c` still write/read durable decisions; no demonstrated bounded participant-ACK retirement proof was found. This is not a new runtime leak measurement. |
| D36 / W42 / W66 / W74 / W98 | **Accepted architecture still applies; implementation/scale gates remain.** Later routing, balancing and RDMA work supersede “no distribution work exists”, not the four-node two-loss protection contract or measured scaling obligations. Fixed two-group observations are dated topology, not a reason to drop W98. |
| W48 / W52 / W53 / W55 / W58 / W69 / W87 / W97 / W99 / W105 / W111 | **No new closure established.** Some have scoped fixes/acceptance, but original trigger, platform, sustained-load, capacity-domain or explicitly named residual gates cannot be inferred complete from unrelated passing tests. Check the item ledger before scheduling; do not reproduce old builds as a current defect claim. |
| W54 / W38 / W64 / W65 / W106 | **Preserve hardware/profile limits.** Existing software acceptance narrows remaining work; it does not prove every physical-host/failover/RDMA profile. W65's old TCP-only queue wording is superseded by the AMD RXE acceptance; no blanket hardware closure. |
| D33 / W90 | **Initial expiry scope already accepted; excluded from current three-package profiles.** Follow-up maximum latency outliers are not evidence that basic expiry still needs implementation. [Expiry evidence](expiry-foreground-latency-20261013.md). |
| D34 / W91 / W92 | **Commands implemented; scale/compatibility acceptance remains distinct.** General ORCD 400 GiB work is recorded on hold. “On hold” is scheduling, not obsolete functionality or PASS. |
| W95 / P4.1 | **Packing implementation is no longer absent.** [Packing ledger](packing-tcp-direct-20261012.md) retains matched comparison, lifecycle evidence and format/rollout gates. Disabled in the three-package profiles; those tests cannot qualify it. Container qualification is separately excluded by [policy](container-testing-policy-20261012.md). |
| W104; W113–W117; P3/P4 | **Unsupported/deferred/proposed or independently gated work remains so.** mmap support, aggregate telemetry, storage availability/protection views and named storage/scaling measurements are not made obsolete by fixes to ordinary I/O. W114 is an implemented benchmark with its own evidence, not proof of product-wide storage acceptance. |
| D1–D24; D32–D35; W56/W59/W60/W67/W68/W70/W76/W78–W80/W82/W85/W89/W93/W94/W96 | **Do not count accepted/closed records as open implementation work.** Retain contracts, closure scopes and regression tests. D6/D12 have explicit supersession in the decision register; D14 remains unassigned. Historical decisions and closed rows need not be deleted. |

## Review method and limits

Reviewed the ordered queue, decision register, public-path review and work-item
ledger through W118; followed the linked newer acceptance records for the
annotations above. Inspected production durability, FUSE synchronous callbacks,
transport channel selection, directory spreading and transaction decision-key
callers. Verified repair and bounded paging commits are ancestors of baseline.
This is targeted reconciliation, not a full source audit of every contract.

Reproduce the source checks from baseline (no running cluster required):

```sh
git show 65954cdca:docs/status/work-queue.md
git show 65954cdca:docs/status/decisions.md
git show 65954cdca:docs/backlog/work-items.md
git log --oneline 65954cdca -- src/common/protocol.c src/client/efs_fuse.c src/server/store.c
git grep -n -e finish_fragment_put -e sync_fragment_parents 65954cdca -- src/server/store.c
git grep -n efs_fuse_sync_write_result 65954cdca -- src/client/efs_fuse.c
git grep -n conn_pick_send_chan 65954cdca -- src/common/protocol.c
git grep -n -e efs_dir_spread_min -e SPLITTING 65954cdca -- src/meta/dir_layout.c
git grep -n -e efs_kv_key_decision -e EFS_KV_KIND_DECISION 65954cdca -- src
git merge-base --is-ancestor 52581e6ca 65954cdca
git merge-base --is-ancestor e77f60305 65954cdca
```

Both ancestry checks return zero. No binaries were built or tested; exact
historical build identities, feature profiles, commands and verdicts remain in
the linked qualification reports. No cleanup or runtime restoration is needed.
Do not derive a new total open count by counting dated rows or treating deferred
work as closed. Each future closure needs a canonical item and explicit scope.
