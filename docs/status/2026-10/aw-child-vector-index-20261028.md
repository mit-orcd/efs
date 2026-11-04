# AW — child vectors: reverse slot→position index (Oct 28–29, 2026)

Branch `agent/devel-taskaw`, head `50218c827` (on devel `9c4bfd171`). Gate:
`make test-all` on intel1 = **TOTAL 547 FAIL 2 NOTRUN 12 (720s)**. The two
FAILs: (1) `test_b6_unavailable.py` slow mode — the pre-existing devel
defect recorded by AK (identical F0 fsync-EIO signature, write path,
unrelated); (2) `test_k41_packer_placement.py` — **timing-sensitive, not a
regression**: the untouched devel baseline passes it on this host (70.1 s)
and the branch build passes it in isolation (72.2 s), so the failure under
gate parallelism is an environment condition (packer/heartbeat timing), not
attributable to this change. The 12 NOTRUN are the usual host limits.
Defects found by this task: one **separate, pre-existing** mechanism
documented below (`recompute_times_up`, out of AW's scope); no defect in the
index itself.

## Verdict

**IMPLEMENTED — above the threshold.** Unlink in a loaded parent was O(D)
per operation and O(D²) for D removals, so the reverse slot→position index
was built as the brief specifies. The scan is gone (~6 map operations per
delete instead of ~D comparisons), but **end-to-end unlink latency is
unchanged** because a different, pre-existing O(D) mechanism —
`recompute_times_up`, not named in the brief — dominates the cost. That
finding is recorded for the coordinator below; it needs its own item.

## Measurement (in-process driver `scripts/evidence/aw/aw_measure.c` + TEMP counter patch)

Private in-memory export, one parent with D children, 200 unlinks/renames
per D on intel1 (production build with a temporary comparison counter in the
two scan functions):

| D | unlink comparisons/op (before → after) | unlink ns/op (before → after) | staged bytes/child (before → after) |
| --- | --- | --- | --- |
| 1,000 | 855 → 6 | 4,225 → 3,412 | 8 → 41 |
| 100,000 | 99,701 → 6 | 1,068,773 → 798,614 | 10 → 94 |
| 1,000,000 | 995,201 → 6 | 12,975,350 → 11,615,464 | 8 → 41 |

Same-parent rename is scan-free even before the change (0 comparisons — a
different code path). Index memory is ~16 B/child of additional map capacity
(charged to `staged_child_bytes`, same budget).

The end-to-end number barely moves because of the rollup rescan below.
`parent_tmin` convergence after deletions is verified against the true
bounds over the remaining children (`aw-measure verify`: parent_tmin =
true_tmin after 200 deletions).

## The dominant cost is elsewhere (finding for the coordinator)

`efs_export_unlink_name_ex` calls `rollup_sub_under` **before**
`child_idx_del` (src/meta/metadata.c:2645–2646). `rollup_sub_under` ends in
`recompute_times_up`, which rescans every child of the parent to recompute
the exact time rollups (narrowing case). perf on the driver at 1 M children:
~64–78 % of unlink time is this rescan — it, not `child_idx_del`, dominates
both before and after the index. The child-vector scan was roughly a third.

Two consequences:

- AW's index eliminates its named scan, but the per-unlink cost stays O(D)
  until the rollup rescan gets its own treatment. This is a separate
  mechanism with its own O(D²) shape for D removals; it deserves its own
  item (reverse-ordered structures or an exact skip analysis), not a silent
  expansion of AW.
- The stock rescan runs before the vector deletion, so a parent's time
  bounds are stale-by-one by design and converge on the next mutation. I
  tried an exact-skip fast path for it during this task and the driver's
  correctness check caught a stale bound it produced (parent_tmin=1 vs true
  2 after ordered deletes): the skip let a stale extreme persist instead of
  converging. **The fast path was reverted**; only the index is committed,
  and the convergence check passes.

## Implementation (`50218c827`)

`struct efs_child_vec` gains `pos_keys/pos_vals/pos_mask` (existing `idx_*`
helpers): a reverse slot→position map. `child_idx_del` looks the position
up, swap-removes, and re-records the moved last element's position;
`child_idx_replace_slot` updates the map in place; `child_idx_add` grows the
map before mutating (failure atomicity) and appends under it; rebuild and
free paths include the map. Memory is charged to `staged_child_bytes`.

## Gates

- New deterministic `tests/test_child_vec_index.c`: forced realloc and
  pos-map rehash, swap of a row from another parent, several aliases,
  repeated grow/delete cycles, allocation failure at every add-path
  allocation point (`--wrap realloc/calloc`), root handling — PASS.
- Named: `test_sparse_metadata.py`, `test_stage_evict`,
  `test_stage_evict_lru.py`, `test_rename_budget.py`, `test_rename_flags.py`,
  `test_unlink_verdict.py`, `test_unlink_authority_errno.py` — all PASS on
  intel1.
- ASan+UBSan with leak detection on the new test — PASS.
- `make test-all` on intel1: **TOTAL 547 FAIL 2 NOTRUN 12 (720s)** — the two
  FAILs are the pre-existing b6 defect and the timing-sensitive K41 case
  above (baseline and isolation both PASS). Raw:
  intel1 `~/efs-artifacts/taskAW-20261029/test-all-summary.txt`.

## Reproduction

```
patch -p1 < scripts/evidence/aw/aw-instrument.patch   # TEMP comparison counter only
make -j8 libefs.a
gcc -O2 -Iinclude -Isrc/common -Ideps/blake3 -o /tmp/aw_measure \
  scripts/evidence/aw/aw_measure.c libefs.a -lpthread -lm -ldl -libverbs
/tmp/aw_measure 1000000 200
make -j8 tests/test_child_vec_index && ./tests/test_child_vec_index
```

Restore `src/meta/metadata.c` afterwards for other builds. Raw artifacts:
intel1 `~/efs-artifacts/taskAW-20261029/` with `SHA256SUMS`.
