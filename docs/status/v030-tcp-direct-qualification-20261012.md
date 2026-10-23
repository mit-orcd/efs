# TCP/direct-I/O common-source qualification — October12

User explicitly resumed assigned groups and requested15-minute reports; recurring
progress heartbeat v0-3-0-qualification-progress was active during the historical split; it is now paused and root works alone. The later human policy
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
| Seven-client query | Intel | Quiet original7/jobs16/directTCPFAIL1 (~124s), broad POSIX timeouts plus client5 graceful teardown failure. Peer/cold/physical dependent checksNOTRUN. Same-source labelled diagnostic planned; no quiet pass. |
| Files POSIX/peer/mutation/GC | NUC | Owner reports core216/216 and peer67/67PASS; complete direct/files core+physicalGC96.290sPASS. Raw evidence reviewed and integrated as3c98c13e; [NUC gate report](nuc-release-52bc-group3-20261012.md). |
| Final merged qualification | AMD | Common candidate full normal/fault nativePASS0 (~90s each); final release integration still pending group2 resolution and evidence reconciliation. |

[AMD exact raw logs and limits](../../results/measure/20261012-common52-amd/README.md).
Intel sourcepost23,318inputsMATCH. Per-client PASS/FAIL/SKIP/NOTRUN totals:
0=171/18/1/31,1=184/18/1/18,2=175/21/1/24,3=171/18/1/31,
4=165/22/1/33,5=177/19/1/24,6=176/19/1/25. Original budgets unchanged.
No repair qualification, push, tag or promotion. Previously stopped/
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
NUC retains original quota/admission failures. Core group3
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
| Core/peer/physical GC | NUC | Owner reports PASS: core 216/216, peer 67/67. Exact-source owner evidence commit pending integration. |
| Seven-client/jobs16, explicit 4G, original deadlines | Intel | FAIL: approximately 119s, client2 graceful-stop failure; dependent peer/cold/physical NOTRUN. Diagnostic evidence being finalized. |
| Seven-client/jobs16, explicit 4G, original deadlines | NUC | FAIL: all seven deep-nesting 30s deadlines, plus client5 stats deadline. Six clients 219 PASS/1 FAIL/1 SKIP; client5 218/2/1. Peer 67/67, cold hashes, physical reclamation and metadata queue drain PASS independently. No ENOSPC failures. |

Intel's labelled diagnostic measures receiver inbox/pump delays (roughly 11–37ms mean inbox ages; drain peaks around 390–445ms), with sampled TCP_NODELAY enabled and wire round trips around 53–74us. These measurements motivate a bounded FIFO receiver-drain investigation; they do not prove an accepted fix. Intel is authorized to propose a narrow scheduling change with focused regressions, preserving durability, ordering and fresh read barriers. NUC holds further launches pending central review and a new common freeze. No budgets changed, no push/tag/promotion or deferred work.

## New central freeze: bounded inbox slices

Current frozen candidate is **03bd89fb7de56c2c6af44ab3bd26c54caf4b8274** on qualification-v030-tcp-direct. Reviewed Intel logical fix 6b097303 takes at most eight FIFO inbox frames per pump cycle, keeps older retained frames ahead of arrivals, charges only processed bytes/frames to unchanged durability retirement, and polls without sleeping while retained work exists. Existing receive, term/configuration, read admission and durability paths remain unchanged. This bounds frame count, not elapsed time; individual large frames and extra sync overhead remain risks to measure. Actual helper ordering/accounting/backlog tests and operation/LOOKUP regressions PASS locally; owner reports sanitizer and negative mutants PASS/fail as intended. Full native and live acceptance NOTRUN on this new source. Intel and NUC must finish old diagnostic/evidence cleanup before renewed same-source 4G original-budget tests. AMD runs full normal/fault and affected sanitizer gates. No acceptance inferred from def passes.

## 09:10 EDT progress on current 03bd freeze

AMD completed full normal/fault native, early-ACK Raft and all five affected sanitizer scripts: **PASS**, exact source verified. [Raw evidence](../../results/measure/20261012-common03bd-amd/README.md). NUC reports core **216/216 PASS**, peer **67/67 PASS**, physical checks **PASS**. Intel build and 13 focused prerequisites **PASS**. Intel and NUC matched-4G quiet seven-client gates **RUNNING**, final acceptance **NOTRUN/pending** until those results and evidence review finish. Prior def/52 deadline failures remain retained. Next action: collect quiet verdicts without overlapping measurement with builds/profiling; review any remaining failures before a further source change.

