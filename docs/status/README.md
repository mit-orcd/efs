# Project status — work queue and evidence

Layout: dated pages (`*-YYYYMMDD[-suffix].md`) live in `YYYY-MM/` directories
(`scripts/status_layout.py` files a new top-level one); undated pages and the
living documents stay here, including the dated
[stage1-acceptance-matrix-20261030.md](stage1-acceptance-matrix-20261030.md).

## Start here

| Need | Read |
| --- | --- |
| Ordered work and unresolved source findings | [Work queue](work-queue.md), preserving existing order and recorded verdicts |
| Execution handoff and scope caveats | [Handoff](in-flight.md) |
| Accepted architecture and pending decisions | [Decision register D1–D37](decisions.md), [specification/public-path review](spec-implementation.md) |
| Find a feature, platform or dated test report | [Complete topic catalog](catalog.md) |
| Earlier assignments, chronology and superseded captures | [Status archive](archive/README.md), [project archive](../archive/README.md) |
| Dated wave and task entries, newest first | [November ledger](2026-11/ledger.md), [October ledger](2026-10/ledger.md) |

## Current

One row per active effort; the linked page states the exact source, verdict, remaining failures, active run and next action.

| Effort | Verdict | State page |
| --- | --- | --- |
| Alpha 1.0 Stage 1 acceptance | Planning only; no PASS/FAIL recorded on the matrix | [Stage 1 matrix](stage1-acceptance-matrix-20261030.md) |
| Alpha 1.0 known defects (section K) | OPEN; no release candidate or topology acceptance | [K register](alpha-1.0-2plus1-checklist.md) |
| W123 crash recovery | Not accepted; correctness gates passed, formal performance FAIL, v3 wait capture pending | [W123 current state](w123-current.md) |
| W171 less context per change | In progress; phase 3 splits of `efs_fuse.c` (Nov 4) and `meta_apply.c` (Nov 5) landed, measured reading sets not reduced; write-back ownership pilot (Task DM, Nov 6, merged in wave 60; its `ready` race fixed by Task DT, wave 61) cut lines needed 691 to 593 and stopped at the boundary; Task EB (Nov 8, unmerged) repeated it on the open-reference table (640 to 543), added multi-tag test routes that refuse misspellings, and put the component guides and per-component nodes in the code graph; Task EE (Nov 9) reviewed the Nov 9 feedback into four briefs EF to EI ([review](2026-11/review-feedback-nov9-20261109.md)) | [Structure plan](2026-11/structure-plan-20261102.md) |

## Maintaining this index

Keep this page short. Wave and task entries go to the current month's ledger ([2026-11](2026-11/ledger.md)), appended directly under its `## Recent evidence and remaining acceptance` heading, newest first. Never add them here. The Current table changes only when an effort starts, changes verdict or closes; the linked page carries the detail.

Put exact source/build, feature flags, environment, commands,
PASS/FAIL/NOTRUN, limitations and remaining gates in the evidence page, following
[the project evidence rule](../../AGENTS.md#evidence-and-reproducibility).
Raw captures stay outside Git; existing evidence is not deleted before an
adequate summary and reproduction instructions exist.

Add each new finding under an unused W identity in the [queue](work-queue.md),
with class, state, evidence, next action and acceptance gate. Check the queue,
[work-item details](../backlog/work-items.md) and decision register for collisions.
Keep supporting pages discoverable in the [catalog](catalog.md). Accepted design
is not completed implementation; close only against the named acceptance gates.

Archive only superseded records, preserve their findings and source-specific
verdicts, and update incoming links. Archiving does not close an item, cancel a
gate or authorize deployment. Keep unresolved work linked from its queue/ledger.
