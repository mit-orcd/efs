# Packing query stage diagnostic — d485afbf

Status: diagnosis, no additional correctness fix; packing closure remains open.
AMD isolated four-daemon TCP/direct fixture, seven clients/jobs16, original30s
case and180s GC budgets. Source `d485afbf`, instrumentation only; normal production
stores/source unchanged. Full normal/fault native and sixteen ASan/UBSan gates
PASS with all1,495 frozen inputs unchanged. Both-mode ordinary overwrite exact
primary/peer/cold/physical gates PASS. Native33, ordinary13 and diagnostic28
retrieved artifacts match their SHA256 manifests.

[Native identity and full gates](../../results/measure/20261012-v030-merged-native/d485afbf/acceptance-summary.json),
[ordinary prerequisites](../../results/measure/20261012-v030-ordinary-overwrite/d485afbf/),
[stage summary and full-raw log hashes](../../results/measure/20261012-v030-query-stage-diagnostic/d485afbf/direct/stage-summary.json).
The full raw daemon logs remain at their absolute remote paths in that summary;
filtered timestamped stage/publish/refresh/disk lines and all seven raw POSIX
records are committed alongside it. No fixture stores were copied.

## Observed stages

| Publisher | Changed transaction cuts | Capture BUSY attempts | Successful checkpoints | Maximum checkpoint attempt ms | Root-authority/page-consume BUSY | Publication writer wait ms |
|---|---:|---:|---:|---:|---:|---:|
| n2 | 71 | 18 | 4 | 10071 | 378 / 378 | 0 |
| n3 | 75 | 18 | 3 | 8950 | 336 / 336 | 0 |

A changed cut means actual captured/current transaction revisions differ and
production code returns BUSY; all attempts keep their new pin/barriers and
existing four-cut bound. One n2 authority BUSY takes402ms. No pending-record
kind/shard refusal was recorded; n3 has one build BUSY250ms not attributed by
this instrumentation. n1/n4 are expected non-query-capable nodes (pin
NOT_PRIMARY), not evidence of a new failure.

n2's successful publications occur at00:47:14.722Z and00:48:01.637Z:46.915s gap.
n3's occur at00:47:14.629Z and00:48:03.445Z:48.816s gap. Both exceed original30s
cases. Catalog checkpoint events include total attempt wall time, not exclusive
fsync cost. Publication-lock wait is zero at millisecond resolution. The earlier
lag-update writer lock is unmeasured; no assertion that every lock wait is zero.

Root-authority is the first `query_auth_dir` plus ancestry refusal in
`query_consume`. It can mean new root missing from old catalog, changed
parent/generation/current binding, current transaction read refusal, permission
or deadline. The enclosing page-consume event repeats that refusal; do not sum
it as an independent failure. We have numeric root and catalog generation in
raw events, not a unique branch reason inside query_auth_dir. Thus observed cut
collision is concrete, but every request's reason is not yet established.

All seven clients209PASS/4FAIL/1unsupported-mmapSKIP/7NOTRUN in44.3–45.3s.
Failed cases: deep nesting timeout; many-files worker killed; stats timeout with
live worker; find blocked after stats. Busy extra-client stop prevents later
packed cold/physical phases; round1, restore0. Foreground directory creation is
not served by the catalog alone. No same-source d485 OFF quiet control or new
foreground stack capture was collected. Prior OFF controls and CPU captures
have different source/workload scope and cannot establish this run's causality.

## Safe next decision and ownership

Mac reviews coherent-cut ordering and individual root-authority branch diagnosis;
ORCD reviews relationship to foreground directory admission/proposal waits.
AMD remains sole implementation/editor and test executor. No fence/pending/import/
authority barrier may be removed on this evidence. In particular simply moving
pin after barriers requires proof about asynchronously applied participants and
cannot be treated as a minimal safe repair yet.

Before a repair, record exact invariant, actual-function regression and failing
old-source/negative control. Before another expensive run, state the changed
hypothesis or missing discriminator. Any repair gets a new immutable full-gated
source and separate quiet same-source ON/OFF pressure acceptance. Synchronous
tracing perturbs latency; this instrumented result is not quiet acceptance.
No new deployment, push, tag or main promotion.
