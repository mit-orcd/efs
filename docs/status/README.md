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
| Alpha 1.0 Stage 1 acceptance | Batch F on candidate `9bb50fe79` (Nov 9): F1 0 FAIL on a third gate run, F2 to F5 PASS; 18 K rows VERIFIED on `9bb50fe79` ([batch F](2026-11/batch-f-candidate-20261109.md)). Batch O on the same candidate (Nov 9–10, [Task EW](2026-11/orcd-mini-batch-o-20261109.md)): O1 FAIL (K38), K32 reopened, K102 new; O5/O6/O8/O9 PASS, O3 one criterion not met, O4 NOTRUN; K8, K26 and K51 VERIFIED on it. K38 fix merged in wave 70 ([Task FB](2026-11/k38-recurrence-20261110.md)); K102 fix merged in wave 71 ([Task FC](2026-11/k102-stuck-client-20261110.md)); K103 and K104 fixes merged in wave 72 ([Task FG](2026-11/k103-read-probe-20261110.md), [Task FF](2026-11/ao-halted-replica-stall-20261110.md)), K106 new and OPEN (waits a halted member still causes); K107 new and OPEN (group-view readers starve the publisher under TSan, blocks alpha: no); `9bb50fe79` is no longer the release candidate, batches F and O rerun on the fixed one; batches D and S in progress | [Stage 1 matrix](stage1-acceptance-matrix-20261030.md) |
| Alpha 1.0 known defects (section K) | OPEN; no release candidate or topology acceptance | [K register](alpha-1.0-2plus1-checklist.md) |
| W123 crash recovery | Not accepted; correctness gates passed, formal performance FAIL, v3 wait capture pending | [W123 current state](w123-current.md) |
| W171 less context per change | In progress; phase 3 splits of `efs_fuse.c` (Nov 4) and `meta_apply.c` (Nov 5) landed, measured reading sets not reduced; write-back ownership pilot (Task DM, Nov 6, merged in wave 60; its `ready` race fixed by Task DT, wave 61) cut lines needed 691 to 593 and stopped at the boundary; Task EB (merged in wave 64, Nov 8) repeated it on the open-reference table (640 to 543), added multi-tag test routes that refuse misspellings, and put the component guides and per-component nodes in the code graph; wave 65 (Nov 9) merged Task EC, which made the `.find` result cache private to `find_res.c` (542 to 428 lines needed) and fixed K101, Task ED, which checks guide counts in `make docs-check`, audited every catalogue tag and declared the `raft_host.c` prefix tags, Task DO, the `raft_host.c` ReadIndex entry map and before-measurement ([evidence](2026-11/readindex-entry-map-20261108.md)), and Task EE, the review of the Nov 9 feedback into briefs EF to EI ([review](2026-11/review-feedback-nov9-20261109.md)); wave 66 (Nov 9) merged Task EF, the graph's `tested_by` edges from `tests_for.py`'s resolver ([evidence](2026-11/graph-selector-resolver-20261109.md)), Task EH, one current-status block per active page, Task EI, two changes made by blind readers, the unmount report and the GC watermark double count, read with 8 to 10 source files against the implementers' 3 to 4 ([evidence](2026-11/blind-reader-20261109.md)), and Task EJ, the K54 fence test asserting what K54 forbids; wave 67 (Nov 9) merged Task EK (the k37 degraded-count `begin` keeps the latest apply's marks) and the eight small tasks EL to ES (`EFS_FUSE_PREFIX` alone drives the fuse3 build, the sequence binaries on the sanitizer lists, 40 budget rows refit down, the K101 pattern audit found no second case, stop files already cleaned by Task DI, every evidence script cited, two open.c splice tests migrated onto the interface test, host-limit NOTRUN rows marked in the gate summary); wave 68 (Nov 10) merged Task ET, the apply visibility contract between owners ([page](../how-it-works/apply-visibility.md)), Task EU, contract-to-case maps in the owner guides checked statically, and Task EV, the reading-set method reporting understanding, validation and total apart with necessary cross-owner reading counted as success; the split still waits on the W123 recovery merge | [Structure plan](2026-11/structure-plan-20261102.md), [Task ED](2026-11/navigation-leftovers-20261108.md) |

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
