# EFS release coordination — 2026-10-11

Manager: efs-coding 6 - mgr, explicitly assigned by Mike. Human instruction: document current owner state first, reconcile all five coding branches, then dispatch stability work. No main promotion or push is authorized at this stage.

## Coordination boundaries

AMD remains sole release-worktree editor and shared AMD qualification executor. The manager owns the cross-chat inventory, assignments, review and acceptance ledger. Mac, NUC, ORCD and Intel retain their isolated branches and existing cluster ownership. Preserve running jobs; documentation checkpoints must describe active jobs immediately rather than wait for long gates. No overlapping deployment or shared-worktree editing.

Every owner handoff must name exact branch/worktree/HEAD, ordered committed changes, dirty work, deployed source/binary/config identity, running jobs, PASS/FAIL/NOTRUN evidence, release blockers, wire/durable allocations and subagent status. A checkpoint is an implementation/evidence input, not release acceptance.

## Inventory at initial handoff

| Chat | Frozen known source/evidence | State |
|---|---|---|
| efs-coding 1 - mac | 51b67e96; implementation e798c2aa | Clean; native POSIX 216/216 reported, broader query freshness open. Missing f842cd37 client adoption prerequisite requires reconciliation. |
| efs-coding 2 - amd | handoff f1babb1c; implementation d26e6874 | Sole release editor; isolated full native/sanitizer/ordinary/packing sequence active. |
| efs-coding 3 - nuc | 101598d2 | Documentation handoff requested; existing 20k write verification may finish safely; no new deployment. |
| efs-coding 4 - orcd | 83cff804 | Documentation handoff requested; active exact-source builds preserved; mounted failures remain open. |
| efs-coding 5 - intel | 60c86d88 | Documentation handoff requested; fresh fault matrix active; existing results do not certify new merged source. |

Known before final reconciliation: AMD and Intel tips are release ancestors. Mac has one evidence-only commit outstanding; NUC six commits; ORCD two commits; review six cherry-picked equivalents. Exact per-commit reconciliation must distinguish true ancestry from equivalent copied implementation and preserve all fixture/docs/evidence inputs. The companion JSON captures an audit snapshot; owner handoffs supersede moving tips.

## Stability assignments and sequence

1. Mac, after its committed handoff: prepare a bounded reconciliation of missing FUSE combined CREATE/OPEN adoption and lifetime/ambiguity/failure cleanup regressions. Classify whether required or optional; no release editing or deployment.
2. AMD, after all frozen handoffs: merge/reconcile exact submitted checkpoints, record integrated/equivalent/held disposition per commit, preserve the active remote candidate, then freeze a new candidate and run full normal/fault native, sanitizer and both-mode ordinary overwrite/packed lifecycle qualification.
3. ORCD, after handoff/reconciliation: diagnose reproducible mounted pressure failures against the manager-approved merged source, preserving original budgets, durability and ownership. Submit minimal fixes with matched negative controls; coordinate with Mac query owner.
4. NUC, after handoff/reconciliation: close exact merged-source append/catchup, 20k, primary/peer/cold, both-mode ordinary overwrite and physical cleanup gates on its owned cluster. Coordinate deployment explicitly with the manager; preserve retained stores/writers.
5. Intel, after handoff/reconciliation: finish the exact-source fault/cold/canonical matrix and compare required guards with the merged candidate; no claimed promotion from prior revisions. Report remaining gaps and safe qualification recipe before deployment.

Assignments 3–5 are planned until explicitly dispatched after their committed handoffs. The manager independently reviews concrete fixes; optional features/refactors remain deferred. Durable tags 45–54 and authority transport IDs 143/144 retain current allocations; verify canonical namespace and compatible voters before durable admission.

## Acceptance

Prior W112 reproduced byte-loss is repaired and both-mode exact ordinary overwrite/cold/cleanup qualification is reported by AMD; new merged candidates must retain those gates. Packing pressure remains an open blocker and buffered success on an earlier revision is not certification of the latest candidate. Final native, live cold-byte, physical-GC, resource accounting and fault gates must name one exact source/configuration. Until those pass and Mike authorizes promotion, main remains 53be1da7 and no tag or push occurs.
