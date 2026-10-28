# Task Y — known-defect register reconciliation, October 28, 2026

`agent/devel-task-y`, reviewed devel **`9e4d66a8cc215f6eab0d2e576721f8722a98e0c1`**
(wave 28): **reconciliation and focused checker tests PASS**. Both tables
contain one row for every K1–K52. **11 FIXED / 0 VERIFIED / 35 VERIFY /
2 DECIDED / 2 CLOSED / 2 OPEN**. Stage 1 remains OPEN. No gate was rerun or
promoted, no product code changed, and no release candidate was frozen.
K48 and K52 remain alpha blockers. This branch is not pushed or merged.

## Method and meaning

Task Y from the October 26–28 parallel-task assignment is documentation and
one small structural checker on the Mac; it needs no reserved host. Initial
base `3debca7ec` gained wave 28 while work was underway. That committed
source/evidence was merged into the isolated branch at `2b7067c57` before
reconciliation; shared devel's uncommitted D39 edit was never read into this
branch or staged. The checker/test/Makefile change starts at `b47d03a2b`.
This report identifies the reviewed devel snapshot separately from its own
subsequent documentation commits.

Read every K disposition and recheck cell and their linked evidence,
then checked cited commits against the reviewed devel ancestry. The source
snapshot and the decision register are the authority; graph navigation is
not a verdict. Graph vocabulary `alpha checklist decision evidence` found
the K/A/S/F checklist sections but was broad/truncated. Evidence pages were
read directly. No graph was rebuilt or used as runtime proof.

The plain leading word is an **acceptance disposition**, not a new test
result. FIXED credits merged implementation plus scoped regression evidence;
VERIFY exposes the named acceptance/recheck still owed; VERIFIED needs a
frozen-candidate gate and none is awarded here. For historical incidents
K11/K12/K21, VERIFY retains the existing instruction to rerun safeguards;
it does not assert that their original root cause was proved fixed.
DECIDED names a user scope decision. CLOSED preserves its reason (K9 was
closed without a fix; K27 was closed by its A/B result). OPEN names an owner
and whether it blocks alpha. A scoped PASS, a merged fix and full release
acceptance remain different facts.

All **52 primary cells and 41 historical recheck cells** are retained
verbatim after the current first sentence. Two grouped rows are expanded;
K15/K16/K27/K28 lacked standalone recheck rows and now have one without
invented historical results. The older chronology remains history, including
its contradictory branch/NOTRUN wording. This task does not rewrite those
evidence pages, decide disputed acceptance or erase original FAILs.

## State table

| State | Count | Rows |
| --- | --- | --- |
| FIXED | 11 | K3, K7, K24–K26, K28, K30, K31, K45–K47 |
| VERIFIED | 0 | No frozen-candidate gate newly certified |
| VERIFY | 35 | K1, K2, K4–K6, K8, K11–K22, K29, K32–K44, K49–K51 |
| DECIDED | 2 | K10 (D40), K23 (D40/D42 scope) |
| CLOSED | 2 | K9 (D41, without resolution), K27 (fresh-format A/B/A′) |
| OPEN | 2 | K48 (Task S/coordinator attribution), K52 (Task X classification) |

The [checklist](alpha-1.0-2plus1-checklist.md) mirrors these states in both
K tables. Its Stage 1 register, candidate/profile and flag-audit rows share
these counts and link the remaining work. Candidate/profile is NOTRUN for
3/4/>4 physical nodes; K28's implemented 16-node cap no longer justifies the
old “UNSUPPORTED until K28” cell. This does not qualify every supported N.
A1/A2/A4/A5 still need a frozen identity, profile, failure domains and sizes.
The flag audit remains NOTRUN for Task Z; K-state counts are not A6–A12
selector/audit counts. Task Z owns the functional-scope decision sheet,
including A4, S4/W122, F12 and power-loss scope. No decision is made here.

## Remaining VERIFY work

The coordinator owns release-profile gates unless another task is named.
None is silently closed by a narrower loopback or sanitizer run:

