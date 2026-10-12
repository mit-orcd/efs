# Intel TCP/direct qualification — October12,2026

Current assigned scope is files engine, TCP/direct I/O, original-budget seven-client packing/query acceptance (group2). AMD coordinates one frozen candidate across AMD/Intel/NUC. The human resumed this assignment after the confirmed stop; earlier stop evidence and failed candidates remain historical evidence. Containers, buffered/RDMA and dedicated repair/recovery qualification are deferred by the centrally documented release scope. No release acceptance, push, tag or production deployment has occurred in this Intel work.

| Exact candidate | Intel prerequisite | Quiet seven-client result | Dependent gates |
| --- | --- | --- | --- |
|52bcfd3570b0a59076c225caad9157b0825954d8|Build+11focusedPASS|FAIL~124s; broadPOSIXtimeouts, client5gracefulstoprefused|Peer/cold/physicalNOTRUN|
|2bf7e257e2a36d7f8779b770c8eca3d967ab5835|ProductbuildPASS,10/11focusedPASS; operationextractormockcompileFAIL|NOTRUN|NOTRUN|
|def46109909c793e58ae2c453c5de537cdb2c627|Build+11focusedPASS|FAIL~119s; all7POSIXclientsfail, client2gracefulstoprefused|Peer/cold/physicalNOTRUN|
|6b0973036bcbea185c5e84496240acb089f85dac|PrivateproposedFIFOdrainfix; focusednormal/ASanUBSanPASS|NOTRUN; centralreview/refreeze required|NOTRUN|

All quiet fixtures use7clients/jobs16,4G,TCP/direct and unchanged30s/75s/180s budgets. Their exact23318regulartrackedinputs are verified before/after; matching six-tool identities, commands/environments and raw logs are retained. Private controllers complete idle with no remaining private processes or mounts. Every failed original run remains failed; no forced-discard teardown is treated as passing.

## Demonstrated fix and remaining cause

Successful LOOKUP/GETATTR route hints could describe the other metadata group while the client keys that hint by the returned inode's group.2bf7e257 corrects the successful hint only; error hints and authority remain unchanged. Its actual-source negative compiles but fails the routing assertion; corrected source passes. def46109 adapts the second extractor's missing host-view mocks. The quiet def result demonstrates that this narrow correction does not resolve overall acceptance.

Separate same-source diagnostics are not quiet acceptance.52 traces show no admitted-read deadline or publication-read timeout, but many transaction-cut refusals and absent new roots in published query catalogs. def receiver-phase diagnostics sample TCP_NODELAY=1 on private outbound/accepted sockets and senderRTTs~50–75us, while dualnodes show inboxages11–37ms and pumpdrains5–15ms average with occasional~400ms drain. Disk sync averages~3ms. These cumulative histograms motivate investigation of receiver scheduling; they do not establish per-request causality or justify stale read authority.

Private6b097303 takes at most8FIFO messages per pump, retains older tails ahead of arrivals, accounts only taken bytes/messages for the existing post-KV durability retirement and immediately polls when a queued tail remains. Decode/ACK/commit/apply/term/config/read authority code is unchanged. A single message/storageoperation can still block; no hard time bound is claimed. The actual-source scheduler test checks prefix size, FIFO arrivals, byte totals, no premature work-credit release, backlog wake and existing control/configuration fences. Removing the bound or restoring backlog sleep produces assertion failures. Central safety/native/live review is pending.

Global packed-empty assertions after failed POSIX runs are potentially confounded by deliberately retained live test trees. Raw FAIL remains FAIL. Separate isolated64-file physical reclamation is NUC's assigned gate, so this Intel failure alone does not demonstrate a GC defect.

Evidence checkpoints: e41ff7d6 (52quiet), b854b0c8 (52diagnostic),6304ba42 (2bfprerequisite),b2d310ea (defquiet+phase). See [52evidence](../../results/measure/20261012-intel-common52/README.md) and [defevidence](../../results/measure/20261012-intel-commondef/README.md). Historical stop checkpoint1cd9876b retains production identities/mounts/ports and512payloadsamples. No additional old-binary compatibility/restoration or inventory work is planned under the newer narrowed scope.
