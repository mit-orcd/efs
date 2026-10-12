# TCP/direct-I/O common-source qualification — October12

User explicitly resumed assigned groups and requested15-minute reports; recurring
progress heartbeat v0-3-0-qualification-progress is active. The later human policy
waives old-binary/data preservation and backward-compatibility efforts. AMD,
Intel and NUC were notified and owners acknowledged. Identified EFS stores may
be wiped where easier; test evidence and unrelated services remain protected.

Current centrally frozen candidate is **2bf7e257e2a36d7f8779b770c8eca3d967ab5835**
on qualification-v030-tcp-direct. It adds only the reviewed successful inode-group
leader-hint correction and its regression to prior52bcfd35. All three owners
received this SHA for affected reruns; do not test moving/divergent branch tips.
AMD normal/fault/changed-host sanitizer reruns and Intel/NUC live reruns are
launched/preparing. Candidate remains outside accepted release integration.
Earlier52 results below retain their own source identity.

| Group | Cluster | Current result |
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

A same-source AMD original-budget quiet7/jobs16 comparison is running after all AMD builds/sanitizers completed, to isolate Intel broadtimeout/RI latency behavior. No profiling or concurrent AMDbuild. Intel labelled READ_PROF/query-stage diagnostic follows its failedquietrun; no diagnostic acceptance transfer.


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
