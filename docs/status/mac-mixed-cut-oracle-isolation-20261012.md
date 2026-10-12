# Mixed-cut oracle isolation for next merged candidate

Manager-authorized test-only patch against exact frozen b239dc7de104ff8437e49d9333a529790601464c. Frozen release worktree and executing qualification inputs were untouched. AMD remains sole central editor.

Patch: evidence/mac-mixed-cut-oracle-b239dc7d.patch. It moves the existing scheduler_oracle call after the existing mixed-cut and ordinary-write safety checks. No assertions, fixtures, model windows, limits or production code changed. This branch lacks the release-only capture helper/test, so the patch is carried as an exact reviewed artifact for AMD to apply on its next candidate, rather than importing incompatible release code into devel-mac.

Exact b239 archive plus patch: safe test normal and ASan/UBSan PASS. Removed-transaction-fence mutant now compiles and FAILS at the intended actual host_query_source_once mixed-image BUSY assertion, before scheduler execution. Baseline-cut-backoff still compiles and FAILS its quiet-window assertion after safety passes. Unsafe-all-BUSY retry still compiles and FAILS terminal authority classification after safety/quiet-window pass. All three negatives fail at their intended assertions in both normal and sanitizer builds; not compiler failures. Eight executions total: two positive PASS, six expected negative FAIL. Evidence: evidence/mac-mixed-cut-order-b239dc7d.txt.

Acceptance scope remains test-only. No original-budget mounted acceptance or performance result claimed. Outstanding blockers: frozen candidate full prerequisites and quiet pressure/cold gates, real steady-state proof traffic, root publication liveness, and distinct foreground case-phase cause. Manager/AMD own those runs; no new deployment or alternate production implementation here.
