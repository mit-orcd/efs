# Specification and public implementation — Oct 7 review

[Queue](README.md) · [Decisions](decisions.md) ·
[Architecture](../how-it-works/architecture.md) · [Capability inventory](../backlog/product-gaps.md)

**Release reconciliation:** v0.2.0-pre-alpha adds RoCE and four-mode AMD
functional acceptance. W75 checksum coverage, closed W76/W82/W85 gates and
W84's enlarged envelope below reflect the subsequent status ledgers. Other
rows retain their explicitly dated review scope; this is not a new full-code
or hardware power-loss audit. See [release evidence](v020-amd-release.md) and
[the current queue](README.md).

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
| Durable fragment ACK / **W71** | Daemon fragment writes lack a persistence barrier; FUSE fsync drains PUT/REPORT without a separate target flush | Persist payload, checksum, length and necessary namespace before durable publication; injected storage failures and crash/power-loss acceptance |
| I23 sessions / **W72** | Session metadata, management coordinator and staged publication admission exist; production HOLD/FLOCK omit session suffixes, mount opid epoch is locally seeded at 1 | Real mount session establishment, touched-authority registration and completed revocation before dependent cleanup |
| Synchronous application writes / **W73** | Both FUSE write paths inspect append/direct flags but do not explicitly publish on O_SYNC/O_DSYNC; benchmark --sync is a separate engine option | Verify actual kernel/FUSE sequencing; wire or prove every supported synchronous path, with failures visible before success |
| Failure profiles / repair / **W74**, **W42** | Data uses fixed 2+1; staged validation accepts only K2F1; heal status returns zeros; no live-fragment repair owner or profile migration found | Track protection debt, restore lost fragments, validate supported profiles and cutover; source inspection is not a permanent-loss acceptance run |
| I25 fragment integrity / **W75** | Missing/corrupt checksum evidence fails closed; captured metadata digests and optional payload verification cover parallel/fallback GET paths; PUT hashes remain payload-based | Identity-bound format and repair; NUC checksum/metadata trust gates pass |
| Logical truncate / **W43, D25** | Metadata histories, read views, internal resize and publication primitives exist; public truncate still uses legacy path | Epoch-owned FUSE integration, live-file materialization/history retirement and activation gates in the handoff |
| Writeback failure / **D27** | Retention/error/controlled-stop foundations and WITHHOLD hook exist; server reject hook absent | Strict whole-call, contention, failure/recovery and RSS gates; D28 salvage remains undecided |
| I16 retry identity / **W76** | Closed: bounded 512-owner admission refuses exhaustion safely; lost-reply mutations retain identity across leader death/restart | Recorded acceptance in the [follow-up ledger](next-five-gates-20261007.md); no unprotected retry accepted as the remedy |
| I24 atomic publication/observation / **W77** | Legacy REPORT splits per group; staged publication is per chunk; read-ref path lacks final range revalidation | Original FUSE-request identity/transaction plus validated multi-chunk read gate; no above-max_write guarantee |
| Authoritative read freshness / **W82** | Closed: stale metadata/session/publication/transaction read views and configuration partition gates pass; coordinator callback refuses untrusted absence | [Recorded gate scope](next-five-gates-20261007.md); healthy POSIX alone is not partition acceptance |
| Transaction lifecycle / **W83** | Participant records resolve; decisions have no inspected durable-ACK retirement path | Safe bounded decision retention under restart, lost replies and lagging participants |
| Namespace predicate bounds / **W84** | 144-participant namespace envelope; full 64-lane removal/replacement, depth 143/refusal 144 and 66-participant prepared/committed crash gates pass | Approved full-ancestry/product limit and exact maximum-envelope crash acceptance remain; [TCP ledger](tcp-five-gates-20261007.md) |
| D7 first-PUT hint / **W85** | Closed: ambiguous placement retained; direct/buffered lost-ACK collision and two-root physical/quota/restart gates pass | [Recorded acceptance](next-five-gates-20261007.md); no claim of whole-program power-loss durability |
| Abandoned PUT ownership / **W86, D31** | Ticket metadata committed `b3a11877`, staged for integration; actual mount/PUT/wire/revocation/storage coordination not active | Fault-gated production integration, all-member deletion and replay floors; D28 salvage is separate |
| Buffered append under load / **W87** | GC checkpoint records missing records in both broad concurrent-append cases; later isolated repeats pass | Trace retained full buffered failure, isolate cause and verify exact records under the original load |
| D1 folded-span retry / **W88** | Legacy tombstone eviction allows an old byte-identical span to append over a later full image; isolated metadata reproduction | Actual host/FUSE delayed retry, durable identity/floor integration and exact-byte gates |
| Metadata topology / **W66** | Two fixed groups | Distributed leadership/routing and measured object/throughput scaling |

[Round-3 evidence](../archive/spec-review-20261007-round3.md) records the
review scope. Fault simulator/integration infrastructure exists; files and
healthy POSIX passes do not establish all the failure guarantees above.
