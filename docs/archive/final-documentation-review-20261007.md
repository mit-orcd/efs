# Final documentation review — Oct 7, round 7

[Current queue](../status/README.md) · [Current handoff](../status/in-flight.md) · [Review history](README.md)

The final round started **16:10:06 UTC**, with a **16:25:06 UTC** checkpoint.
Baseline: HEAD `b3a11877d5836655633875047c1c6e93e4849a07` plus the retained
working documentation changes from rounds 1–6. During this round the concurrent
GC task committed its final checkpoint in `75f6a42e` and regenerated artifacts
in `7582df59`; those checkpoint reports were reconciled without independently
repeating their remote tests. This closes the planned active
documentation review, not implementation of every open finding or independent
acceptance of every historical deployment. No production code, cluster data or
services were changed by this round; no commits were made.

## Final reconciliation

- W86 metadata/header/tests/publication hooks are now committed in `b3a11877`.
  The queue, handoff and work item distinguish this from production activation.
  W72 sessions, versioned admission/wire, authoritative distributed revocation,
  stable storage member identities, durable fences and all-member deletion ACKs
  remain integration gates. D31 records the checkpoint's narrow policy approval;
  it does not decide D28 or constitute a newly retrieved approving exchange.
- W43's original silent-success title is historical. Explicit failure was
  already implemented; `3a1b4a52` makes capacity rejection atomic and preserves
  fragment references. The stable heading/link is retained, with a current
  checkpoint preceding historical evidence. D25 public logical resize remains
  disabled and integration/acceptance gates stay in the running queue.
- D25's original receipt recommendation now points to the implemented staged
  durable-result/retirement/client-ownership primitives below it. Remaining work
  is production integration and gates, not a second receipt design.
- Completed io-stats/version rollout rows move from the active queue to the
  dated records below. Their reference pages/source and recorded acceptance
  remain available. This does not close unrelated observability findings.
- D17's ecrawl gate and D26's live-table idle-hour/GC_ACK-share gates remain
  explicit in the index. W20's old “remaining action: none” wording is corrected
  to distinguish implemented allocation counts from its outstanding ecrawl gate.
  P3's original pwrite-loop/design instructions are labelled historical; current
  tools exist, while named-host scaling measurements remain owed.
- The latest reconciliation history moves here. The active index links this
  ledger and retains current state, numbered findings and unresolved gates.
- The existing documentation link gate now covers all active Markdown pages
  and the root README, in addition to its original normative source list.
  Normative invariant/table/section checks keep their existing scope. Generated
  Markdown/HTML are rebuilt from source, never patched independently.

## Completed rollout rows retained from the active index

| # | item | class | status | home |
| --- | --- | --- | --- | --- |
| — | **io-stats** · always-on per-op-class data-plane counters (`iostats:` log line + `efs-mgmt io-stats`) | observability | landed (dev cluster, Oct 4) | [note](../backlog/io-stats.md) |
| — | **version reporting** · `--version` on all five binaries, startup log lines, `EFS_MSG_VERSION` op, `efs-mgmt version`, `status` Versions line | observability | landed (dev cluster, Oct 5) | [code](../../include/efs/version.h) |

These are recorded development-cluster rollouts, not live process inventory or
blanket acceptance of a changed release.

## Previous running-page reconciliation, retained intact


The [round-2 evidence ledger](../archive/queue-review-20261007-round2.md)
records code/evidence checks and acceptance limits. W36 and directory-time
row 0m are archived as accepted on their recorded builds; a new rollout must
repeat the release gates. W42 has four-node capacity evidence but retains its
protection/reroute question. W52 approval has not been mistaken for implementation.

The [round-4 operator review](../archive/operator-review-20261007-round4.md)
records W78–W81 and current command/counter limits; dated runbooks and
capability wording remain archived with their evidence.

The [round-5 protocol review](../archive/protocol-review-20261007-round5.md)
records W82–W85, isolated failing reproductions, simulator/host coverage
differences and refreshed primary-source comparisons. These findings await
triage and do not establish a new implementation order.

The [round-6 contract/evidence review](../archive/contract-review-20261007-round6.md)
reconciles the committed GC checkpoint, indexes W86–W89 and D31, and corrects
W23's completed measurement and current backpressure meaning. It creates no
new design approval or production acceptance.


## Original D25 receipt recommendation, retained

The publication continuation adds captured FileID validation at REPORT preflight
and same-group durable apply, exact base/list identity, immutable materialization
and staged typed cache PUT/REPORT lifecycle APIs. Failed PUT retains accepted
bytes; ambiguous REPORT retains its pending token. Activation first needs durable
per-publication outcome recovery, because the current aggregate reply cannot
distinguish partial commit from a CAS loser or an evicted apply verdict. Recommend
per-record results plus durable retry/status identity before wiring typed flush
paths. No new client-scaling or live activation result is claimed.

This was the earlier checkpoint recommendation; the following sections in the
active D25 page now document its implemented staged primitives.

## Coverage and remaining obligations

The seven rounds reconciled the active status index, handoff, decisions,
GC/ownership, memory, scaling and specification gaps; backlog contracts;
architecture/protocol/failure/performance explanations; user/operator guides;
testing, verification, runbooks and entry points. Accepted contracts remain in
their current homes or linked archives; original failure fixtures, measurements
and withdrawn interpretations remain retained. Archived histories were used
where needed, not exhaustively re-reviewed as current implementation claims.

All 33 active Markdown pages plus the root README receive link validation.
The active W index keeps one row per numbered finding; explicit newer W anchors
and the D register are checked for identity consistency. Historical gates remain
attributed to their recorded builds. This review does not independently repeat
remote acceptance, reproduce the 40 GiB incident, establish power-loss safety
or prove every code path. Open W findings and decision asks are implementation
or evidence work, not grounds to keep this documentation cleanup perpetually
in flight. No additional documentation review round is required for the planned
scope; future source changes require their normal documentation updates.

## Final verification

- All five architecture documentation gates pass: regeneration diff, expanded
  internal links, invariant references, operation-matrix sections and single home.
- The link-scope fixture catches missing targets in an active status page and a
  bad root-README anchor, passes corrected links, excludes unrelated archives
  and keeps explicitly normative archive sources checked.
- The final archive ledger and archive index resolve their own links.
- The W index has 44 unique numbered rows. W61–W89 each has exactly one current
  row and explicit work-item anchor. D1–D31 are accounted for; D14 remains
  unassigned, D28–D30 remain ASK, and D31 has one stable anchor.
- All 33 active Markdown pages plus root README are included in link coverage.
- `git diff --check` passes. Production source/header/test/Makefile diffs are
  empty at the final checkpoint; concurrent committed implementation is preserved.

No full runtime suite or new cluster fault run was performed in this docs round.
Isolated failures from earlier rounds remain retained as findings, not rewritten
as passing tests. These checks establish documentation consistency and identity
coverage, not universal correctness of the implementation.
