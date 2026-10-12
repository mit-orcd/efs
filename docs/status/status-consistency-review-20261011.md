# Active status consistency review — 2026-10-11

Scope: active index and handoff, release acceptance/operator notes, machine-readable reconciliation, central and owner stop checkpoints, and linked source-specific evidence. Reviewed current Git ancestry and retained PASS/FAIL/STOPPED/NOTRUN identities; no cluster tests, deployment or new qualification was performed. This is not a full revalidation of every historical W-item or raw payload.

## Corrected inconsistencies

| Finding | Correction |
| --- | --- |
| Index called31ab the frozen running source while the top said410/stopped | Current summary now pins410/v2 and actual stopped state; older narrative preserved in archive with explicit supersession. |
| CREATE hook fix described as a next-candidate proposal after410 already contained it | Marked integrated at410; strict mounted/cold ownership remains NOTRUN centrally. |
| JSON latest source/input/archive fields still named b239/1517, with d26 initial field | Historical snapshot retained; authoritative fields now410/v2/1563 and exact hashes. Current branch-tip ancestry audit stored separately from original frozen handoffs. |
| Release and in-flight pages still ordered agents to continue; ORCD live runtime claimed c52 despite later restored4fe | Added stopped-state precedence; old assignments/runtime/next steps explicitly historical. No new runtime observation inferred. |
| Initial bounded Mac and NUC NOTRUN/pending text conflicted with their later full native PASS/stop records | Owner status precedence names final checkpoint and distinguishes source/gate/phase. |
| Intel index called superseded0ae current, and repaired fixture still looked universally NOTRUN | Old narrative labeled historical; final410 owner evidence gap named explicitly, older repaired c2 gates not promoted to410. |
| Earlier task-right-now and latest reconciliation headers presented Oct7/8 counts as current | Renamed historical package/doc-review checkpoints; totals remain dated baselines, not live counts. |
| Central stop prohibited all merges although later human authorized all-branch reconciliation | Records specific subsequent merge/doc-review authorization while preserving stopped cluster execution. |
| Operator notes presented early AMD gate results as current limits | Added authoritative410 qualification/blocker summary; earlier gates labeled historical. |

## Remaining evidence gaps, not documentation closures

AMD c2 stats/find deadline FAIL persists; publication gaps do not prove the per-root cause. Mac c2 mounted17timeouts remains a blocker.410 fixed CREATE needs strict mounted/cold/dentry/ownership acceptance; its controlled client harness cannot prove live ownership. ORCD410 normal STOPPED and sanitizer/mounted NOTRUN, Mac410 native STOPPED and subsequent gates NOTRUN, NUC410 execution NOTRUN. Intel final410 stop checkpoint is newly committed on devel-intel at aaaeaa38 after the prior integration audit: both normal full Make suites PASS, additional standalone4/88 and6/88 PASS with remainder STOPPED/NOTRUN, fault/sanitizer/mounted NOTRUN. It is not yet integrated; no old-source pass transfers. Retained Mac orphan creating writer/coherent ownership and safe repair remain unproven. D36 is accepted design, not implementation of four-node/two-loss protection.

Evidence log bytes and original failures were not rewritten. Removed release-index chronology is preserved in [archive](../archive/v030-index-checkpoints-20261011.md). Documentation and JSON identities must distinguish the audited integration commit from the frozen executable source; no later documentation commit changes410 qualification identity.

## New post-audit Intel checkpoint

During review, `devel-intel` advanced to `aaaeaa38f77003865e17e5be34efc3e6a0ff9fe4` (documentation/raw evidence only). Read its `docs/status/intel-stop-checkpoint-20261011.md` directly from that Git commit:410/v2 both full normal Make suites PASS; additional standalone4/88 and6/88 PASS, remainder STOPPED/NOTRUN; fault, sanitizer and mounted gates NOTRUN. It reports both hosts idle and retained original production/payload samples protected. These are source-specific owner-reported checkpoint claims, not a new central runtime verification. This new commit is not yet integrated in the release branch; the prior complete-merge claim is explicitly limited to the15ff4de7 audit. No merge/deployment was performed during this documentation review.

## Validation

Generated architecture refreshed after index/link corrections. All five documentation gates PASS: regeneration, internal links, invariant references, operation-matrix sections and single normative home. Reconciliation JSON parses and its source410/v2,1,563-input count and stored branch ancestry booleans agree with the audited Git head. Source/build/test implementation and raw result bytes are unchanged.