## Final 03bd live verdicts

[NUC exact-source report](nuc-release-03bd-group3-20261012.md) and raw evidence integrated: core/peer/physical PASS; seven-client FAIL174.267s. All seven deep cases timed out30s; clients2/6 stats and client4 find also timed out. Seven-run peer67/67, cold hashes, physical reclamation and queue drain PASS independently. Intel reports quiet FAIL146s at client3 graceful stop; dependent peer/cold/physical NOTRUN, evidence commit pending. Bounded eight-frame slices did not clear either acceptance gate; Intel per-client durations were worse than previous def, so no performance benefit is claimed. Intel is preparing a separate seven-client deep-only syscall/phase diagnostic with the original test and30s budget; source remains03bd. No further implementation freeze until evidence identifies a justified change.

Intel03 hard-error follow-up: owner also reports client1 fsync_reopen_visible EIO, client4 basic_write_read EIO and client6 basic_seek_cur_past_eof_write EBUSY. These are distinct runtime blockers alongside broad timeouts, not deadline-only failures. Retained flush/RPC/FUSE logs require classification; no data-loss inference or runtime safety acceptance follows from native PASS. The bounded-slice candidate remains unaccepted.

## Stop checkpoint

Intel reports a newer direct human instruction: “Document, commit, and stop.” Root stops new launches/fixes and disables progress heartbeat pending explicit human resume. Candidate remains03bd89fb and unaccepted; native PASS, seven-client FAIL, Intel EIO/EBUSY classification unresolved. NUC fixtures already stopped; AMD native controller finished. Intel will retain completed deep diagnostic and stop. Root created isolated branch/worktree fix-release-readdir-fence at03bd for review, with no implementation edits or deployment. Code inspection found READDIR repeats its directory-group ReadIndex before lane checks; request-local reuse is a possible follow-up, not an implemented or measured fix.

## Alpha scope update: `.efs` excluded

Latest human decision removes `.efs` from this alpha: clients EFS_COMMANDS=0 and daemons EFS_COMMANDS_SERVER=0, query-specific cases excluded explicitly. Previous WITH-query failures remain historical failures; feature-disabled live acceptance NOTRUN. Deep paths and ordinary EIO/EBUSY/flush checks remain required. See [scope contract](v030-release-scope-20261012.md). No launches resumed.

## Human-authorized resume and new freeze