| Row | Existing implementation/evidence and work still owed |
| --- | --- |
| K1 | T2 merged (`c083b98b5`, `de98832b7`); coordinator owes retained orcd-mini `stop.sh`/blocked-handler qualification; nuc stop evidence remains summary-only ([T2 evidence](k01-t2-forced-exit-bound-20261022.md)). |
| K2 | proposal allow-list merged (`a5eb7e6ff`, wave 9 `9ae2f36d3` follow-ups); Task C submit gate PASS scoped, overall decoder acceptance still pending in its evidence ([Task C](task-c-20261025.md)). |
| K4 | declaration gate merged (`918ed9207`); coordinator owes `w125_p3_compat.sh` hardware gate ([W133](../backlog/work-items.md#w133)). |
| K5 | session/T1 fixes merged (`a443863b1`, `3bcde5e2a`, `9a88e332e`); retained fence, partitioned-client and leader-change barrier gates remain W72/coordinator work; nuc report is not fresh raw qualification ([T1](k05-t1-abandon-capture-20261022.md)). |
| K6 | fail-closed apply merged (`08cc16af4`; Task B wave 15 `10aa4244b`); W130 halt/resync PASS on loopback, distinct-host/RDMA decoder qualification remains ([Task B](k22-k6-loopback-taskb-20261025.md)). |
| K8 | K8 patch merged in Oct 22 wave (`50003197c`, code `7a3a6a18e`); historically cited `71ce4ff35` is not an ancestor but has the same stable patch-id; retained fixed-build W148 rerun still owed after nuc capture loss ([W148 evidence](review-pack-validate-once-20261022.md)). |
| K11 | coordinator owes Linux-FUSE W55 publication/cold-byte qualification; historical trigger remains unisolated, NFS/macOS moved to W152 ([validation](known-bugs-validation-20261008.md)). |
| K12 | coordinator owes W58 exclusive-create/lost-reply live recheck; historical trigger remains unisolated ([validation](known-bugs-validation-20261008.md)). |
| K13 | ticket/default, D31 collector and dead-client drain merged (`00c865924`, `e6a21692d`); coordinator owes live abandoned-upload collection, RDMA and distinct-host acceptance ([D31 drain](k13-d31-drain-20261024.md)). |
| K14 | atomic publication merged (Oct 23 wave 4, `d2369a49d`); loopback torn-read gate PASS, RDMA/orcd-mini/multi-host gate NOTRUN ([evidence](k14-w77-atomic-publication-20261023.md)). |
| K15 | completion/abandon fixes merged (`a51832413`, `9a88e332e`); Task N warm/peer/cold append and lost-reply gates PASS scoped, broader RDMA/multi-host/combined-fault acceptance pending ([Task N](k15-k29-append-gates-20261027.md)). |
| K16 | identity format merged (`e9ca0d07b`); coordinator owes named integrity/hardware/RDMA gates on a fresh-format candidate; unrelated RDMA passes do not automatically close them ([evidence](k16-w75-identity-format-20261022.md)). |
| K17 | coordinator owes D27/0a STALE-convergence C1 rerun on orcd-mini; no new run in this reconciliation ([work queue](work-queue.md)). |
| K18 | coordinator owes W54/W38 fold/GC hardware gate on distinct RDMA hosts; Task L single-host tests are narrower ([work queue](work-queue.md)). |
| K19 | coordinator owes W61/W62/W63 final-drain and lost-ledger reconciliation gate on the alpha profile ([work queue](work-queue.md)). |
| K20 | B5 one-QUOTA fix merged (`5a09fd938`), 3–8 loopback members checked; named real-host capacity/protection-debt acceptance remains NOTRUN ([B5](b-geometry-capacity-20261024.md)). |
| K21 | coordinator must rerun the original W119 RDMA/query/blocked-stop gates before attributing the historical failures; W65 TCP-only closure is narrower ([W119 evidence](orcd-devel-qualification-20261015.md)). |
| K22 | bounds/election fixes merged (`28883f2b1`, `001b5da30`, Task B wave 15 `10aa4244b`); small loopback snapshot/admission PASS, production-scale plateau/1 GiB lag and RDMA/multi-host gates remain ([Task B](k22-k6-loopback-taskb-20261025.md)). |
| K29 | batching/probe fixes merged (`6b621993d`), Task N sustained/step-down/lost-reply runs PASS scoped; coordinator owes multi-host/RDMA append qualification, not a placement-performance conclusion ([Task N](k15-k29-append-gates-20261027.md)). |
| K32 | batching merged (`1cd2092a4`, code `ea2dc4443`); Task U cost profile is measurement only, coordinator still owes orcd-mini ior easy-write target with K13/K14 gates ([fix](k32-write-fix-20261023.md), [Task U](k32-write-cost-profile-20261028.md)). |
| K33 | formation/addition/ids/client view/payload acceptance merged (`0098e06c4`, `53080d744`, `2ebd4fd85`; join-retry wave 11 `d3051f273`); coordinator owes cross-subnet, RDMA and distinct-host addition under load, beyond Task K scoped live checks ([acceptance](k33-acceptance-20261024.md), [join retry](k33-join-retry-20261025.md)). |
| K34 | stopping-member/returned-member fix merged (`fc5c1d64e`); 20/20 loopback payload runs PASS, coordinator owes live multi-host stop under load ([evidence](k34-stop-put-ack-20261024.md)). |
| K35 | D42 degraded-fsync proof merged (`376369aa8`); scoped loopback gates PASS, multi-host/RDMA stripe-durability acceptance remains ([evidence](k35-fsync-degraded-20261024.md)). |
| K36 | D42 verified recovery/segment marking merged (`0f0a79eac`, `fc3af9169`); coordinator owes distinct-host/current-profile recovery gate; power-loss scope is a separate user decision, not an inferred PASS ([recovery](k36-verified-recovery-20261024.md), [segments](k36-k37-segment-recovery-20261024.md)). |
| K37 | segment rebuild/recovery marks merged (`0061a2343`, `fc3af9169`); scoped loopback and Task L checks do not replace distinct-host rebuild acceptance ([evidence](k37-segment-rebuild-20261024.md), [Task L](rdma-gate-node9901-20261027.md)). |
| K38 | replay fixes merged (wave 9 `9ae2f36d3`, Task G follow-up wave 19 `21630289a`); coordinator owes real-cluster/RDMA shared-chunk and orcd-mini ior hard rerun ([original](k38-stale-span-replay-20261025.md), [Task G](k38-task-g-repair-20261027.md)). |
| K39 | one-loss client fixes merged (wave 9 `9ae2f36d3`); K35 20/20 scoped PASS, live multi-host stop/RDMA/orcd-mini remain; separate K33 read EIO is not closed here ([evidence](k39-write-eio-one-member-down-20261025.md)). |
| K40 | submitted-unit ABORT merged (wave 9 `9ae2f36d3`, Task G wave 19 `21630289a`); coordinator owes real-cluster/RDMA two-client shared-chunk acceptance ([original](k40-two-client-unit-busy-20261025.md), [Task G](k38-task-g-repair-20261027.md)). |
| K41 | packer/liveness fix merged (wave 12 `78b9e0981`); growth 10/10 scoped PASS, live multi-host/RDMA/orcd-mini gates remain ([evidence](k41-packer-placement-20261025.md)). |
| K42 | certified cleanup fix merged (wave 13 `899c0a2e0`); 20/20 loaded stop runs PASS, loopback-5 gc-reclamation and live multi-host gates remain ([evidence](e2-clean-stop-busy-20261025.md)). |
| K43 | abort-decided reap fix merged (wave 26 `4e4f15307`); unit negative control and scoped incident-chain/soak checks recorded, coordinator owes four-hour soak and multi-host/RDMA reruns ([evidence](k43-coordinator-kill-20261026.md)). |
| K44 | retired-seed routing merged (wave 18 `08701031d`); private TCP/RDMA and authorized orcd-mini checks PASS scoped, merged-candidate and cold-query acceptance remain incomplete ([evidence](k44-retired-replica-routing-20261026.md)). |
| K49 | RDMA alignment fix merged (wave 23 `6967bfda2`, code `f78bd66f9`); retained real-HCA/sanitizer checks PASS scoped, integrated-candidate acceptance remains ([Task L](rdma-gate-node9901-20261027.md)). |
| K50 | RMDIR deadline fix merged (wave 22 `a7e881a7a`, code `b78955995`); scoped negative/positive and repeated gates PASS, broader W159 latency and integrated/live acceptance remain ([Task O](dir-ops-busy-under-load-20261027.md)). |
| K51 | heartbeat lifetime fix merged (wave 23 `6967bfda2`, code `f78bd66f9`); TSan/ASan and RDMA checks PASS scoped, integrated-candidate acceptance remains ([Task L](rdma-gate-node9901-20261027.md)). |

## Contradictions and stale wording reported

These are traceability findings, not fresh runtime verdicts. Present-tense
prefixes state the later recorded fact; historical evidence is retained and
its owner must resolve any remaining acceptance conflict.

| Rows | Conflicting/stale statements and bounded treatment |
| --- | --- |
| K8 | Main row calls `71ce4ff35` merged, but it is **not an ancestor** of reviewed devel; `7a3a6a18e` is, via `50003197c`. Both complete diffs have stable patch-id `ae288ad846e073f98f3d64d762dadac8e0217d14`. Prefix gives the actual merge/patch identity; historical citation stays. Fixed-build W148 rerun remains owed because nuc captures were lost; no visibility/durability inference is added. |
| K13 | Early text says default path/collector not merged and collection INCOMPLETE; later evidence records `00c865924` collector and `e6a21692d` dead-client drain merged. The drain page says scoped unit/in-process PASS and live collection NOTRUN. VERIFY names that remaining gate; no all-member live PASS is manufactured. Historical `a79b59de1` is a merge **into devel-k13**, not itself proof of the final devel merge. |
| K33 | “IN PROGRESS” conflates implementation with acceptance; committed increments, client-view refresh, payload/growth and wave-11 join-retry are recorded. Cross-subnet, multi-host/load and RDMA acceptance is still broader than the recorded scoped Task K checks. The join-retry page still says not merged; current prefix cites the later merge and keeps its original outcome unchanged. |
| K38–K41 | Main/evidence text says merge pending/not merged while recheck/merge records show wave 9 `9ae2f36d3`, wave 12 `78b9e0981`, and Task G wave 19 `21630289a`. Prefixes expose merged code plus owed real-cluster gates. Old “not merged” prose remains a dated report, not an instruction to merge again. |
| K43 | Main row begins FIX and recheck says K42 is unmerged, while wave 26 records `4e4f15307`. The page proves the aborted-stage unit mechanism; quiet incident-chain runs never reproduced the original application failure. Four-hour loaded soak/multi-host reruns remain owed. Task U's baseline D4 failure is not erased by the reap fix. |
| K26 | Original page says not merged and ORCD/x86_64 NOTRUN; later nuc PASS is historical summary-only after raw loss. Independent efs1/efs2 25/25 and merged gate remain credited. Task L ran ASan/UBSan with `detect_leaks=0` and lacked `valgrind/memcheck.h`: it **does not close leak verification**. Do not interpret its sanitizer PASS as LSan/Memcheck PASS. |
| K30 / W126 | Historical text says peek ordering OPEN; later W126 evidence/work queue records `d5a1df25e` merged with TSan/Memcheck/native checks. The old page still says not merged. Prefix credits the scoped fix, not the separate pending orcd-mini performance A/B. |
| K2 | The submit gate's scoped PASS is recorded, but Task C's page still says overall decoder acceptance PENDING/reruns active, despite merged code. This is not resolved by Task Y; VERIFY retains the larger acceptance gap. |
| K6 / K22 | Old “snapshot/admission NOTRUN” phrases precede Task B's loopback PASS and wave-15 merge; later Task J adds capacity/fencing corrections. None demonstrates production-scale plateau/1 GiB lag on distinct RDMA hosts. VERIFY preserves that boundary. |
| K9 / K10 / K27 | Historical W87 OPEN/INCOMPLETE is followed by D41 CLOSED without resolution; K10's recheck “Open” is a deferred kernel fix under D40, not an undecided scope item; K27 starts VERIFY but its evidence explicitly closes fresh-format regression. These leading words now follow the recorded decisions/evidence, without changing the old test results. |
| K23 / K36 | D40's broad W123 deferral coexists with D42's explicit verified crash-recovery inclusion. Both are cited: K23 stays DECIDED, K36's current-profile gates stay VERIFY, full power-loss qualification remains a separate scope decision. No new repair/power-loss scope is chosen. |
| K11 / K12 / K21 | Register VERIFY wording can look like an established fix; W55/W58/W119 evidence/queue still says original trigger/cause open or unisolated. Prefixes explicitly mean recheck before attribution, not “original incident fixed.” Owners must settle the result with retained runs. |
| K44 / K49 / K50 / K51 | “Pending merge/merged-candidate acceptance” combines two different tasks: merge ancestry is present, integrated acceptance is still owed; narrowed prefix removes only the apparent need to merge again. Task L's real-HCA result is single-host, Task O leaves broader W159 latency OPEN, and Task K cold queries are INCOMPLETE. |
| K48 | Wave 28 merged Task S's same-signature layout race fix, but its evidence explicitly keeps original attribution OPEN. Task N lacks the missing filename/RPC trace; Task V separately captured f0057 after resync. Neither later passing runs nor this reconciliation close either historical failure by inference. |
| K52 | Task V's eight immediate mismatches remain OPEN. Later matching reads do not prove freshness at the failed read. Task X must classify against K10 with exact observations; Task Y does not decide that they are equivalent. |
| Stage 1 / chronology | “UNSUPPORTED until K28,” old W153/bootstrap instructions and old fixed/open tallies are historical after later membership/cap merges. Current register counts/profile cells are explicit, while chronology is retained. Missing rows and buried outcomes are structure errors; they do not become passing product gates when rearranged. |

## Checker and focused validation

`python3 scripts/check_k_register.py [CHECKLIST] [--json]` reads only the two
named table headers. It fails on missing/empty/repeated tables, noncanonical
plain prefixes, grouped/invalid ids, duplicate numbers, missing K numbers in
either table (including gaps missing from both), orphan K rows, incorrect
cell counts or state disagreement. It does **not** infer a verdict from
keywords in history, verify raw captures, validate merge ancestry on every
`make test`, or award acceptance. Those are human evidence-review duties.

Both the checker and `tests/test_k_register.py` are wired into the fast
`make test` recipe. Seven negative-control test methods cover all six valid
states/history, later-word false positives, duplicates, gaps, absent tables,
grouped ids, disagreement, and CLI exit/JSON behavior. Running the checker
against the reviewed base is an expected **FAIL**; the reconciled version
is PASS. One early duplicate-fixture test placed its row after a Markdown
blank separator; the fixture was corrected and orphan-row rejection added,
without relaxing duplicate detection.

Validation is on macOS with system Python 3, no daemon build, host fixture,
quota or network use. Exact commands:

```sh
python3 scripts/check_k_register.py
python3 scripts/check_k_register.py --json
python3 tests/test_k_register.py
python3 tests/test_make_test_recipe.py
make -n test
python3 docs/gen-architecture-full.py
python3 docs/check-architecture.py
```

Expected: 52 rows per table and the counts above; seven test methods PASS;
recipe detects both commands as wired; docs gate 5/5. A full Linux
`make test` / `make test-all`, live profile, raw-evidence revalidation and
Task Z's audit are **NOTRUN by Task Y**. This Mac-only task does not reserve
a shared host or reuse another agent's test results as a new Task Y PASS.
The recipe's scheduling model is reported as a model, not observed wall time;
adding two fast Python checks does not raise D44's 1100 s target/1650 s bound
or change a long-test slot/budget.

## Retention

Source/ancestry audit, preserved-history counts, expected-base failure,
focused validation output, input hashes and manifests are outside Git at
`/Users/mike/Documents/Codex/artifacts/taskY-20261028/`. No runtime stores,
payloads, profiler output or graph copies are committed. No test host or
cluster was touched; no host cleanup is needed. The reviewed source is
reproducible from the commit and commands above. Closed manifest: **18 files**, all hashes rechecked; `SHA256SUMS` SHA-256
`a3937edfd38b4f18cb098af6165699b051a07a724156510293059e112fda6367`.
`PRUNED.txt` records that no host runtime bulk was created; audit inputs and
text results are retained. Focused results follow.

## Closed focused results

- Checker: **PASS**, 52 rows per table and counts exactly as above;
  checker on unchanged reviewed-base Markdown: **FAIL as expected**.
- `tests/test_k_register.py`: **7/7 PASS**; CLI failure is nonzero and its
  JSON/count output is checked. `make -n test` confirms both new commands.
- Recipe check: **PASS**; 940 enumerated commands in this unbuilt Mac tree,
  73 unit binaries / 424 Python tests (one explicitly listed unwired),
  91 budget rows; scheduling model **1282 s**, above D44's **1100 s target**
  but below its unchanged **1650 s bound**. This is not a measured full gate.
  No budget/slot/target is changed by Task Y.
- Existing `tests/test_code_graph.py`: **9/9 PASS** (known-path/negative
  controls); this is not a graph freshness or runtime correctness claim.
- Merge audit: **60 cited commits**, 59 ancestors plus the documented K8
  patch-equivalent exception; **73 reviewed input hashes** recorded.
- Preserved history: **52 primary + 41 old recheck cells**, with four
  previously missing standalone recheck rows supplied and two grouped rows
  expanded. No old cell text deleted.
- Architecture docs gate: **5/5 PASS**; no full Linux/live qualification
  rerun. Stage 1 and the named OPEN/VERIFY acceptance stay open.
