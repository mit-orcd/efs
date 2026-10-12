# Approved merged NUC qualification (in progress)

Manager approved `b239dc7de104ff8437e49d9333a529790601464c` for exclusive isolated NUC W52, direct/buffered W112 and then controlled W97 comparison. No production rollout is authorized. Prior-source passes do not qualify this candidate.

## Verified input and running prerequisites

Complete archive `/tmp/efs-v030-b239dc7d-source.tar.gz` SHA256 `24f6ca3ec7d509a24c74de4b6a746d033bef872b7145eec9beb2e387a5caa548`. Input manifest SHA256 `1500601db39a388c5d033c1ab8c9c537e0d94d5fc2c1a2ef9ed8946da90696c7`; empty directory manifest SHA256 `911c85e28319837c62e30a5ab92fba394be649713ec3062b34442f58668197bb`. All 1,517 file hashes verified after remote extraction under `/data1/efs/nuc-b239-qualification/source`. Complete tracked results included; previous missing-doc-target packaging error is not carried forward.

Normal/fault full native controller PID154899 started. Both full normal and fault native suites exited0; overall native run0. All14 focused ASan/UBSan tests passed (build0). Both native candidate and baseline runs0. Five images saved separately per variant after acceptance. Focused ASan/UBSan follows both native modes. Mounted acceptance and W97 remain pending; native acceptance is source-specific.

Regular four f52 direct/TCP daemons still mapped original `/data1/efs/src` binaries at17432..35, original n1/n2/data1 and n3/n4/data2 placement. Stores and mounts remain untouched during builds. No new generator started. Before mounted gates, reverify all36 preserved payloads and five original executable identities; normal drain only. Restore original services and reverify after private gates; never replay new command logs with old images.

## Gates and flags

Use exact fault candidate images consistently for private server/client/mgmt, no mixed HELLO identities. FLOW0, EARLY_RETIRE0, GROUP_DRAIN0, overlap/noop0 (D25 disabled), TCP, both direct and buffered. Ordinary overwrite fixture explicitly PACKED_WRITES0; separate mounted packing fixture enables PACKED_WRITES1/COMMANDS1/SERVER1. Full W52 recipe and separate20k unique4KiB append/peer/cold/physical gate in each mode retain original30s RPC limits and1800/10800s outer budgets. Full POSIX/peer counts, all skips and failures retained. Full candidate acceptance NOTRUN at checkpoint.

## Controlled W97 baseline approved

Manager reviewed minimal inverse root-stop delta in local `qualification-b239/w97-baseline.patch`: remove only initialized-root termination from `sync_fragment_parents`, continue parent walk through `/`. Retain identical root lookup overhead; `packed_fence.h` unchanged and certificate NULL call full ancestry remains unchanged. Native witness success counts3→5, interrupt4→6; third-directory failure remains3. Source and native witness are the only differing tracked files. Baseline full normal/fault controller PID158713 passed both suites0; separate baseline ASan/UBSan durability witness passed0. Complete reviewed baseline archive SHA256 `c5ce2e079ef16a12e7520f2ef467f054586125bbea936a2dd1ca0a7691a2d96e`; full manifest SHA256 `2e81d131195cf2b5636894430db0fdd9663705ad209f9f99e8a7e782492783a8`. All1,517 remote source/archive hashes independently verified. No performance measurement yet.

After native/sanitizer/functional correctness acceptance, compare same merged source/scheduler/completion with only this reviewed delta. Three voters, fixed leaders, SATA metadata, SATA and NVMe data, base/root/root/base600s each, cold exact bytes and restoration. Report late throughput and change in fractional decline per repeat. This measures the root-stop optimization effect on b239, not all historical W97 fixes, drive cache behavior or hardware ceiling. W97 remains open until measured evidence supports a narrower verdict.

Input receipts and executed controllers: [qualification input](../../results/measure/20261012-nuc-b239-qualification/input/). Raw live files remain under `/data1/efs/nuc-b239-qualification`; gate logs will be retained after verdicts.

## Mounted progress and regression control input repair

Candidate live controller PID186416: both original mounts normally drained0, all original daemons drained; no original store opened by candidate. Direct ordinary overwrite and separate mounted packing mutation/cold/physical gates passed0. Full direct W52/POSIX recipe is active; separate direct20k, all buffered gates and W97 remain NOTRUN at this checkpoint.

The original whole-file ba73 write control did not compile against the newer merged fold harness (changed `dcache_flush_keep` signature and missing `dcache_pin_add`). This was a control-input failure, not an overwrite regression verdict. Initial sanitizer aggregate1/failure/log retained under `sanitizers/initial-control-input-failure`; all14 actual sanitizer test exits were0. A same-source negative control removes only the W112 seven-line full-overwrite guard from b239 write.c, SHA256 `ca7b2500b9cb03803ee111bda9faada6bfa6f004f74714668acccdf28713996b`. It compiled and failed the exact `sent[i]=='N'` byte assertion as expected; candidate normal and sanitizer fold tests passed. Revised aggregate0 explicitly records the repaired control input; no test skip or engine source change.
