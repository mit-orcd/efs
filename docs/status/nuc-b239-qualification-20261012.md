# Approved merged NUC qualification (in progress)

Manager approved `b239dc7de104ff8437e49d9333a529790601464c` for exclusive isolated NUC W52, direct/buffered W112 and then controlled W97 comparison. No production rollout is authorized. Prior-source passes do not qualify this candidate.

## Verified input and running prerequisites

Complete archive `/tmp/efs-v030-b239dc7d-source.tar.gz` SHA256 `24f6ca3ec7d509a24c74de4b6a746d033bef872b7145eec9beb2e387a5caa548`. Input manifest SHA256 `1500601db39a388c5d033c1ab8c9c537e0d94d5fc2c1a2ef9ed8946da90696c7`; empty directory manifest SHA256 `911c85e28319837c62e30a5ab92fba394be649713ec3062b34442f58668197bb`. All 1,517 file hashes verified after remote extraction under `/data1/efs/nuc-b239-qualification/source`. Complete tracked results included; previous missing-doc-target packaging error is not carried forward.

Normal/fault full native controller PID154899 started. Both full normal and fault native suites exited0; overall native run0. All14 focused ASan/UBSan tests passed (build0). Both native candidate and baseline runs0. Five images saved separately per variant after acceptance. Focused ASan/UBSan follows both native modes. Mounted acceptance and W97 remain pending; native acceptance is source-specific.

Regular four f52 direct/TCP daemons still mapped original `/data1/efs/src` binaries at17432..35, original n1/n2/data1 and n3/n4/data2 placement. Stores and mounts remain untouched during builds. No new generator started. Before mounted gates, reverify all36 preserved payloads and five original executable identities; normal drain only. Restore original services and reverify after private gates; never replay new command logs with old images.

## Gates and flags

Use exact fault candidate images consistently for private server/client/mgmt, no mixed HELLO identities. FLOW0, EARLY_RETIRE0, GROUP_DRAIN0, overlap/noop0 (D25 disabled), TCP, both direct and buffered.  Full W52 recipe and separate20k unique4KiB append/peer/cold/physical gate in each mode retain original30s RPC limits and1800/10800s outer budgets. Full POSIX/peer counts, all skips and failures retained. Full candidate acceptance NOTRUN at checkpoint.

## Controlled W97 baseline approved

Manager reviewed minimal inverse root-stop delta in local `qualification-b239/w97-baseline.patch`: remove only initialized-root termination from `sync_fragment_parents`, continue parent walk through `/`. Retain identical root lookup overhead; fence header unchanged and certificate NULL call full ancestry remains unchanged. Native witness success counts3→5, interrupt4→6; third-directory failure remains3. Source and native witness are the only differing tracked files. Baseline full normal/fault controller PID158713 passed both suites0; separate baseline ASan/UBSan durability witness passed0. Complete reviewed baseline archive SHA256 `c5ce2e079ef16a12e7520f2ef467f054586125bbea936a2dd1ca0a7691a2d96e`; full manifest SHA256 `2e81d131195cf2b5636894430db0fdd9663705ad209f9f99e8a7e782492783a8`. All1,517 remote source/archive hashes independently verified. No performance measurement yet.

After native/sanitizer/functional correctness acceptance, compare same merged source/scheduler/completion with only this reviewed delta. Three voters, fixed leaders, SATA metadata, SATA and NVMe data, base/root/root/base600s each, cold exact bytes and restoration. Report late throughput and change in fractional decline per repeat. This measures the root-stop optimization effect on b239, not all historical W97 fixes, drive cache behavior or hardware ceiling. W97 remains open until measured evidence supports a narrower verdict.

Input receipts and executed controllers: [qualification input](../../results/measure/20261012-nuc-b239-qualification/input/). Raw live files remain under `/data1/efs/nuc-b239-qualification`; gate logs will be retained after verdicts.

## Mounted progress and regression control input repair

Candidate live controller PID186416: both original mounts normally drained0, all original daemons drained; no original store opened by candidate. Direct ordinary overwrite gates passed0. Full direct W52/POSIX recipe is active; separate direct20k, all buffered gates and W97 remain NOTRUN at this checkpoint.

