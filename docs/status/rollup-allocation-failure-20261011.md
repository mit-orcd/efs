# Rollup queue allocation-failure fix

October 11, 2026. Release correctness checkpoint on `devel-code-review`, after report checkpoint `1b0c6b7c5f3c74c74c6b03764916008a14b9f8ae`. See [review release disposition](../reviews/release-disposition-20261011.md).

## Problem and change

`pending_rollup_note` grew three independently owned arrays using three reallocations. If one resize failed after another had moved, error cleanup freed replacement storage but left the export's owner pointers pointing to freed old storage. Subsequent reads or export teardown could use/free those dangling pointers.

Growth now allocates all replacement arrays, copies existing entries, and frees/replaces the old owners only after all allocations succeed. On failure it frees only the unpublished replacements and preserves pointers, count, capacity and entries. Growth overflow checks precede allocation. Queue semantics, rollup behavior, inode/native RPC layouts and publication APIs are unchanged.

During successful growth, old and new arrays coexist transiently. This trades realloc's possible in-place resize for ownership-safe failure behavior. It does not fix the separate finding that retained pending-array capacity is absent from the staged-memory estimate; cache-bound acceptance must address that separately.

## Regression and before/after evidence

`tests/test_rollup_allocation_failure.py` extracts the actual queue function and includes the real public export structure. The allocator shim forces each of the three allocation failures separately; against the old implementation successful reallocations deliberately move. It checks both an empty queue and a full 64-entry queue, retained entries, pointer/count/capacity preservation, safe teardown and successful retry after each failure, duplicate-entry coalescing and reuse without allocation after clearing the count. It is wired into `make test` and `make test-client-memory`.

Before modification, `python3 tests/test_rollup_allocation_failure.py --sanitize` failed with **AddressSanitizer: heap-use-after-free** in the preserved-entry check after growth failure. The prior archived narrower OOM probe instead catches the same ownership defect at cleanup as a double-free. These are deterministic fault injections, not evidence that a particular production crash used this path. macOS's external ASan symbolizer emitted warnings, but the sanitizer report identified the invalid access and allocation/free stacks.

After modification, the regression passes normally and under AddressSanitizer/UndefinedBehaviorSanitizer. Each failure now preserves owner storage; cleanup/retry are safe.

## Focused gates passed

- Full `metadata.c` syntax check and `test_stage_evict` linked with production metadata/common implementations.
- Archived create/link/deferred size+mtime/cross-directory rename/unlink probe under ASan/UBSan.
- Archived 20,000 seeded mutations under ASan/UBSan: digest `afdaf51ba528758e` matches the baseline.
- `test_client_memory.py` (including read pressure), `test_metadata_balance.py`, `test_read_failure_admission.py`, `test_fold_observation.py`.
- Documentation architecture gate and whitespace checks.

The old function is byte-identical in the inspected commits `a48076c4e9373ea47c143c231e2b3cdb3c1e20fe` (ORCD), `1f1037a6aa001546982defcb4e5e3997d694076f` (NUC), and `b2c01fa05afa1287cd358f352ebfbc831b570e31` (MAC). Temporary copies with only this function replaced passed the allocation-failure sanitizer regression. That establishes the focused function's compatibility with those snapshots, **not** full newer-branch builds or acceptance. No other branch/worktree was modified.

## Remaining release acceptance

Coordinator must run Linux builds, merged acceptance and mounted create/link/rename/unlink/hardlink tests with the current release source. Recheck intervening changes when integrating. Separate findings remain open: pending queue capacity accounting, recorder successful-reap shutdown behavior, and query compatibility gates. No broad rollup removal, wire shrink, test-data pruning, main merge or push is included.
