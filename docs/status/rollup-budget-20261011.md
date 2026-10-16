# Deferred rollup capacity accounting and growth admission

October 11, 2026. Focused follow-up to [allocation-failure ownership repair](rollup-allocation-failure-20261011.md) on `devel-code-review`. Production APIs and native/wire layouts are unchanged.

## Behavior

Staged accounting now includes all retained pending-array capacity at 17 payload bytes per slot and refreshes the root running total after growth, including owned shard tables. Clearing pending entries preserves that charge while arrays remain allocated. Compaction frees empty queues and removes their charge; live pending entries remain intact.

Queue growth checks the same `EFS_CLIENT_META_MB` target as the evictor. Its environment parsing/default is moved unchanged into a small internal header shared by these two sites, preventing divergent limits. Before allocation, admission includes the entire replacement payload while old arrays still exist. Exact-fit replacement allocation is accepted; one byte short or already over target drains queued rollups and applies the current update eagerly instead of allocating. The write/mtime update still succeeds and statistics remain correct.

This is a bound on **optional rollup growth**, not a new hard global cache/RSS promise. Other staging structures and pinned work retain existing eviction semantics, which explicitly allow an over-target table when nothing is evictable. The estimate counts payload, not allocator overhead. No wire fields, public function signatures or pool limits change.

## Focused evidence

`tests/test_rollup_budget.py` compiles the actual metadata implementation and exercises root and owned-table accounting; first 64 slots add 1,088 bytes; retained capacity survives drain/reuse; compaction frees empty arrays but preserves live queues. Synthetic accounted slab payload places the table at exact and one-byte-short boundaries for initial allocation and 64-to-128 growth, including the old/new overlap. It verifies eager fallback, parent tree-byte totals and immediate counts, then tests above-target updates. The boundary payload is simulated accounting, not an RSS measurement. Both ordinary and ASan/UBSan runs pass.

The allocation-failure regression remains passing under ASan/UBSan after adding the accounting/admission hooks. Production-linked `test_stage_evict` and metadata/evictor syntax checks pass. Documentation/whitespace gates pass. Full merged/Linux/live acceptance remains with release coordination.

## Integration boundary

Rechecked ORCD `8d07b45adac9c6aa24ba46f7e8e1d4cc163f4da5`, NUC `3538213cfe793dbdb9025c3e0f65cc96eb97149a` and MAC `b2c01fa05afa1287cd358f352ebfbc831b570e31`: the old queue function, staged estimator and evictor target parser are byte-identical to the inspected original baseline. These queue/staging APIs remain separate from server query catalogs. This patch touches local cache accounting and optional deferred work only. Apply after the allocation ownership fix; recheck later branch changes before integration.
