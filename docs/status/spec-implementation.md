# Specification and public implementation — Oct 7 review

**October 15 reconciliation:** [Review of stale claims and remaining gates](open-item-reconciliation-20261015.md). Read these annotations before treating dated implementation descriptions as current tasks. No new global acceptance or deployment is implied.

[Queue](README.md) · [Decisions](decisions.md) ·
[Architecture](../how-it-works/architecture.md) · [Capability inventory](../backlog/product-gaps.md)

**Release reconciliation:** v0.2.0-pre-alpha adds RoCE and four-mode AMD
functional acceptance. W75 checksum coverage, closed W76/W82/W85 gates and
W84's enlarged envelope below reflect the subsequent status ledgers. Other
rows retain their explicitly dated review scope; this is not a new full-code
or hardware power-loss audit. See [release evidence](v020-amd-release.md) and
[the current queue](README.md).

**D36 node-count correction (Oct 9):** three nodes minimum, four nodes maximum
two-loss safety, larger clusters for scale. Fixed 2+1 and current metadata
quorums do not meet that contract. [Decision and gates](node-count-protection-20261009.md)
govern W74/W66; the former f=3 ladder is superseded.

The specification states the accepted design. This table records inspected
public-path limitations initially at HEAD `ddc677ae`, reconciled through the
final round against `b3a11877` and its subsequent GC documentation checkpoint;
it does not weaken that design or establish a deployment's acceptance.
Round 3 ran no runtime tests. Round 5 reproduced W82 in an isolated in-memory
Raft core and W85 in an extracted hint helper. Round 6 reproduced W88 in
isolated metadata apply and found W89 in retained raw measurement evidence;
no live cluster/FUSE fault test was run. Detailed evidence and gates belong
to each W item.

| Contract / identity | Inspected implementation | Remaining acceptance |
| --- | --- | --- |
| Durable fragment ACK / **W71** | Production file and parent-directory barriers precede PUT success; later directory-sync grouping preserves the boundary | Original missing-barrier implementation task superseded. Physical power-loss acceptance remains separate/deferred; [implementation](replay-readiness-20261008.md) and [batching](intel-sync-batching-20261014.md) |
| I23 sessions / **W72** | Mount identity, retained CREATE/OPEN/CLOSE, namespace admission and REPORT/session gates are implemented with scoped evidence | Full production ticketed flush/PUT, storage fences and revocation integration remain; [dependency ledger](dependency-package-20261009.md) |
| Synchronous application writes / **W73** | Both FUSE write paths explicitly publish positive O_SYNC/O_DSYNC writes before replying | Supported software TCP/RXE direct/buffered acceptance recorded; hardware durability separate under W71. [Closure](amd-rxe-gate-closure-20261009.md) |
| Failure profiles / repair / **W74**, **W42** | Ordinary missing-fragment standalone repair is implemented and scoped TCP/RDMA-qualified; broader profile/debt/cutover integration remains incomplete | Original two-GAP/TCP-only description superseded. Automatic/unplanned and packed repair deferred; D36 not closed. [Scope and evidence](orcd-default-repair-20261012.md) |
| I25 fragment integrity / **W75** | Missing/corrupt checksum evidence fails closed; captured metadata digests and optional payload verification cover parallel/fallback GET paths; PUT hashes remain payload-based | Identity-bound format and repair; NUC checksum/metadata trust gates pass |
| Logical truncate / **W43, D25** | Metadata histories, read views, internal resize and publication primitives exist; public truncate still uses legacy path | Epoch-owned FUSE integration, live-file materialization/history retirement and activation gates in the handoff |
| Writeback failure / **D27** | Retention/error/controlled-stop foundations and WITHHOLD hook exist; server reject hook absent | Strict whole-call, contention, failure/recovery and RSS gates; D28 automatic salvage is explicitly deferred |
| I16 retry identity / **W76** | Closed: bounded 512-owner admission refuses exhaustion safely; lost-reply mutations retain identity across leader death/restart | Recorded acceptance in the [follow-up ledger](next-five-gates-20261007.md); no unprotected retry accepted as the remedy |
| I24 atomic publication/observation / **W77** | Legacy REPORT splits per group; staged publication is per chunk; read-ref path lacks final range revalidation | Original FUSE-request identity/transaction plus validated multi-chunk read gate; no above-max_write guarantee |
| Authoritative read freshness / **W82** | Closed: stale metadata/session/publication/transaction read views and configuration partition gates pass; coordinator callback refuses untrusted absence | [Recorded gate scope](next-five-gates-20261007.md); healthy POSIX alone is not partition acceptance |
| Transaction lifecycle / **W83** | Participant records resolve; decisions have no inspected durable-ACK retirement path | Safe bounded decision retention under restart, lost replies and lagging participants |
| Namespace predicate bounds / **W84** | Accepted 144-participant namespace envelope with prompt atomic EOVERFLOW refusal | Closed for D32, including exact maximum-envelope cold recovery; larger protocol deferred. [Closure](w84-closure-20261009.md) |
| D7 first-PUT hint / **W85** | Closed: ambiguous placement retained; direct/buffered lost-ACK collision and two-root physical/quota/restart gates pass | [Recorded acceptance](next-five-gates-20261007.md); no claim of whole-program power-loss durability |
| Abandoned PUT ownership / **W86, D31** | Ticket metadata committed `b3a11877`, staged for integration; actual mount/PUT/wire/revocation/storage coordination not active | Fault-gated production integration, all-member deletion and replay floors; D28 salvage is separate |
| Buffered append under load / **W87** | GC checkpoint records missing records in both broad concurrent-append cases; later isolated repeats pass | Trace retained full buffered failure, isolate cause and verify exact records under the original load |
| D1 folded-span retry / **W88** | Legacy tombstone eviction allows an old byte-identical span to append over a later full image; isolated metadata reproduction | Actual host/FUSE delayed retry, durable identity/floor integration and exact-byte gates |
| Metadata topology / **W66** | Two fixed groups | Distributed leadership/routing and measured object/throughput scaling |

[Round-3 evidence](../archive/spec-review-20261007-round3.md) records the
review scope. Fault simulator/integration infrastructure exists; files and
healthy POSIX passes do not establish all the failure guarantees above.
