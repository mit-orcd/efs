# TCP/direct-I/O common-source qualification — October12

User explicitly resumed assigned groups and requested15-minute reports; recurring
progress heartbeat v0-3-0-qualification-progress is active. The later human policy
waives old-binary/data preservation and backward-compatibility efforts. AMD,
Intel and NUC were notified and owners acknowledged. Identified EFS stores may
be wiped where easier; test evidence and unrelated services remain protected.

All use exact candidate52bcfd3570b0a59076c225caad9157b0825954d8 on
qualification-v030-tcp-direct, based release2b3ccfe0. This is a qualification
branch, not accepted release integration. [Scope/assignments](v030-release-scope-20261012.md).

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
