# TCP/direct-I/O common-source qualification — October12

User explicitly resumed assigned groups and requested15-minute reports; recurring
progress heartbeat v0-3-0-qualification-progress is active. The later human policy
waives old-binary/data preservation and backward-compatibility efforts. AMD,
Intel and NUC were notified and owners acknowledged. Identified EFS stores may
be wiped where easier; test evidence and unrelated services remain protected.

Previous centrally frozen candidate was **def46109909c793e58ae2c453c5de537cdb2c627**
on qualification-v030-tcp-direct. It adds a test-only operation-extractor mock to
reviewed2bf leader-hint correction. All owners received exact def for renewed
native/affected sanitizer/live gates. Source implementation is identical to2bf,
but no fullnative pass is inferred from its failed fixture. Root/owner launch
results must identify full def SHA. Earlier52 and2bf evidence stays source-specific;
candidate is not release accepted or integrated.

| Group | Cluster | Historical common52 result |
| --- | --- | --- |
| Read-progress safety | AMD | Full build/actual Raft normal+earlyACK, coherent completion/publication and full Raft ASan/UBSanPASS. Supplemental actual-core both-record/context checksPASS after correcting invalid test reply; supplemental sanitizerPASS0. |
| Seven-client packing/query | Intel | Quiet original7/jobs16/directTCPFAIL1 (~124s), broad POSIX timeouts plus client5 graceful teardown failure. Peer/cold/physical dependent checksNOTRUN. Same-source labelled diagnostic planned; no quiet pass. |
| Files POSIX/peer/mutation/GC | NUC | Owner reports core216/216 and peer67/67PASS; complete direct/files core+physicalGC96.290sPASS; separate two-client packing18.323sPASS with exact coldhashes, emptyphysicalpacks and drainedqueues. Raw evidence reviewed and integrated as3c98c13e; [NUC gate report](nuc-release-52bc-group3-20261012.md). |
| Final merged qualification | AMD | Common candidate full normal/fault nativePASS0 (~90s each); final release integration still pending group2 resolution and evidence reconciliation. |

[AMD exact raw logs and limits](../../results/measure/20261012-common52-amd/README.md).
Intel sourcepost23,318inputsMATCH. Per-client PASS/FAIL/SKIP/NOTRUN totals:
0=171/18/1/31,1=184/18/1/18,2=175/21/1/24,3=171/18/1/31,
4=165/22/1/33,5=177/19/1/24,6=176/19/1/25. Original budgets unchanged.
No container/repair qualification, push, tag or promotion. Previously stopped/
failed broader-source evidence remains immutable; none of these native passes
qualifies the failed quiet live test.

The common52 AMD quiet comparison subsequently failed, as recorded below. Intel diagnostics remain separate from quiet acceptance.


## Failed common52 high-concurrency comparisons and next freeze

[AMD quiet7 exact evidence](../../results/measure/20261012-common52-amd-quiet/README.md):
FAIL1 at extra-client2 graceful stop,51.848s; stats/findfailall7,deepfail2/5.
Peer/cold/physicalNOTRUN. No owned fixtures remain. [Intel exact quiet and
separate diagnostics](../../results/measure/20261012-intel-common52/README.md)
are integrated as evidence only; broad POSIX timeouts/teardown remainFAIL.
Diagnostic found no admitted-read or publication-deadline trace; accumulated
fresh-read latency and repeated transaction-cut/root-catalogBUSY are measured,
not permission to relax authority or proof of full root cause.

NUC reported a separately labelled52seven runFAIL1 in201.841s: all7 deepnesting
30sFAIL plus ENOSPC/EIO cases. It used default1G per-node fixture quota, unlike
AMD/Intel4G; usage966,332,416/1,073,741,824 triggered admission despite host207GiB
free. Do not infer equivalent workload or successful hint A/B from this run.
Next2bf seven-client comparisons explicitly4G everywhere; deadlines unchanged.
NUC retains original quota/admission failures. Core/isolated2 packing group3
passes remain their named scope, not high-concurrency acceptance.

Intel logical fix2bf resets successhint to the returned inode group's coherent
leader after paired/covered reads, preserving errorhints and freshproofs.
Original-source negative regression fails intended hint assertion; corrected
lookup/paired/remote-pair extraction regressionsPASS locally. Centralreview
matches client rpc_learn_leader grouping by returnedinode. Allclusters received
exact newcandidate2bf; fullqualifications still required, no causal performance
or release acceptance inferred.


## Test adapter correction and current def freeze