Current frozen source **ecdc8c6034b610a2bf69e7a79664d8a5e990de84**. Explicit disabled-feature harness added and syntax-checked; product source unchanged03bd. Resource split AMD heavy full/native, Intel medium focused I/O, NUC light deep/configuration. Owners notified of direct resume; archive/manifests exporting. New-source live acceptance NOTRUN, AMD native launch preparing and broad quiet gate queued behind successful native completion. Existing failures retained.15-minute heartbeat resumes. [Assignment contract](v030-release-scope-20261012.md#resumed-resource-weighted-split).

## Feature-disabled split: focused owner results

Intel exactecdc focused ordinary I/O/seek/fsync/reopen **70/70 PASS** (10 original cases on7simultaneous clients, jobs16,30s); all7graceful client stops PASS. [Integrated evidence](../../results/measure/20261012-intel-ecdc-focused/README.md). Actual4daemon/7client config and23,318source inputs verified. Private local fixture, reduced scope; daemon cleanup is not an independent graceful acceptance, and no causal `.efs` attribution is established. NUC reports focused deep60/100depth **14/14 PASS**, config/hidden `.efs`/basic I/O/fsyncedpeer/rename/unlink and cleanup PASS; owner evidence commit pending. AMD native controller finished; full seven-client core acceptance RUNNING. Focused passes do not replace heavy acceptance.

NUC owner commitd97214e7 reviewed/integrated with [exact-source report](nuc-release-ecdc-focused-20261012.md) and raw evidence. Focused14/14 and config/basic/peer/cleanup PASS retained; no duplicate launch.

## 10:04 EDT ecdc progress checkpoint

AMD full normal/fault native,early-ACK and five affected sanitizer scripts **PASS** on exactecdc. [Native evidence](../../results/measure/20261012-commonecdc-amd-native/README.md). Full disabled-query seven-client core **FAIL281.113s**: clients1–6 214/216,client0 213/216; all deep and names_crazy_dirs timeout30s,client0 beyond64 also. Extra-client unmounts/peer/cold acknowledged bytes completed. Global physical-empty assertion **FAIL180s**, final queue oracle **NOTRUN**; residual live failed-test namespaces may confound it, object attribution pending, no standaloneGC/data-loss conclusion. [Raw failure](../../results/measure/20261012-commonecdc-amd-quiet/README.md). Intel70/70 and NUC14/14 focused passes retained; neither closes broad load. Next root action: classify namespace/RPC/load interaction and scope physical oracle to deleted fixture-owned objects without discarding original failure or changing deadlines. Owners hold duplicate live launches. Candidate unaccepted; no push/tag/promotion.

Root follow-up: isolated READDIR request-local fence reuse committed5e035dcd, focused actual callback/helper/operation regressions PASS; independent review pending, not integrated or deployed. One labelled exactecdc AMD broad diagnostic started after quiet cleanup with RPC/read/raft/batch/phase tracing, same seven/jobs16/4G/core/deadlines. Diagnostic is NOT acceptance; no parallel build. Goal is measured foreground phase/RPC costs and physical-oracle attribution, not speculative deadline relaxation.

## 10:19 EDT diagnostic and reviewed next freeze

Exactecdc labelled diagnostic FAIL281.520s, same deep/names failures plus client5 beyond64; peer67/67 and cold bytesPASS, globalphysicalempty timeout180s. All7observed deep phase sequences reach create but no leaf-write marker (missing datagrams unknown; observer35records/noerrors), directing investigation toward creation/path authority rather than claiming recursive cleanup cause. Cumulative pump histograms on busiest dual node n3: inbox mean12.748ms,max110.985ms; drain mean0.427ms,max96.859ms. Other-node means differ; profilingnotacceptance and no elapsed causal allocation established. Raw diagnostic retained.

Current new frozen candidate **e59e815fab2916340baad585dc08712ed182b7ea** adds reviewedREADDIR request-local fence reuse (logical5e035dcd) toecdc. Independent Intel review found no blocking correctness issue; actual callback/helper/operation regressions PASS. This removes one duplicate directory-group round but doesnotclaim tofix creation or broad deadlines. AMD export/native→quiet queue preparing; newlive gatesNOTRUN. Intel focused I/O/teardown and NUC lightweight focused deep/readdir confirmation assigned on sameSHA. Flags0/packedwrites1/core/4G/originaldeadlines remain. Previous failed evidence immutable; no push/tag/promotion.

Newe59 focused confirmation: [NUC report](nuc-release-e59-focused-20261012.md) integrated,14/14deep and7/7READDIR cases PASS, config/basic/peer/cleanupPASS. Intel reports original70/70IOPASS, but added7listing/6names cases timeout30s (71/84PASS,13FAIL); commit pending. Listing test includes50writes before its listdir/ls, so actual failure stage must be measured, not assumed READDIR. Intel assigned one bounded external-phase diagnostic after evidence retention, original7jobs16/4G/deadlines. AMD e59 normalnativePASS, faultnativeRUNNING, broadNOTRUN/queued.

## Completed e59 and corrected Intel diagnosis

AMD full normal/fault/earlyACK/affectedsanitizers **PASS**; broad quiet **FAIL277.468s**, coldbytesPASS, globalphysicalempty timeout180s. Exact-source raw logs retained; not releaseaccepted. Intel focused/failedobserver/correctedworker commits f134cdda,38d841d8,28c52bdd integrated as evidence only. Corrected observer survivedworkerexec and ordinaryreference2/2passed; actual14workerjournals show listing file-context exit23.8–26.7s cumulative, opens3.1–4.3s,Pythonwrites~2ms. Two reachinglistdir took34.8/42.3ms. Names exits14.5–17.1s, some deadlineendsincleanup. Context exit includes bufferedflush+close; exactRPCstage unmeasured. Supports investigatingflush/publication ratherthan claimingREADDIRcause; no durabilityrelaxation.

Automatic approval review rejected an additional Intel profiled diagnostic, citing a newer human instruction to stop all chats. Root requested clarification because latest directrootinstruction resumes split. No further workloads/fixes launched pending answer; completed evidence/documentation checkpoint continues. Current candidatee59 remainsunaccepted, heartbeat authority to be reconciled with humanreply.

## Direct human clarification: continue

The human answered the pending conflict question: **Continue qualification and diagnostics**. This explicitly resumes diagnostics and supersedes the reported stop;15-minute reporting remainsactive. Root dispatched one targeted samee59 corrected worker diagnostic with existing RPC/read profilers,7jobs16/4G/flags0/unchanged30s, no product edit or build. Objective: identify RPCstage behind measuredflush/context-exit costs before implementation. Currente59 remainsunaccepted; no push/tag/promotion/deferredactivation.

Intel exacte59 existingRPC/read diagnostic70dcd7da integrated:4/14PASS10timeoutFAIL;7clientstopsPASS,source23318MATCH. Per-clientperiodic genericRPC snapshots1514–1695calls,receive14.927–18.575s vs checkout4.277–5.051ms/send36.868–42.8ms; LOOKUP740–846/6.757–8.763s,READDIR35–40/111–222ms. Cumulative bothcases/setup/cleanup,notcloseallocation. No alphaacceptance/deadline/durabilitychange.

## Solo continuation after agent stop

Human authorized root to continue alone, with no other-agent assignments. Intel checkpoint a61c6e18 and NUC5d9e97d0 are committed; both report clean trees and no owned fixtures. Recurring heartbeat remains paused.

## Reviewed release integration: final-source freeze

Human requested integration and qualification of one final SHA. qualification-v030-tcp-direct merged cleanly into release-v0.3.0-pre-alpha as **1e3a46384e897a47751871b15e82ead86c9d0abd**. This is the current frozen qualification source, superseding e59 as execution identity; product/test inputs under src/include/tests/Makefile are identical to e59. Includes reviewed read-completion safety, paired inode leader hint, extraction adapter, bounded FIFO slices, disabled-query harness and request-local READDIR fence reuse. Integration is not acceptance.

Root alone exports all24,334regular tracked files with SHA256 manifest and runs normal/fault native, earlyACK and five affected ASan/UBSan tests, then quiet original seven-client/jobs16/core/4G filesTCPdirect peer/cold/physical/shutdown on ports34740–43 if all prerequisites pass. Both commands flags0, no profilers/build overlap in live phase. RUNNING; previous e59/7ef failures remain open. Raw roots /data1/efs/evidence/amd-release-final-1e3a4638-20261012 and /data1/efs/amd-release-final-1e3a4638-live-20261012. No other agents, new recurring tasks, tag or push.

## Final integrated source verdict, ordinary-files scope

[Final report](v030-final-qualification-1e3a4638-20261012.md): frozen1e3a normal/fault/native/earlyACK/fiveaffectedsanitizers PASS. Ordinaryfiles7client core1505PASS/7FAIL, all failures dir_deep_nesting30s; peer67/67 and7gracefulstopsPASS. Isolatedordinaryoverwrite primary/peer/cold/physical/queuedrainPASS. Bothcommands0 verified on all actual processes.24,334source inputs unchanged,89evidencefilesverified,privatefixturesstopped. Notaccepted: deep-directory concurrency remains a blocker. No otheragents/tag/push.


## Solo deep-directory fix: final release qualification PASS

Frozen **4e239d029029b8105a6e07f0e69a26fdc449de32** supersedes 1e3a as current execution identity. [Exact report](v030-deep-directory-fix-20261012.md): bounded receive prefix8→32 closes ordinary deep-directory timeout without authority/durability changes. Three fresh candidate runs plus final release run each core1512/1512, deep7/7, peer67/67 and seven stops PASS, original budgets unchanged. Discarded routing and one-frame experiments are not integrated; repeat2 setup port conflict was NOTRUN and retained.

Final exact-source normal/fault fullnative, earlyACK and five affected ASan/UBSan tests PASS; isolated ordinary overwrite primary/peer/cold/physical/queuedrain PASS. 24,466 tracked regular inputs unchanged; raw evidence integrity verified. All owned private AMD fixtures stopped. Packing0, commands0/server0, files/TCP/direct. .efs, buffered/RDMA and dedicated repair/recovery remain excluded. Root worked alone; heartbeat paused; no push/tag/promotion. Deep-directory release blocker closed for this scope.

**Raw evidence note (2026-10-23):** the bulk data in `amd-release-final-1e3a4638-20261012` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