The original whole-file ba73 write control did not compile against the newer merged fold harness (changed `dcache_flush_keep` signature and missing `dcache_pin_add`). This was a control-input failure, not an overwrite regression verdict. Initial sanitizer aggregate1/failure/log retained under `sanitizers/initial-control-input-failure`; all14 actual sanitizer test exits were0. A same-source negative control removes only the W112 seven-line full-overwrite guard from b239 write.c, SHA256 `ca7b2500b9cb03803ee111bda9faada6bfa6f004f74714668acccdf28713996b`. It compiled and failed the exact `sent[i]=='N'` byte assertion as expected; candidate normal and sanitizer fold tests passed. Revised aggregate0 explicitly records the repaired control input; no test skip or engine source change.

## Current work-item numbering and measurement limitation

The approved merged backlog splits historical W97 persistence slowdown into W111. Current W97 is receiver feedback/replication queue pressure. The reviewed root-stop baseline with FLOW0 on both sides measures the persistence/root-sync change (W111 and historical W97 context), not the current feedback feature. Manager coordination requested a separate same-source FLOW0/FLOW1 comparison or its preferred pressure gate before making a W97 feedback benefit claim. Native and W52/W112 acceptance continue independently. No measured slowdown reduction is established at this checkpoint.

## Release-only scope hold and direct correctness result

Verified the human manager-thread instruction: “we only need to do testing and fix true defects, no agent needs to work on optimiations.” Both root-stop/W111 and FLOW0/FLOW1 W97 performance comparisons are HOLD/NOTRUN; neither started. No optimization or benchmark controller is active. Staged inputs are preserved, not executed. Do not imply a measured W97 slowdown benefit. Continue approved original-budget correctness/fault/cold/physical gates only; AMD remains central editor and there is no production rollout.

Full direct recipe exited0 at01:42:54UTC. POSIX216/216,0fail/0skip/0notrun (50.9s); peer67/67 (35.8s). Two-thousand unique4KiB records (8,192,000bytes), successful final fsync0.422231756s, exact warm primary/peer and cold bodies, physical deletion/GC/quota zero passed. Synchronous O_SYNC/O_DSYNC failures, retained-body stop/recovery, append lost replies/ACK retirement, shutdown mutations and repaired WAL tails all passed. Raw full direct evidence root `/data1/efs/gc-direct-xaotwh8t`, retained local in `live-direct/full`; binary store files excluded from copies and remain on NUC.

Separate20k direct gate began01:42:54UTC, private evidence root `/data1/efs/gc-direct-a1kmj9hw`. It is active, not accepted. Buffered ordinary/full/20k remain NOTRUN. Original f52 source/stores remain untouched and normally stopped during private fixtures; final restoration must pass actual four daemon/two client mappings, five installed executable hashes and all36 payloads.

Additional actual host-flow Linux async sender and append receipt ASan/UBSan gates passed0 (`flow-sanitizers`). This is correctness evidence, not current W97 pressure acceptance or performance measurement. No new demonstrated NUC defect at this checkpoint.

## Superseded candidate stopped safely; original service restored

Manager required all five chats to switch to one defect-only common candidate. Held the waiting old controller186416 (producer196853 continued initially), then paused the producer, fsynced its completed bytes successfully0.513111835s, verified warm primary/peer, quiesced both clients normally, interrupted the producer and cold restarted the same private images. Exactly2,409 unique4KiB records (9,867,264bytes) match on both fresh cold mounts, SHA256 `43646503e86542243958ebe1391e2a5798a77800eb7585d50a926531be6d3201`. Normal deletion/GC/actual physical emptiness passed0. This is an orderly partial-stop witness; the20k gate is STOPPED, not PASS. Buffered b239 gates never launched. No pending accepted bytes discarded; no regular store opened by candidate.

The original restoration helper failed after eight retries. Original f52 startup refused187.50GiB quota when available safe maximum fell to187.09–187.11GiB. A bounded5s startup diagnostic showed namespace usage recount; the actual refusal was capacity protection, not a demonstrated engine hang. Reclaimed only rebuildable compiled outputs from three own b239 stages (982,840KiB) and two known never-installed101 stages (374,936KiB), keeping source/bundles/raw/accepted image copies/original binaries/stores/quota. Original f52 idempotent retry passed; actual n2 safe maximum188.22GiB gives0.72GiB quota margin. Four actual ports, four mapped daemons/two mapped clients, five installed hashes and36 payloads pass in `restoration-retry-proof.json`. Retain the initial restoration failure separately from successful retry.

All later candidate launch plans31ab/b3 are superseded without execution. New sole common c2 qualification follows its own ledger; no old-source pass is inherited. Both optimization comparisons remain HOLD/NOTRUN.