[AMD2bf original native failures](../../results/measure/20261012-common2bf-amd/README.md):
normal/fault buildsPASS, nativeFAIL2 because second actual helper extraction lacks
host_view/mockgetter; changed-hint/pair ASan/UBSanPASS. Intel also refused live
launch after the same prerequisite failure. def46109 supplies only the missing
adapter/coveredhintassertion; all actual extraction consumers audited locally.
NUC's already-running2bfcore measurement continues unmodified; queued later2bf
launches are blocked intentionally, not misclassified as a product gate failure.
All owners must switch to def after active child cleanup, with4Gseven fixtures
and unchanged deadlines. No source divergence, failure erasure or acceptance.

## Current def results — October 12 progress checkpoint

All results below identify frozen `def46109909c793e58ae2c453c5de537cdb2c627`; candidate remains unaccepted.

| Gate | Cluster | Result |
| --- | --- | --- |
| Full normal/fault native and affected sanitizers | AMD | PASS: both complete native suites, early-ACK Raft and four sanitizer scripts. [Raw evidence](../../results/measure/20261012-commondef-amd/README.md). |
| Core/peer/physical GC and isolated two-client packing | NUC | Owner reports PASS: core 216/216, peer 67/67; isolated packing PASS. Exact-source owner evidence commit pending integration. |
| Seven-client/jobs16, explicit 4G, original deadlines | Intel | FAIL: approximately 119s, client2 graceful-stop failure; dependent peer/cold/physical NOTRUN. Diagnostic evidence being finalized. |
| Seven-client/jobs16, explicit 4G, original deadlines | NUC | FAIL: all seven deep-nesting 30s deadlines, plus client5 stats deadline. Six clients 219 PASS/1 FAIL/1 SKIP; client5 218/2/1. Peer 67/67, cold hashes, physical packing reclamation and metadata queue drain PASS independently. No ENOSPC failures. |

Intel's labelled diagnostic measures receiver inbox/pump delays (roughly 11–37ms mean inbox ages; drain peaks around 390–445ms), with sampled TCP_NODELAY enabled and wire round trips around 53–74us. These measurements motivate a bounded FIFO receiver-drain investigation; they do not prove an accepted fix. Intel is authorized to propose a narrow scheduling change with focused regressions, preserving durability, ordering and fresh read barriers. NUC holds further launches pending central review and a new common freeze. No budgets changed, no push/tag/promotion or deferred work.

## New central freeze: bounded inbox slices

Current frozen candidate is **03bd89fb7de56c2c6af44ab3bd26c54caf4b8274** on qualification-v030-tcp-direct. Reviewed Intel logical fix 6b097303 takes at most eight FIFO inbox frames per pump cycle, keeps older retained frames ahead of arrivals, charges only processed bytes/frames to unchanged durability retirement, and polls without sleeping while retained work exists. Existing receive, term/configuration, read admission and durability paths remain unchanged. This bounds frame count, not elapsed time; individual large frames and extra sync overhead remain risks to measure. Actual helper ordering/accounting/backlog tests and operation/LOOKUP regressions PASS locally; owner reports sanitizer and negative mutants PASS/fail as intended. Full native and live acceptance NOTRUN on this new source. Intel and NUC must finish old diagnostic/evidence cleanup before renewed same-source 4G original-budget tests. AMD runs full normal/fault and affected sanitizer gates. No acceptance inferred from def passes.

## 09:10 EDT progress on current 03bd freeze

AMD completed full normal/fault native, early-ACK Raft and all five affected sanitizer scripts: **PASS**, exact source verified. [Raw evidence](../../results/measure/20261012-common03bd-amd/README.md). NUC reports core **216/216 PASS**, peer **67/67 PASS**, physical checks and isolated two-client packing **PASS**. Intel build and 13 focused prerequisites **PASS**. Intel and NUC matched-4G quiet seven-client gates **RUNNING**, final acceptance **NOTRUN/pending** until those results and evidence review finish. Prior def/52 deadline failures remain retained. Next action: collect quiet verdicts without overlapping measurement with builds/profiling; review any remaining failures before a further source change.

## Final 03bd live verdicts

[NUC exact-source report](nuc-release-03bd-group3-20261012.md) and raw evidence integrated: core/peer/physical and isolated packing PASS; seven-client FAIL174.267s. All seven deep cases timed out30s; clients2/6 stats and client4 find also timed out. Seven-run peer67/67, cold hashes, physical reclamation and queue drain PASS independently. Intel reports quiet FAIL146s at client3 graceful stop; dependent peer/cold/physical NOTRUN, evidence commit pending. Bounded eight-frame slices did not clear either acceptance gate; Intel per-client durations were worse than previous def, so no performance benefit is claimed. Intel is preparing a separate seven-client deep-only syscall/phase diagnostic with the original test and30s budget; source remains03bd. No further implementation freeze until evidence identifies a justified change.
